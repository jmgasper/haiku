#!/usr/bin/env python3
"""Compile actual patched Mesa reset functions against a queue-state fixture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--baseline', type=Path, required=True)
args = parser.parse_args()
relative = 'src/gallium/drivers/panfrost/pan_csf.c'
source = (args.source / relative).read_text()
baseline = (args.baseline / relative).read_text()


def function(text, name):
    marker = '\n' + name + '('
    assert text.count(marker) == 1, name
    at = text.index(marker)
    start = text.rfind('\nstatic ', 0, at) + 1
    begin = text.index('{', at)
    depth = 1
    end = begin + 1
    while depth:
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
        end += 1
    return text[start:end] + '\n'


def functions(text, native):
    result = function(text, 'update_reset_status')
    if native:
        result += '#ifdef __HAIKU__\n' + function(text, 'csf_native_reset_status') + '#endif\n'
    result += function(text, 'csf_check_ctx_state_and_reinit')
    result += function(text, 'get_device_reset_status')
    return result


with tempfile.TemporaryDirectory(prefix='mesa-reset-status-') as directory:
    root = Path(directory)
    code = root / 'reset-functions.inc'
    code.write_text(functions(source, True))
    old = root / 'baseline.inc'
    old.write_text(functions(baseline, False))
    # With Haiku disabled, both preprocessed implementations must match.
    def preprocess(path):
        value = subprocess.check_output(['cc', '-E', '-P', '-x', 'c', str(path)], text=True)
        return '\n'.join(line.strip() for line in value.splitlines() if line.strip())
    assert preprocess(code) == preprocess(old), 'Non-Haiku reset implementation changed'
    binary = root / 'reset-status-test'
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-no-pie',
               '-D__HAIKU__', '-I' + str(root),
               '-I' + str(args.source / 'src/panfrost/lib/kmod'),
               str(HERE / 'reset-status-test.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10,
                            env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                                     UBSAN_OPTIONS='halt_on_error=1'))
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'MESA_HAIKU_RESET_STATUS_TEST_PASS' in result.stdout
    print(json.dumps(dict(status='pass', actual_functions=4,
                          non_haiku_implementation_unchanged=True,
                          source=str(args.source / relative),
                          sha256=hashlib.sha256(source.encode()).hexdigest())))
