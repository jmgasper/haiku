#!/usr/bin/env python3
"""Run a captured native lab session; JSON commands on stdin, recovery on exit."""
import argparse
import os
from collections import deque
import json
from pathlib import Path
import select
import signal
import string
import sys
import time
import lab
import nanokvm
import shell


def emit(value):
    print(json.dumps(value), flush=True)


def interrupted(signum, frame):
    raise RuntimeError('Interactive session interrupted: ' + str(signum))


def type_text(value):
    if len(value) > 160:
        raise ValueError('At most 160 characters per operation')
    codes = {c: (0, i + 4) for i, c in enumerate(string.ascii_lowercase)}
    codes.update({c: (2, i + 4) for i, c in enumerate(string.ascii_uppercase)})
    codes.update({c: (0, i + 30) for i, c in enumerate('1234567890')})
    codes.update({c: (2, i + 30) for i, c in enumerate('!@#$%^&*()')})
    for code, plain, shifted in [(44, ' ', ' '), (45, '-', '_'), (46, '=', '+'),
            (47, '[', '{'), (48, ']', '}'), (49, '\\', '|'), (51, ';', ':'),
            (52, "'", '"'), (53, '`', '~'), (54, ',', '<'), (55, '.', '>'), (56, '/', '?')]:
        codes[plain] = (0, code)
        if shifted != plain:
            codes[shifted] = (2, code)
    codes['\n'] = (0, 40)
    codes['\t'] = (0, 43)
    reports = []
    for character in value:
        modifier, key = codes[character]
        reports.extend([bytes([1, modifier, 0, key, 0, 0, 0, 0, 0]), bytes([1] + [0] * 8)])
    nanokvm.send_reports(reports)


def raw_reports(command):
    device = command['device']
    assert type(device) is int and 0 <= device <= 2
    reports = command['reports']
    size = [8, 4, 6][device]
    assert 1 <= len(reports) <= 32
    assert all(len(r) == size and all(type(b) is int and 0 <= b <= 255 for b in r)
               for r in reports)
    code = '''import os, select, time, json
device = %r
reports = %r
fd = os.open('/dev/hidg' + str(device), os.O_WRONLY | os.O_NONBLOCK)
result = []
try:
    for report in reports:
        deadline = time.monotonic() + 2
        while True:
            try:
                written = os.write(fd, bytes(report))
                result.append({'written': written, 'report': report})
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('HID endpoint stopped accepting reports')
                select.select([], [fd], [], 0.02)
        time.sleep(0.15)
finally:
    os.close(fd)
print(json.dumps(result))
''' % (device, reports)
    return json.loads(lab.remote_python(config, code))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('manifest')
parser.add_argument('qemu_result')
parser.add_argument('--seconds', type=int, default=900)
args = parser.parse_args()
if not 60 <= args.seconds <= 3600:
    parser.error('--seconds must be between 60 and 3600')
if not os.path.ismount(lab.WORK):
    raise RuntimeError(f'Required filesystem is not mounted: {lab.WORK}')
os.umask(0o077)
config = json.loads(lab.CONFIG.read_text())
nanokvm.BASE = config['nanokvm_url']
nanokvm.SESSION = lab.WORK / 'state/nanokvm-session.json'
manifest = args.manifest
qemu = json.loads(lab.local_path(args.qemu_result).read_text())
assert qemu['status'] == 'pass'
assert qemu['artifact']['sha256'] == json.loads(Path(manifest).read_text())['sha256']
output = lab.WORK / 'artifacts/interactive' / lab.timestamp()
output.mkdir(parents=True)
result = {'evidence': str(output), 'status': 'incomplete', 'events': [], 'captures': []}
for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
    signal.signal(sig, interrupted)
with lab.lock('hardware'):
    result['boot_id_before'] = lab.boot_id(config)
    assert result['boot_id_before'], 'Start from ROOBI'
    nanokvm.api('/api/vm/hardware')
    serial = None
    started = False
    try:
        serial = lab.start_serial(config, output / 'serial.log')
        started = True
        emit({'uploading': True, 'evidence': str(output)})
        result['deployment'] = lab.deploy(config, manifest)
        serial.set_baud(config['serial_trial_baud'])
        nanokvm.api('/api/vm/gpio', {'type': 'reset', 'duration': 800})
        emit({'session_started': True, 'evidence': str(output), 'limit_seconds': args.seconds})
        deadline = time.monotonic() + args.seconds
        next_capture = time.monotonic() + 10
        input_buffer = b''
        input_lines = deque()
        frame = 0
        def capture():
            global frame
            path = output / f'frame-{frame:03}.jpg'
            value = nanokvm.screenshot(path)
            result['captures'].append(value)
            frame += 1
            lab.save(output / 'result.json', result)
            return str(path)
        while time.monotonic() < deadline:
            serial.check()
            if not input_lines and select.select([sys.stdin], [], [], 1)[0]:
                chunk = os.read(sys.stdin.fileno(), 65536)
                if not chunk:
                    break
                input_buffer += chunk
                if len(input_buffer) > 1024 * 1024:
                    raise ValueError('JSON command line exceeds 1 MiB')
                while b'\n' in input_buffer:
                    line, input_buffer = input_buffer.split(b'\n', 1)
                    if line.strip():
                        input_lines.append(line)
            if input_lines:
                command = json.loads(input_lines.popleft())
                result['events'].append({'time': lab.timestamp(), 'command': command})
                action = command['action']
                if action == 'finish':
                    break
                if action == 'key':
                    nanokvm.key(command['keys'])
                elif action in ('click', 'double_click'):
                    for _ in range(2 if action == 'double_click' else 1):
                        nanokvm.click(command['x'], command['y'], command.get('width',1920), command.get('height',1080))
                elif action == 'text':
                    type_text(command['text'])
                elif action == 'raw_reports':
                    value = raw_reports(command)
                    result['events'][-1]['result'] = value
                    emit({'raw_reports': value})
                elif action == 'shell':
                    transcript = output / ('shell-' + lab.timestamp() + '.txt')
                    value = shell.run_commands(config, shell.usb_address(command['target']),
                        lab.local_path(command['commands']).read_text(), transcript,
                        command.get('timeout', 60))
                    result['events'][-1]['result'] = value
                    emit(value)
                elif action == 'upload':
                    transcript = output / ('upload-' + lab.timestamp() + '.txt')
                    value = shell.upload(config, shell.usb_address(command['target']),
                        command['source'], command['name'], transcript,
                        command.get('executable', False), command.get('transport', 'tcp'))
                    result['events'][-1]['result'] = value
                    emit(value)
                elif action == 'download':
                    transcript = output / ('download-' + lab.timestamp() + '.txt')
                    value = shell.download(config, shell.usb_address(command['target']),
                        command['name'], command['destination'], transcript)
                    result['events'][-1]['result'] = value
                    emit(value)
                elif action == 'gpio':
                    kind = command['type']
                    duration = command.get('duration', 800)
                    if kind not in ('power', 'reset') or type(duration) is not int \
                            or not 100 <= duration <= 5000:
                        raise ValueError('Invalid GPIO pulse')
                    nanokvm.api('/api/vm/gpio', {'type': kind, 'duration': duration})
                elif action != 'capture':
                    raise ValueError('Unknown action')
                time.sleep(1)
                try:
                    emit({'action': action, 'screenshot': capture()})
                except Exception as error:
                    result['captures'].append({'capture_error': str(error)})
                    emit({'action': action, 'capture_error': str(error)})
            if time.monotonic() >= next_capture:
                try:
                    capture()
                except Exception as error:
                    result['captures'].append({'capture_error':str(error)})
                next_capture = time.monotonic() + 10
        result['status'] = 'observed'
    except BaseException as error:
        result['status'] = 'error'
        result['error'] = str(error)
        emit({'error': str(error)})
    finally:
        if started:
            if serial:
                try:
                    serial.set_baud(config['serial_baud'])
                except BaseException as error:
                    result['serial_baud_error'] = str(error)
            try:
                emit({'recovering': True})
                result['recovery'] = lab.recover(config)
            except BaseException as error:
                result['status'] = 'recovery_failed'
                result['recovery_error'] = str(error)
        if serial:
            try:
                result['serial'] = serial.stop()
            except BaseException as error:
                result['serial_error'] = str(error)
        lab.save(output / 'result.json', result)
        lab.save(lab.WORK / 'state/rock5-desktop-interactive.json', result)
        emit({'finished': result['status'], 'recovery': result.get('recovery'), 'evidence': str(output)})

raise SystemExit(0 if result['status'] == 'observed' else 1)
