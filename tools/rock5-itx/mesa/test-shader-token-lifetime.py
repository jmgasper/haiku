#!/usr/bin/env python3
"""Check interpreter shader replacement with deterministic token-allocation reuse."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mesa-source', required=True, type=Path)
parser.add_argument('--host-build', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
work = Path('/mnt/HaikuWork')
source, build, output = (p.resolve() for p in (args.mesa_source, args.host_build, args.output))
assert work.is_mount() and os.environ.get('HAIKU_WORK') == str(work)
assert all(p.is_relative_to(work) and p != work for p in (source, build, output))
run = output / datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
run.mkdir(parents=True)
record = dict(status='running', root=str(run), native_qualified=False, inputs=[], commands=[])

def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

for path, name in ((source / 'src/gallium/auxiliary/draw/draw_vs_exec.c', 'draw_vs_exec.c'),
        (here / 'shader-token-lifetime-test.c', 'test.c'), (Path(__file__), 'runner.py')):
    shutil.copy2(path, run / name)
    record['inputs'].append(dict(source=str(path), file=name, sha256=digest(path)))
commands = json.loads((build / 'compile_commands.json').read_text())
template = next(c for c in commands if c['file'].endswith('/draw_vs_exec.c'))
assert Path(template['directory']).resolve() == build
flags = shlex.split(template['command'])
flags = flags[:flags.index('-o')]
for option in ('-MQ', '-MF'):
    if option in flags:
        i = flags.index(option)
        del flags[i:i + 2]
flags = [f for f in flags if f != '-MD']
flags += ['-I' + str(source / 'src/gallium/auxiliary/draw')]
assert '-fsanitize=address,undefined' in flags
environment = dict(os.environ)
environment.pop('LD_LIBRARY_PATH', None)
environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
    UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', GALLIUM_NOSSE='1')

def execute(command, name):
    with (run / name).open('w') as log:
        result = subprocess.run(command, cwd=build, env=environment,
            stdout=log, stderr=subprocess.STDOUT)
    record['commands'].append(dict(command=command, exit_status=result.returncode, log=name))
    (run / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    if result.returncode:
        print((run / name).read_text()[-6000:])
    result.check_returncode()

for name in ('test', 'draw_vs_exec'):
    execute(flags + ['-c', str(run / (name + '.c')), '-o', str(run / (name + '.o'))], name + '-compile.log')
libraries = [build / p for p in ('src/gallium/auxiliary/libgallium.a',
    'src/compiler/nir/libnir.a', 'src/compiler/libcompiler.a', 'src/util/libmesa_util.a',
    'src/c11/impl/libmesa_util_c11.a', 'src/util/blake3/libblake3.a',
    'src/util/libmesa_util_clflush.a', 'src/util/libmesa_util_clflushopt.a',
    'src/util/libmesa_util_simd.a')]
for path in libraries:
    record['inputs'].append(dict(source=str(path), sha256=digest(path)))
execute(['c++', '-fsanitize=address,undefined', '-g', '-o', str(run / 'test'),
    str(run / 'test.o'), str(run / 'draw_vs_exec.o'), '-Wl,--wrap=tgsi_dup_tokens,--wrap=free',
    '-Wl,--start-group', *map(str, libraries), '-Wl,--end-group', '-lm', '-lpthread', '-ldl'], 'link.log')
execute([str(run / 'test')], 'execution.log')
assert 'TOKEN_LIFETIME_RESULT checks=10 failures=0 allocations=10 releases=10 reuses=8\n' in (run / 'execution.log').read_text()
record.update(status='pass', checks=10, token_reuses=8)
(run / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
(output / 'latest-host-test.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(dict(status='pass', root=str(run), checks=10, native_qualified=False)))
