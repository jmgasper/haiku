#!/usr/bin/env python3
"""Build artifact, QEMU, deployment and recovery tools for the dedicated lab."""

import argparse
import contextlib
import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

import nanokvm
import serial_capture

WORK = Path('/mnt/HaikuWork')
SOURCE = Path(__file__).resolve().parents[2]
CONFIG = WORK / 'state/lab.json'


def local_path(value):
    path = Path(value).resolve()
    if not path.is_relative_to(WORK):
        raise ValueError(f'Project path must be under {WORK}: {path}')
    return path


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def efi_metadata(data):
    if len(data) < 64 or data[:2] != b'MZ':
        raise RuntimeError('EFI loader is empty, truncated, or missing its DOS header')
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    if offset + 94 > len(data) or data[offset:offset + 4] != b'PE\0\0':
        raise RuntimeError('EFI loader has an invalid PE header')
    machine = struct.unpack_from('<H', data, offset + 4)[0]
    magic = struct.unpack_from('<H', data, offset + 24)[0]
    subsystem = struct.unpack_from('<H', data, offset + 24 + 68)[0]
    if (machine, magic, subsystem) != (0xaa64, 0x20b, 10):
        raise RuntimeError('EFI loader is not an ARM64 PE32+ EFI application')
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'machine': 'ARM64'}


def validate_image(image):
    image = local_path(image)
    with image.open('rb') as stream:
        mbr = stream.read(512)
    if len(mbr) != 512 or mbr[510:] != b'\x55\xaa':
        raise RuntimeError('Missing MBR signature')
    partitions = []
    for index in range(4):
        entry = mbr[446 + index * 16:462 + index * 16]
        start, count = struct.unpack_from('<II', entry, 8)
        if count:
            if start == 0 or (start + count) * 512 > image.stat().st_size:
                raise RuntimeError('Partition extends outside the image')
            partitions.append({'type': entry[4], 'start_sector': start, 'sectors': count})
    ordered = sorted(partitions, key=lambda p: p['start_sector'])
    if any(a['start_sector'] + a['sectors'] > b['start_sector'] for a, b in zip(ordered, ordered[1:])):
        raise RuntimeError('Overlapping image partitions')
    esp = [p for p in partitions if p['type'] == 0xef]
    if len(esp) != 1 or not any(p['type'] == 0xeb for p in partitions):
        raise RuntimeError('Expected one EFI partition and a Haiku BFS partition')
    mtype = shutil.which('mtype') or str(WORK / 'toolchains/host/usr/bin/mtype')
    loader = subprocess.run([mtype, '-i', f'{image}@@{esp[0]["start_sector"] * 512}',
                             '::/EFI/BOOT/BOOTAA64.EFI'], check=True, capture_output=True).stdout
    return {'partitions': partitions, 'efi_loader': efi_metadata(loader)}


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, capture_output=True, **kwargs).stdout.strip()


def save(path, value):
    path = local_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.part')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def timestamp():
    return time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + secrets.token_hex(3)


@contextlib.contextmanager
def lock(name):
    with (WORK / f'state/{name}.lock').open('a') as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError(f'Another operation owns the {name} lock') from None
        yield


def ssh(config, host, command, timeout=30, input=None):
    return run(['ssh', '-F', str(local_path(config['ssh_config'])),
                config[host], command], timeout=timeout, input=input)


def remote_python(config, code, timeout=30):
    return ssh(config, 'nanokvm_ssh', 'python3 -c ' + shlex.quote(code), timeout)


def boot_id(config):
    try:
        return ssh(config, 'recovery_ssh', 'cat /proc/sys/kernel/random/boot_id', timeout=8)
    except (subprocess.SubprocessError, OSError):
        return None


def gadget(config):
    code = f'''import json
from pathlib import Path
g = Path({config['gadget']!r})
l = g / 'functions/mass_storage.disk0/lun.0'
print(json.dumps({{key: (l / key).read_text().strip() for key in ('file', 'ro', 'cdrom')}}))
'''
    return json.loads(remote_python(config, code))


def remote_image(value):
    if not re.fullmatch(r'/data/[A-Za-z0-9][A-Za-z0-9._-]*\.img', value):
        raise ValueError('Use a raw /data/<simple-filename>.img image')
    return value


def attach(config, image, readonly=False):
    remote_image(image)
    # Version 2.4.3 may retain CD-ROM flags when returning to disk mode.
    # Detach first, clear flags, then let the API select and persist the image.
    code = f'''from pathlib import Path
p = Path({image!r})
if not p.is_file(): raise RuntimeError('Image is missing')
l = Path({config['gadget']!r}) / 'functions/mass_storage.disk0/lun.0'
(l / 'file').write_text('\\n')
(l / 'cdrom').write_text('0\\n')
(l / 'ro').write_text({('1' if readonly else '0')!r} + '\\n')
'''
    remote_python(config, code)
    nanokvm.api('/api/storage/image/mount', {'file': image, 'cdrom': False})
    actual = gadget(config)
    if actual != {'file': image, 'ro': '1' if readonly else '0', 'cdrom': '0'}:
        raise RuntimeError(f'Unexpected USB disk state: {actual}')
    return actual


def artifact(image):
    image = local_path(image)
    checksum = digest(image)
    record = json.loads((WORK / 'build/arm64/build-record.json').read_text())
    if record['sha256'] != checksum:
        raise RuntimeError('Image does not match the completed build record; rebuild it first')
    output = WORK / f'artifacts/images/haiku-arm64-{checksum[:16]}.img'
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        if digest(output) != checksum:
            raise RuntimeError('Existing immutable artifact has changed')
    else:
        temporary = output.with_suffix('.part')
        shutil.copyfile(image, temporary)
        if digest(temporary) != checksum:
            raise RuntimeError('Image changed during copy; finish the build first')
        temporary.replace(output)
        output.chmod(0o444)
    manifest_path = output.with_suffix('.json')
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        if manifest['sha256'] != checksum:
            raise RuntimeError('Existing artifact manifest mismatch')
        return manifest
    state = record['inputs']['source_status']
    manifest = {
        'image': str(output), 'sha256': checksum, 'bytes': output.stat().st_size,
        'created_utc': timestamp(), 'architecture': 'arm64', 'target': record['target'],
        'source_revision': record['inputs']['source_revision'],
        'source_dirty': record['inputs']['source_dirty'],
        'buildtools_revision': record['inputs']['buildtools_revision'],
        'build_started_utc': record['started_utc'], 'build_finished_utc': record['finished_utc'],
        'haiku_revision': record['haiku_revision'],
        'layout': record['layout'],
        'host': run(['uname', '-a']), 'gcc': run(['gcc', '--version']).splitlines()[0],
        'qemu': run(['qemu-system-aarch64', '--version']).splitlines()[0],
    }
    if state:
        # Untracked files are listed; tracked changes are preserved as a patch.
        # Commit the tree before release builds for exact source reproduction.
        manifest_path.with_suffix('.patch').write_text(record['inputs']['source_patch'])
        manifest['source_status'] = state
        manifest['untracked_sha256'] = record['inputs']['untracked_sha256']
    packages = WORK / 'build/arm64/download'
    manifest['packages'] = [
        {'name': str(p.relative_to(packages)), 'sha256': digest(p)}
        for p in sorted(packages.rglob('*.hpkg'))
    ] if packages.exists() else []
    save(manifest_path, manifest)
    return manifest


def read_manifest(path):
    manifest = json.loads(local_path(path).read_text())
    image = local_path(manifest['image'])
    if image.stat().st_size != manifest['bytes'] or digest(image) != manifest['sha256']:
        raise RuntimeError('Artifact checksum or size does not match its manifest')
    return manifest, image


def deploy(config, manifest_path):
    manifest, image = read_manifest(manifest_path)
    current = gadget(config)
    if current['file'].startswith('/dev/'):
        raise RuntimeError('Whole /data partition is exported. Unmount it on the ROCK and detach before writing /data.')
    # Guest filesystems can write to the USB disk. Give every deployment its
    # own copy, retaining the immutable source image only on the workstation.
    name = remote_image('/data/' + image.stem + '-' + timestamp() + '.img')
    # The server exposes exactly one immutable file, with no directory listing.
    route = '/' + secrets.token_hex(24)
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != route:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Length', str(image.stat().st_size))
            self.end_headers()
            with image.open('rb') as source:
                shutil.copyfileobj(source, self.wfile, 1024 * 1024)

        def log_message(self, *_args):
            pass

    from urllib.parse import urlparse
    host = urlparse(config['nanokvm_url']).hostname
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route_socket:
        route_socket.connect((host, 22))
        address = route_socket.getsockname()[0]
    server = http.server.ThreadingHTTPServer((address, 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f'http://{address}:{server.server_port}{route}'
    code = f'''import hashlib, json, os, shutil, urllib.request
from pathlib import Path
p = Path({name!r})
def digest(p):
    h = hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda: f.read(1048576), b''): h.update(b)
    return h.hexdigest()
expected = {manifest['sha256']!r}
if p.exists():
    if digest(p) != expected: raise RuntimeError('Existing remote image differs; refusing overwrite')
else:
    if shutil.disk_usage('/data').free < {manifest['bytes']} + 67108864:
        raise RuntimeError('Insufficient NanoKVM storage')
    part = p.with_suffix('.part')
    with urllib.request.urlopen({url!r}, timeout=60) as src, part.open('wb') as dst:
        shutil.copyfileobj(src, dst, 1048576)
        dst.flush(); os.fsync(dst.fileno())
    if digest(part) != expected: raise RuntimeError('Upload checksum mismatch')
    part.rename(p)
print(json.dumps({{'image': str(p), 'sha256': expected, 'bytes': p.stat().st_size}}))
'''
    try:
        result = json.loads(remote_python(config, code, timeout=max(180, manifest['bytes'] // 250000)))
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    result['gadget'] = attach(config, name)
    return result


def wait_recovery(config, previous, seconds=90):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = boot_id(config)
        if value and value != previous:
            return value
        time.sleep(3)
    return None


def start_serial(config, output):
    """Start raw capture and require readiness before mutating the target."""
    if config.get('serial_remote_device'):
        local_path(config['ssh_config'])
    return serial_capture.start(config, local_path(output))


def recover(config):
    previous = boot_id(config)
    timeout = int(config.get('recovery_timeout', 90))
    attach(config, config['recovery_image'], readonly=False)
    nanokvm.api('/api/vm/gpio', {'type': 'reset', 'duration': 800})
    value = wait_recovery(config, previous, seconds=timeout)
    if not value:
        nanokvm.api('/api/vm/gpio', {'type': 'power', 'duration': 5000})
        time.sleep(5)
        nanokvm.api('/api/vm/gpio', {'type': 'power', 'duration': 800})
        value = wait_recovery(config, previous, seconds=timeout)
    if not value:
        raise RuntimeError('ROOBI did not return after reset and power cycle; inspect video/serial')
    return {'recovery': 'ROOBI', 'boot_id': value, 'previous_boot_id': previous}


def cycle(config, manifest_path, seconds):
    output = WORK / 'artifacts/hardware' / timestamp()
    output.mkdir(parents=True)
    result = {'started_utc': timestamp(), 'status': 'incomplete', 'captures': []}
    # Check authentication and recovery prerequisites before changing the target.
    nanokvm.api('/api/vm/hardware')
    if not boot_id(config):
        raise RuntimeError('Start trials from reachable ROOBI; use recover first')
    recovery = remote_image(config['recovery_image'])
    remote_python(config, f'from pathlib import Path\nassert Path({recovery!r}).is_file()')
    save(output / 'result.json', result)
    serial = None
    trial_started = False
    try:
        serial = start_serial(config, output / 'serial.log')
        result['serial_configured'] = serial is not None
        trial_started = True
        result['deployment'] = deploy(config, manifest_path)
        if serial:
            serial.check()
            if config.get('serial_trial_baud'):
                serial.set_baud(config['serial_trial_baud'])
        result['boot_id_before'] = boot_id(config)
        nanokvm.api('/api/vm/gpio', {'type': 'reset', 'duration': 800})
        deadline = time.monotonic() + seconds
        index = 0
        while time.monotonic() < deadline:
            time.sleep(min(5, max(0, deadline - time.monotonic())))
            if serial:
                serial.check()
            try:
                result['captures'].append(nanokvm.screenshot(output / f'frame-{index:03}.jpg'))
            except Exception as error:
                result['captures'].append({'capture_error': str(error)})
            index += 1
        result['recovery_os_visible_during_trial'] = boot_id(config)
        result['status'] = 'observed'  # Screenshots require review; never infer a Haiku pass from power/reachability.
    except Exception as error:
        result['status'] = 'error'
        result['error'] = str(error)
    finally:
        if trial_started:
            if serial and config.get('serial_trial_baud'):
                try:
                    serial.set_baud(config.get('serial_baud', 1500000))
                except Exception as error:
                    result['serial_error'] = str(error)
                    result['status'] = 'error'
            try:
                result['recovery'] = recover(config)
            except Exception as error:
                result['status'] = 'recovery_failed'
                result['recovery_error'] = str(error)
        else:
            result['recovery'] = {'unchanged': True, 'reason': 'Trial did not start'}
        if serial:
            try:
                result['serial'] = serial.stop()
            except Exception as error:
                result['serial_error'] = str(error)
                if result['status'] != 'recovery_failed':
                    result['status'] = 'error'
        result['evidence'] = str(output)
        save(output / 'result.json', result)
    return result


def qmp_command(stream, command, arguments=None):
    stream.write((json.dumps({'execute': command, 'arguments': arguments or {}}) + '\n').encode())
    stream.flush()
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError('QMP closed')
        response = json.loads(line)
        if 'error' in response:
            raise RuntimeError(str(response['error']))
        if 'return' in response:
            return response['return']


def qemu(manifest_path, seconds, expect, el2=False, usb_controller='xhci'):
    manifest, image = read_manifest(manifest_path)
    output = WORK / 'artifacts/qemu' / timestamp()
    output.mkdir(parents=True)
    overlay = output / 'disk.qcow2'
    run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'raw', '-b', str(image), str(overlay)])
    firmware = Path('/usr/share/qemu-efi-aarch64/QEMU_EFI.fd')
    firmware_copy = output / 'QEMU_EFI.fd'
    shutil.copyfile(firmware, firmware_copy)
    machine = 'virt,virtualization=on,gic-version=3' if el2 else 'virt'
    controller = {'xhci': 'qemu-xhci', 'ehci': 'usb-ehci'}[usb_controller]
    command = ['qemu-system-aarch64', '-M', machine, '-cpu', 'max', '-m', '2048', '-smp', '4',
               '-bios', str(firmware_copy), '-device', f'{controller},id=usb',
               '-drive', f'file={overlay},if=none,id=drv0,format=qcow2',
               '-device', 'usb-storage,bus=usb.0,drive=drv0']
    # QEMU's standalone EHCI has no low/full-speed companion. Keep HID on
    # xHCI while requiring the boot disk to go through the selected controller.
    hid_bus = 'usb.0'
    if usb_controller == 'ehci':
        command += ['-device', 'qemu-xhci,id=hid']
        hid_bus = 'hid.0'
    command += ['-device', f'usb-kbd,bus={hid_bus}', '-device', f'usb-tablet,bus={hid_bus}',
               '-device', 'ramfb', '-display', 'none', '-monitor', 'none', '-nic', 'none',
               '-serial', f'file:{output}/serial.log',
               '-qmp', f'unix:{output}/qmp.sock,server=on,wait=off']
    result = {'artifact': manifest, 'command': command, 'firmware_sha256': digest(firmware_copy),
              'evidence': str(output), 'expect': expect, 'status': 'incomplete'}
    with (output / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + seconds
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(1)
            if process.poll() is not None:
                raise RuntimeError(f'QEMU exited early: {process.returncode}')
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(10)
                sock.connect(str(output / 'qmp.sock'))
                with sock.makefile('rwb') as stream:
                    json.loads(stream.readline())
                    qmp_command(stream, 'qmp_capabilities')
                    qmp_command(stream, 'screendump', {'filename': str(output / 'screen.ppm')})
                    qmp_command(stream, 'quit')
            serial = (output / 'serial.log').read_text(errors='replace')
            result['status'] = 'observed' if not expect else ('pass' if re.search(expect, serial) else 'fail')
            if re.search(r'PANIC:|Welcome to Kernel Debugging Land', serial):
                result['status'] = 'fail'
                result['error'] = 'Kernel panic/debugger appeared in the serial log'
        except Exception as error:
            result['status'] = 'error'
            result['error'] = str(error)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
    save(output / 'result.json', result)
    return result


def main():
    if not os.path.ismount(WORK):
        raise RuntimeError(f'Required filesystem is not mounted: {WORK}')
    local_path(SOURCE)
    os.environ['TMPDIR'] = str(WORK / 'tmp')
    for directory in ('tmp', 'state', 'artifacts'):
        (WORK / directory).mkdir(exist_ok=True)
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('doctor')
    p = sub.add_parser('artifact'); p.add_argument('image')
    p = sub.add_parser('qemu'); p.add_argument('manifest')
    p.add_argument('--seconds', type=int, default=90); p.add_argument('--expect')
    p.add_argument('--el2', action='store_true', help='Exercise EL2 handoff with GICv3')
    p.add_argument('--usb-controller', choices=('xhci', 'ehci'), default='xhci',
                   help='USB controller carrying the boot disk')
    p = sub.add_parser('deploy'); p.add_argument('manifest')
    p = sub.add_parser('cycle'); p.add_argument('manifest'); p.add_argument('--seconds', type=int, default=60)
    sub.add_parser('recover')
    args = parser.parse_args()
    if hasattr(args, 'seconds') and not 5 <= args.seconds <= 3600:
        parser.error('--seconds must be between 5 and 3600')
    if args.action == 'artifact':
        with lock('build'):
            result = artifact(args.image)
    elif args.action == 'qemu':
        result = qemu(args.manifest, args.seconds, args.expect, args.el2, args.usb_controller)
    else:
        config = json.loads(CONFIG.read_text())
        nanokvm.BASE = config['nanokvm_url']
        nanokvm.SESSION = WORK / 'state/nanokvm-session.json'
        if args.action == 'doctor':
            result = {'free_gib': round(shutil.disk_usage(WORK).free / 2**30, 1),
                      'recovery_boot_id': boot_id(config), 'gadget': gadget(config),
                      'nanokvm': nanokvm.api('/api/vm/hardware'),
                      'serial_configured': bool(config.get('serial_device') or config.get('serial_remote_device'))}
        else:
            with lock('hardware'):
                if args.action == 'deploy':
                    result = deploy(config, args.manifest)
                elif args.action == 'cycle':
                    result = cycle(config, args.manifest, args.seconds)
                else:
                    result = recover(config)
    print(json.dumps(result, indent=2))
    if result.get('status') in ('error', 'fail', 'recovery_failed'):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
