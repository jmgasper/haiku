"""Build and execute the exact geometry-helper bytes with a checked pipe adapter."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import shlex
import subprocess

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mesa-source', required=True, type=Path)
parser.add_argument('--host-build', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
source = args.mesa_source.resolve()
build = args.host_build.resolve()
root = args.output.resolve()
work = Path('/mnt/HaikuWork')
assert os.environ.get('HAIKU_WORK') == str(work) and work.is_mount()
assert all(p.is_relative_to(work) and p != work for p in (source, build, root))
run = root / datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
run.mkdir(parents=True)
inputs = [source / 'src/gallium/drivers/panfrost' / name for name in ('pan_sw_polygon.c', 'pan_sw_polygon.h')]
inputs += [here / name for name in ('polygon-context-test-adapter.h', 'polygon-geometry-test.c')]
inputs += [Path(__file__).resolve()]
frozen = []
for path in inputs:
    data = path.read_bytes()
    name = {'polygon-context-test-adapter.h': 'pan_context.h',
            'polygon-geometry-test.c': 'polygon-test.c'}.get(path.name, path.name)
    (run / name).write_bytes(data)
    frozen.append(dict(source=str(path), file=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
(run / 'inputs.json').write_text(json.dumps(frozen, indent=2) + '\n')
commands = json.loads((build / 'compile_commands.json').read_text())
template = next(c for c in commands if c['file'].endswith('/draw_context.c'))
assert Path(template['directory']).resolve() == build
assert (build / template['file']).resolve().is_relative_to(source)
flags = shlex.split(template['command'])
assert '-fsanitize=address,undefined' in flags
flags = flags[:flags.index('-o')]
for option in ('-MQ', '-MF'):
    if option in flags:
        offset = flags.index(option)
        del flags[offset:offset+2]
flags = [f for f in flags if f != '-MD']
flags += ['-I' + str(run), '-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types']
env = os.environ.copy()
env.pop('LD_LIBRARY_PATH', None)
env.pop('PKG_CONFIG_PATH', None)
env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
env['HAIKU_PAN_SW_POLYGON'] = '1'
record = dict(status='in_progress', root=str(run), native_qualified=False, commands=[])
(root / 'latest-host-test.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record), flush=True)

def execute(cmd, log):
    record['commands'].append(cmd)
    with (run / log).open('w') as output:
        result = subprocess.run(cmd, cwd=build, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        record.update(status='failed', failed_log=log, exit_status=result.returncode)
        (run / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        print((run / log).read_text()[-14000:], flush=True)
    result.check_returncode()

for name in ('pan_sw_polygon', 'polygon-test'):
    execute(flags + ['-c', str(run / (name + '.c')), '-o', str(run / (name + '.o'))], name + '-compile.log')
libraries = [
    'src/gallium/auxiliary/libgallium.a', 'src/compiler/nir/libnir.a',
    'src/compiler/libcompiler.a', 'src/util/libmesa_util.a',
    'src/c11/impl/libmesa_util_c11.a', 'src/util/blake3/libblake3.a',
    'src/util/libmesa_util_clflush.a', 'src/util/libmesa_util_clflushopt.a',
    'src/util/libmesa_util_simd.a',
]
execute(['c++', '-fsanitize=address,undefined', '-g', '-o', str(run / 'polygon-test'),
         str(run / 'polygon-test.o'), str(run / 'pan_sw_polygon.o'),
         '-Wl,--start-group', *libraries, '-Wl,--end-group', '-lm', '-lpthread', '-ldl'], 'link.log')
execute([str(run / 'polygon-test')], 'execution.log')
assert 'POLYGON_HELPER_PASS' in (run / 'execution.log').read_text()
for row in frozen:
    assert hashlib.sha256(Path(row['source']).read_bytes()).hexdigest() == row['sha256']
record.update(status='pass', exit_status=0)
(run / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
(root / 'latest-host-test.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(dict(status='pass', root=str(run), native_qualified=False)), flush=True)
