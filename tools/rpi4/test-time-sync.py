#!/usr/bin/env python3
"""Run the production clock worker with an isolated clock/network command fixture."""
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

source = Path(__file__).resolve().parents[2] / 'data/boot/rpi/rpi-time-sync'
fixture = r'''#!/usr/bin/env python3
import json, os, pathlib, sys, time
root = pathlib.Path(os.environ['RPI_TIME_TEST'])
p = root / 'clock.json'
s = json.loads(p.read_text())
command = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
result = 0
output = None
if command == 'finddir':
    output = str(root / 'settings')
elif command == 'date':
    if '-r' in args: output = str(s['build'])
    elif '-s' in args:
        s['restored'].append(int(args[-1][1:]))
        s['now'] = s['restored'][-1]
    else: output = str(s['now'])
elif command == 'timeout':
    assert args == ['30', '/boot/system/preferences/Time', '--update']
    s['attempts'] += 1
    result = 1 if s['attempts'] <= s['failures'] else 0
    if not result: s['now'] = 1791359999
elif command == 'sleep':
    s['sleeps'].append(int(args[0]))
else: raise AssertionError(command)
p.write_text(json.dumps(s))
if output is not None: print(output)
if command == 'sleep' and args == ['3600']: time.sleep(30)
sys.exit(result)
'''

for name, now, saved, failures, expected in [
    ('fresh-late-network', 0, None, 65, [1791300000]),
    ('saved-clock', 0, '1791350000\n', 2, [1791350000]),
    ('malformed-cache', 0, 'not-a-timestamp\n', 1, [1791300000]),
    ('never-move-backwards', 1791355000, '1791350000\n', 0, []),
]:
    with tempfile.TemporaryDirectory(prefix='rpi-time-', dir='/mnt/HaikuWork/tmp') as work:
        root = Path(work)
        (root / 'bin').mkdir()
        state = root / 'settings/rpi-time'
        state.mkdir(parents=True)
        if saved is not None: (state / 'last-sync').write_text(saved)
        data = dict(build=1791300000, now=now, restored=[], attempts=0,
                    failures=failures, sleeps=[])
        (root / 'clock.json').write_text(json.dumps(data))
        for command in ('date', 'finddir', 'timeout', 'sleep'):
            path = root / 'bin' / command
            path.write_text(fixture)
            path.chmod(0o755)
        env = dict(os.environ, RPI_TIME_TEST=str(root),
                   PATH=str(root / 'bin') + ':' + os.environ['PATH'])
        process = subprocess.Popen(['sh', str(source)], env=env,
                                   start_new_session=True)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                try: data = json.loads((root / 'clock.json').read_text())
                except json.JSONDecodeError: continue
                if data['sleeps'] and data['sleeps'][-1] == 3600: break
                assert process.poll() is None, f'{name}: worker stopped early'
                time.sleep(0.02)
            else: raise AssertionError(f'{name}: did not synchronize')
            assert data['attempts'] == failures + 1, data
            assert data['restored'] == expected, data
            assert (state / 'last-sync').read_text() == '1791359999\n'
            if failures > 60: assert 60 in data['sleeps'], data
        finally:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=5)
        print(name + ': passed')
