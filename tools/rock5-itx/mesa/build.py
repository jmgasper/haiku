#!/usr/bin/env python3
"""Reconstruct the pinned experimental Haiku Mesa package from local inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
WORK = Path('/mnt/HaikuWork')
PINS = json.loads((HERE / 'sources.json').read_text())


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def checked(path, expected):
    if digest(path) != expected:
        raise ValueError('Input checksum mismatch: ' + str(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archives', type=Path, required=True,
        help='Directory containing the two archives named in sources.json')
    parser.add_argument('--packages', type=Path, required=True,
        help='Directory containing the six pinned Haiku SDK packages')
    parser.add_argument('--output', type=Path, required=True,
        help='New directory under /mnt/HaikuWork; existing builds are never overwritten')
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    if not WORK.is_mount() or os.environ.get('HAIKU_WORK') != str(WORK):
        parser.error('Source tools/rock5-itx/env.sh with /mnt/HaikuWork mounted')
    root = args.output.resolve()
    if not root.is_relative_to(WORK) or root == WORK or root.exists():
        parser.error('--output must be a new directory under /mnt/HaikuWork')
    jobs = int(os.environ.get('HAIKU_JOBS', '8'))
    if jobs <= 0:
        parser.error('HAIKU_JOBS must be positive')
    for name, expected in PINS['bundle_files'].items():
        checked(HERE / name, expected)
    for source in (PINS['mesa'], PINS['libglvnd']):
        checked(args.archives / source['archive'], source['sha256'])
    for package in PINS['sdk_packages']:
        checked(args.packages / package['name'], package['sha256'])

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

    def run(command, name, cwd=None, env=None):
        log = root / (name + '.log')
        print(name, flush=True)
        with log.open('w') as output:
            result = subprocess.run([str(x) for x in command], cwd=cwd,
                env=environment if env is None else env,
                stdout=output, stderr=subprocess.STDOUT)
        commands.append(dict(command=[str(x) for x in command], cwd=str(cwd),
            exit=result.returncode, log=str(log)))
        save(root / 'commands.json', commands)
        result.check_returncode()

    try:
        for source in (PINS['mesa'], PINS['libglvnd']):
            with tarfile.open(args.archives / source['archive']) as archive:
                archive.extractall(root, filter='data')
            source_root = root / source['source_dir']
            for patch in source['patches']:
                run(['git', 'apply', '--check', HERE / patch], patch + '-check', source_root)
                run(['git', 'apply', HERE / patch], patch + '-apply', source_root)
        mesa = root / PINS['mesa']['source_dir']
        source_manifest = json.loads((HERE / 'mesa-native-source-manifest.json').read_text())
        for item in source_manifest['changes']:
            checked(mesa / item['path'], item['after'])
        abi = mesa / 'src/panfrost/lib/kmod/haiku-abi'
        for item in PINS['native_abi']['files']:
            checked(abi / item['name'], item['sha256'])

        sdk = root / 'sysroot'
        system = sdk / 'boot/system'
        system.mkdir(parents=True)
        package_tool = WORK / 'build/arm64/objects/linux/x86_64/release/tools/package/package'
        for index, package in enumerate(PINS['sdk_packages']):
            run([package_tool, 'extract', '-C', system, '-i',
                root / ('package-' + str(index) + '.PackageInfo'),
                (args.packages / package['name']).resolve()], 'sdk-' + str(index))
        for path in system.rglob('*'):
            if path.is_symlink():
                link = os.readlink(path)
                if link.startswith('/boot/system/'):
                    path.unlink()
                    path.symlink_to(os.path.relpath(sdk / link.lstrip('/'), path.parent))

        compiler_prefix = WORK / 'build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-'
        cross = root / 'haiku-aarch64.ini'
        pkg_dirs = [str(system / p) for p in ('develop/lib/pkgconfig', 'lib/pkgconfig')]
        text = '[binaries]\n'
        for key, suffix in [('c', 'gcc'), ('cpp', 'g++'), ('ar', 'ar'), ('strip', 'strip')]:
            text += key + ' = ' + repr(str(compiler_prefix) + suffix) + '\n'
        text += "pkg-config = '/usr/bin/pkg-config'\n\n[host_machine]\n"
        text += "system = 'haiku'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n"
        text += '\n[properties]\nneeds_exe_wrapper = true\nsys_root = ' + repr(str(sdk)) + '\n'
        text += 'pkg_config_libdir = ' + repr(pkg_dirs) + '\n\n[built-in options]\n'
        for key in ('c_args', 'cpp_args', 'c_link_args', 'cpp_link_args'):
            text += key + ' = ' + repr(['--sysroot=' + str(sdk)]) + '\n'
        cross.write_text(text)
        save(root / 'inputs.json', dict(pins=PINS,
            recipe_sha256=digest(__file__), cross_sha256=digest(cross)))
        if args.prepare_only:
            save(root / 'result.json', dict(status='prepared', native_executed=False))
            return

        for name, expected in PINS['host_helpers'].items():
            checked(WORK / 'toolchains/mesa-host/bin' / name, expected)
        environment['PKG_CONFIG_LIBDIR'] = ':'.join(pkg_dirs)
        environment['PKG_CONFIG_SYSROOT_DIR'] = str(sdk)
        meson = WORK / 'toolchains/mesa-python/bin/meson'
        ninja = WORK / 'toolchains/host/usr/bin/ninja'
        glvnd_build = root / 'glvnd-build'
        glvnd_install = root / 'glvnd-install'
        run([meson, 'setup', glvnd_build, root / PINS['libglvnd']['source_dir'],
            '--cross-file=' + str(cross), '--buildtype=debugoptimized',
            '--prefix=/boot/system', '--libdir=lib',
            '--includedir=develop/headers/os/opengl', '--sysconfdir=settings',
            '--wrap-mode=nofallback', '-Dx11=disabled', '-Dglx=disabled', '-Dhgl=true',
            '-Dgles1=false', '-Dgles2=true', '-Degl=true'], 'glvnd-configure')
        run([ninja, '-C', glvnd_build, '-j' + str(jobs)], 'glvnd-compile')
        run([meson, 'install', '-C', glvnd_build, '--no-rebuild'], 'glvnd-install',
            env=dict(environment, DESTDIR=str(glvnd_install)))
        for path in (glvnd_install / 'boot/system').rglob('*'):
            target = system / path.relative_to(glvnd_install / 'boot/system')
            if path.is_dir() and not path.is_symlink():
                target.mkdir(parents=True, exist_ok=True)
            else:
                if target.exists() or target.is_symlink():
                    raise ValueError('Unexpected SDK collision: ' + str(target))
                target.parent.mkdir(parents=True, exist_ok=True)
                if path.is_symlink():
                    target.symlink_to(os.readlink(path))
                else:
                    shutil.copy2(path, target)
        headers = system / 'develop/headers'
        for old, new in [('os/opengl/OpenGLKit.h', 'os/OpenGLKit.h'),
                ('os/opengl/opengl/GLView.h', 'os/opengl/GLView.h')]:
            (headers / old).rename(headers / new)
        for name in ('libGL.so', 'libEGL.so', 'libOpenGL.so', 'libGLESv2.so', 'libGLdispatch.so'):
            (system / 'develop/lib' / name).symlink_to('../../lib/' + name)

        def option(value):
            if isinstance(value, list):
                return ','.join(value) if value else '[]'
            if isinstance(value, bool):
                return str(value).lower()
            return str(value)

        build = root / 'build'
        run([meson, 'setup', build, mesa, '--cross-file=' + str(cross),
            '--prefix=' + str(root / 'install'), '--libdir=lib',
            '--buildtype=debugoptimized', '--wrap-mode=nofallback']
            + ['-D' + key + '=' + option(value) for key, value in PINS['mesa_options'].items()],
            'mesa-configure')
        run([ninja, '-C', build, '-j' + str(jobs)], 'mesa-compile')
        probe = root / 'rock5_haiku_mesa_probe'
        run([str(compiler_prefix) + 'g++', '--sysroot=' + str(sdk), '-std=c++17',
            '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(headers / 'os/opengl'),
            '-I' + str(abi), HERE / 'render-probe.cpp',
            '-L' + str(glvnd_install / 'boot/system/lib'), '-lEGL', '-lGLESv2', '-o', probe],
            'probe-compile')
        window_probe = root / 'rock5_haiku_window_probe'
        run([str(compiler_prefix) + 'g++', '--sysroot=' + str(sdk), '-std=c++17',
            '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(headers / 'os/opengl'),
            '-I' + str(abi), '-I' + str(mesa / 'src/gallium/winsys/sw/hgl'),
            HERE / 'window-probe.cpp', '-L' + str(glvnd_install / 'boot/system/lib'),
            '-lEGL', '-lGLESv2', '-lbe', '-o', window_probe], 'window-probe-compile')
        package = root / 'package'
        package.mkdir()
        assets = []
        libraries = [('lib/libEGL_mesa.so.0', build / 'src/egl/libEGL_mesa.so.0.0.0')]
        for name, version in [('libEGL', '1.1.0'), ('libGLESv2', '2.1.0'),
                ('libGLdispatch', '0.0.0'), ('libOpenGL', '0.0.0'), ('libGL', '1.0.0')]:
            libraries.append(('lib/' + name + '.so.' + version.split('.')[0],
                glvnd_install / 'boot/system/lib' / (name + '.so.' + version)))
        for name, source in libraries + [('rock5_haiku_mesa_probe', probe),
                ('rock5_haiku_window_probe', window_probe)]:
            target = package / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            run([str(compiler_prefix) + 'strip', '--strip-unneeded', target],
                'strip-' + target.name)
            assets.append(dict(source=str(target), guest='/boot/home/mesa-trial/' + name,
                sha256=digest(target), bytes=target.stat().st_size, mode='755',
                unstripped_source=str(source), unstripped_sha256=digest(source)))
        for name, mode in [('run', '755'), ('run-window', '755'), ('vendor.json', '644')]:
            target = package / name
            shutil.copy2(HERE / name, target)
            assets.append(dict(source=str(target), guest='/boot/home/mesa-trial/' + name,
                sha256=digest(target), bytes=target.stat().st_size, mode=mode))
        save(package / 'manifest.json', dict(assets=assets, native_executed=False,
            inputs=str(root / 'inputs.json'), mesa_patch_sha256=digest(HERE / 'mesa-haiku-native.patch')))
        save(root / 'result.json', dict(status='built', native_executed=False,
            manifest=str(package / 'manifest.json')))
    except Exception as error:
        save(root / 'result.json', dict(status='failed', error=str(error), native_executed=False))
        raise


if __name__ == '__main__':
    main()
