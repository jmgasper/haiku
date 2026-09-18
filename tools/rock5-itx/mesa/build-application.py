#!/usr/bin/env python3
"""Build the unchanged GLTeapot application and its isolated trial package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
WORK = Path('/mnt/HaikuWork')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-build', type=Path, required=True)
    parser.add_argument('--glu-archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not WORK.is_mount() or os.environ.get('HAIKU_WORK') != str(WORK):
        parser.error('Source tools/rock5-itx/env.sh with /mnt/HaikuWork mounted')
    root = args.output.resolve()
    parent = args.base_build.resolve()
    archive = args.glu_archive.resolve()
    if root.exists() or root == WORK or not root.is_relative_to(WORK):
        parser.error('--output must be a new directory under /mnt/HaikuWork')
    if not parent.is_relative_to(WORK) or not archive.is_relative_to(WORK):
        parser.error('Build and archive inputs must be under /mnt/HaikuWork')
    pins = json.loads((HERE / 'application-sources.json').read_text())
    base_inputs = json.loads((parent / 'inputs.json').read_text())
    current = json.loads((HERE / 'sources.json').read_text())
    base = json.loads((parent / 'package/manifest.json').read_text())
    assert json.loads((parent / 'result.json').read_text())['status'] == 'built'
    for field in ('mesa', 'libglvnd', 'native_abi', 'host_helpers', 'mesa_options',
            'sdk_packages', 'sdk_haiku_revision', 'haikuports_revision'):
        assert current[field] == base_inputs['pins'][field], field
    for name, expected in base_inputs['pins']['bundle_files'].items():
        # This extension adds a launcher to capture orchestration only.
        # Every input to the existing Mesa/library/probe build remains exact.
        if name != 'graphics_capture.py':
            assert digest(HERE / name) == expected, name
    for item in base['assets']:
        assert digest(item['source']) == item['sha256']
        assert Path(item['source']).stat().st_size == item['bytes']
    for name, expected in pins['bundle_files'].items():
        assert digest(HERE / name) == expected, name
    for name, expected in pins['application_files'].items():
        assert digest(REPO / name) == expected, name
    assert archive.stat().st_size == pins['glu']['bytes']
    assert digest(archive) == pins['glu']['sha256']
    source_revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'],
        cwd=REPO, text=True).strip()
    for name in list(pins['application_files']) + [
            'tools/rock5-itx/mesa/' + n for n in pins['bundle_files']]:
        raw = subprocess.check_output(['git', 'show', source_revision + ':' + name], cwd=REPO)
        assert hashlib.sha256(raw).hexdigest() == digest(REPO / name), name
    os.umask(0o077)
    root.mkdir(parents=True)
    commands = []
    environment = dict(os.environ)
    environment['PATH'] = ':'.join(str(WORK / x) for x in (
        'toolchains/mesa-python/bin', 'toolchains/mesa-host/bin',
        'toolchains/host/usr/bin')) + ':' + environment['PATH']
    environment['LD_LIBRARY_PATH'] = ':'.join(str(WORK / x) for x in (
        'toolchains/mesa-native-deps/usr/lib/x86_64-linux-gnu',
        'toolchains/mesa-native-deps/usr/lib/llvm-18/lib'))
    environment.pop('PKG_CONFIG_PATH', None)

    def run(command, name, env=None):
        command = list(map(str, command))
        log = root / (name + '.log')
        print(name, flush=True)
        with log.open('w') as output:
            result = subprocess.run(command, env=environment if env is None else env,
                stdout=output, stderr=subprocess.STDOUT)
        commands.append(dict(command=command, exit=result.returncode, log=str(log)))
        save(root / 'commands.json', commands)
        result.check_returncode()

    try:
        sdk = parent / 'sysroot'
        system = sdk / 'boot/system'
        headers = system / 'develop/headers'
        compiler = str(WORK / 'build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-')
        cross = root / 'haiku-aarch64.ini'
        shutil.copy2(parent / cross.name, cross)
        with tarfile.open(archive) as source:
            source.extractall(root, filter='data')
        glu_source = root / pins['glu']['source_dir']
        glu_build = root / 'glu-build'
        glu_install = root / 'glu-install'
        environment['PKG_CONFIG_LIBDIR'] = ':'.join(str(system / x)
            for x in ('develop/lib/pkgconfig', 'lib/pkgconfig'))
        environment['PKG_CONFIG_SYSROOT_DIR'] = str(sdk)
        meson = WORK / 'toolchains/mesa-python/bin/meson'
        ninja = WORK / 'toolchains/host/usr/bin/ninja'
        run([meson, 'setup', glu_build, glu_source, '--cross-file=' + str(cross),
            '--buildtype=debugoptimized', '--prefix=/boot/system', '--libdir=lib',
            '--includedir=develop/headers/os/opengl', '--wrap-mode=nofallback',
            '-Ddefault_library=shared', '-Dgl_provider=glvnd'], 'glu-configure')
        run([ninja, '-C', glu_build, '-j' + os.environ.get('HAIKU_JOBS', '8')], 'glu-compile')
        run([meson, 'install', '-C', glu_build, '--no-rebuild'], 'glu-install',
            dict(environment, DESTDIR=str(glu_install)))
        glu_system = glu_install / 'boot/system'
        game = WORK / 'build/arm64/objects/haiku/arm64/release/kits/game/libgame.so'
        media = WORK / 'build/arm64/objects/haiku/arm64/release/kits/media/libmedia.so'
        app = REPO / 'src/apps/glteapot'
        teapot = root / 'GLTeapot'
        run([compiler + 'g++', '--sysroot=' + str(sdk), '-std=gnu++17', '-O2',
            '-g', '-Wall', '-Wextra', '-I' + str(headers / 'os/opengl'),
            '-I' + str(glu_system / 'develop/headers/os/opengl'),
            *[app / name for name in ('FPS.cpp', 'GLObject.cpp', 'ObjectView.cpp',
                'error.cpp', 'TeapotWindow.cpp', 'TeapotApp.cpp')],
            '-L' + str(game.parent), '-L' + str(parent / 'glvnd-install/boot/system/lib'),
            '-L' + str(glu_system / 'lib'), '-Wl,-rpath-link,' + str(system / 'lib'),
            '-lbe', '-lgame', '-llocalestub', '-lsupc++', '-lGLU', '-lGL',
            '-o', teapot], 'teapot-compile')
        host_tools = WORK / 'build/arm64/objects/linux/x86_64/release/tools'
        run([host_tools / 'rc/rc', '-o', root / 'GLTeapot.rsrc', app / 'GLTeapot.rdef'],
            'teapot-resources')
        run([host_tools / 'xres', '-o', teapot, root / 'GLTeapot.rsrc'], 'teapot-embed')
        probe = root / 'rock5_haiku_application_probe'
        frames = root / 'rock5_application_frames'
        # This cross compiler's LIBGCC_SPEC is unconditionally -lgcc, even with
        # -shared-libgcc. Use the SDK's shared unwinder for C++ cleanup frames.
        for source, target, libraries in [('application-probe.cpp', probe, ['-lbe', '-lgcc_s']),
                ('application-frames.cpp', frames, [])]:
            run([compiler + 'g++', '--sysroot=' + str(sdk), '-std=gnu++17', '-O2',
                '-g', '-Wall', '-Wextra', '-Werror',
                '-I' + str(REPO / 'src/add-ons/kernel/drivers/graphics/mali_csf'),
                HERE / source, *libraries, '-o', target], source + '-compile')
        symbols = subprocess.check_output([compiler + 'readelf', '-Ws', probe], text=True)
        dynamic = subprocess.check_output([compiler + 'readelf', '-d', probe], text=True)
        unwind = [line for line in symbols.splitlines() if '_Unwind_' in line]
        assert unwind and all(' UND ' in line for line in unwind), unwind
        assert any('_Unwind_Resume@GCC_3.0' in line for line in unwind)
        assert 'Shared library: [libgcc_s.so.1]' in dynamic
        save(root / 'controller-unwinder.json', dict(status='shared_unwinder_linked',
            symbols=unwind, runtime_executed=False))
        package = root / 'package'
        package.mkdir()
        assets = [dict(item) for item in base['assets']]
        for name, source, mode, strip in [
                ('GLTeapot', teapot, '755', False),
                ('rock5_haiku_application_probe', probe, '755', True),
                ('rock5_application_frames', frames, '755', True),
                ('lib/libGLU.so.1', glu_system / 'lib/libGLU.so.1.3.1', '755', True),
                ('lib/libgame.so', game, '755', True),
                ('lib/libmedia.so', media, '755', True),
                ('run-application', HERE / 'run-application', '755', False),
                ('run-system', HERE / 'run-system', '755', False),
                ('licenses/glu-9.0.3.tar.xz', archive, '644', False)]:
            target = package / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            if strip:
                run([compiler + 'strip', '--strip-unneeded', target], 'strip-' + target.name)
            item = dict(source=str(target), guest='/boot/home/mesa-trial/' + name,
                sha256=digest(target), bytes=target.stat().st_size, mode=mode)
            if source in (teapot, probe, frames, game, media) or name == 'lib/libGLU.so.1':
                item.update(unstripped_source=str(source), unstripped_sha256=digest(source))
            assets.append(item)
        assert len({a['guest'] for a in assets}) == len(assets)
        # Resolve every DT_NEEDED edge using the actual private package and
        # normal image/SDK libraries. This is separate from runtime validation.
        runtime = WORK / 'build/arm64/objects/haiku/arm64'
        contents = runtime / 'packaging/packages_build/minimum/hpkg_-haiku.hpkg/contents/lib'
        icu = WORK / 'build/arm64/build_packages/icu74-74.1_bootstrap-1-arm64/lib'
        libraries = {}
        for directory in (system / 'lib', contents, icu):
            libraries.update({p.name: p for p in directory.glob('*.so*') if p.is_file()})
        libraries.update({Path(a['guest']).name: Path(a['source']) for a in assets
            if '/lib/' in a['guest']})
        pending = [(p.name, p) for p in (package / 'GLTeapot',
            package / probe.name, package / frames.name)]
        visited, missing, records = set(), set(), []
        while pending:
            name, path = pending.pop(0)
            if name in visited:
                continue
            visited.add(name)
            dynamic = subprocess.check_output([compiler + 'readelf', '-d', path], text=True)
            needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)
            search = re.findall(r'\((?:RPATH|RUNPATH)\).*\[(.*?)\]', dynamic)
            assert not any('/mnt/' in value for value in search), (name, search)
            raw = path.read_bytes()
            assert raw[:6] == b'\x7fELF\x02\x01' and int.from_bytes(raw[18:20], 'little') == 183
            records.append(dict(name=name, source=str(path), resolved_source=str(path.resolve()),
                bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest(), needed=needed,
                soname=re.findall(r'\(SONAME\).*\[(.*?)\]', dynamic), search=search))
            for dependency in needed:
                if dependency in libraries:
                    pending.append((dependency, libraries[dependency]))
                else:
                    missing.add(dependency)
        save(root / 'dependency-closure.json', dict(status='pass' if not missing else 'failed',
            records=records, missing=sorted(missing), runtime_executed=False))
        assert not missing, missing
        save(root / 'base-manifest.json', base)
        save(root / 'inputs.json', dict(pins=current, application_pins=pins,
            source_revision=source_revision, base_build=str(parent),
            base_manifest_sha256=digest(parent / 'package/manifest.json'),
            sdk=str(sdk), cross_sha256=digest(cross), recipe_sha256=digest(__file__)))
        save(package / 'manifest.json', dict(assets=assets, native_executed=False,
            inputs=str(root / 'inputs.json'), mesa_patch_sha256=base['mesa_patch_sha256'],
            application_source_revision=source_revision,
            application_scope='Unchanged GLTeapot and GLU sources; external scripting/capture controller. Unqualified.'))
        save(root / 'result.json', dict(status='built', native_executed=False,
            manifest=str(package / 'manifest.json')))
        print(json.dumps(dict(status='built', assets=len(assets), root=str(root))), flush=True)
    except Exception as error:
        save(root / 'result.json', dict(status='failed', error=str(error), native_executed=False))
        raise


if __name__ == '__main__':
    main()
