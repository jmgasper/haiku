#!/usr/bin/env python3
"""Check actual Mesa state-tracker notification consumption, including callbacks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--baseline', type=Path, required=True)
args = parser.parse_args()
relative = 'src/mesa/state_tracker/st_cb_flush.c'


def functions(path):
    text = path.read_text()
    output = ''
    for name in ('gl_reset_status_from_pipe_reset_status',
                 'st_device_reset_callback', 'st_get_graphics_reset_status'):
        marker = '\n' + name + '('
        assert text.count(marker) == 1, name
        at = text.index(marker)
        start = text.rfind('\nstatic ', 0, at) + 1
        end = text.index('{', at) + 1
        depth = 1
        while depth:
            if text[end] == '{': depth += 1
            elif text[end] == '}': depth -= 1
            end += 1
        output += text[start:end] + '\n'
    return output


resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='mesa-state-tracker-reset-') as directory:
    root = Path(directory)
    code = root / 'state-tracker-reset-functions.inc'
    code.write_text(functions(args.source / relative))
    old = root / 'baseline.inc'
    old.write_text(functions(args.baseline / relative))
    def preprocess(path):
        text = subprocess.check_output(['cc', '-E', '-P', '-x', 'c', str(path)], text=True)
        return '\n'.join(line.strip() for line in text.splitlines() if line.strip())
    assert preprocess(code) == preprocess(old), 'Non-Haiku state-tracker implementation changed'
    binary = root / 'state-tracker-reset-test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-no-pie',
                    '-D__HAIKU__', '-I' + str(root),
                    str(HERE / 'state-tracker-reset-test.c'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10,
                            env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                                     UBSAN_OPTIONS='halt_on_error=1'))
    print(result.stdout, end='', flush=True)
    if result.returncode:
        print(result.stderr, end='', flush=True)
        raise SystemExit(1)
    assert 'MESA_HAIKU_STATE_TRACKER_RESET_TEST_PASS' in result.stdout
    print(json.dumps(dict(status='pass', actual_functions=3,
                          non_haiku_implementation_unchanged=True,
                          source=str(args.source / relative),
                          sha256=hashlib.sha256((args.source / relative).read_bytes()).hexdigest())))
