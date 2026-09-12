#!/usr/bin/env python3
"""Check USB login, memory integrity and optional software power control in QEMU."""

import argparse
import hashlib
import json
import os
import re
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import time

import lab
import qemu_nvme
import shell
import shell_image


def prepare_transfer_peer(output):
    token = secrets.token_hex(32)
    fixture = hashlib.shake_256(b'ROCK5 USB round trip fixture v1').digest(8 * 1024 * 1024)
    (output / 'transfer-source.bin').write_bytes(fixture)
    peer = output / 'transfer-peer.py'
    peer.write_text('''import json, pathlib, struct, sys
root = pathlib.Path(__file__).parent
def read_exactly(count):
    data = bytearray()
    while len(data) < count:
        chunk = sys.stdin.buffer.read(count - len(data))
        if not chunk:
            raise EOFError('Truncated guest transfer')
        data.extend(chunk)
    return data
if read_exactly(65) != %r:
    raise RuntimeError('Incorrect transfer token')
direction = sys.argv[1]
if direction in ('receive', 'truncated'):
    data = (root / 'transfer-source.bin').read_bytes()
    sys.stdout.buffer.write(struct.pack('!Q', len(data)))
    sys.stdout.buffer.write(data if direction == 'receive' else data[:4096])
    sys.stdout.buffer.flush()
else:
    count, = struct.unpack('!Q', read_exactly(8))
    if count > 16 * 1024 * 1024:
        raise ValueError('Oversized guest transfer')
    (root / 'transfer-returned.bin').write_bytes(read_exactly(count))
''' % (token.encode() + b'\n'))
    return {'token': token, 'peer': str(peer), 'bytes': len(fixture),
            'sha256': hashlib.sha256(fixture).hexdigest()}


def check_transfers(client, output, credentials, fixture):
    helper = '/boot/home/config/non-packaged/bin/rock5_file_transfer'
    token = fixture['token']
    path = '/boot/home/rock5-roundtrip.bin'
    # Installed images can retain fixtures from an earlier qualification run.
    # Reset only these test files in this run's disposable QEMU overlay.
    commands = (f'umask 077\nrm -f {path} /boot/home/rock5-partial.bin\n'
                f'{helper} receive 10.0.2.100 9000 {token} {path}\n'
                f'actual=$(sha256sum {path})\n'
                f'[ "${{actual%% *}}" = {fixture["sha256"]} ]\n'
                f'{helper} send 10.0.2.100 9001 {token} {path}\n'
                f'if {helper} receive 10.0.2.100 9002 {token} /boot/home/rock5-partial.bin; '
                'then exit 1; else rock5_partial=$?; [ "$rock5_partial" -eq 1 ]; fi\n'
                'echo ROCK5_TRANSFER_CHECKS_PASS\n')
    shell.execute(client, commands, output / 'transfer.txt', credentials, timeout=180)
    returned = output / 'transfer-returned.bin'
    if not returned.exists() or lab.digest(returned) != fixture['sha256']:
        raise RuntimeError('Guest round-trip checksum differs from the fixture')
    return {'bytes': fixture['bytes'], 'sha256': fixture['sha256'],
            'roundtrip': 'pass', 'truncated_transfer_rejected': True}


def diagnose(output):
    """Collect guest state over the emulated keyboard when its login is unavailable."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(10)
        sock.connect(str(output / 'qmp.sock'))
        with sock.makefile('rwb') as stream:
            json.loads(stream.readline())
            lab.qmp_command(stream, 'qmp_capabilities')

            def key(names):
                lab.qmp_command(stream, 'send-key', {'keys': [
                    {'type': 'qcode', 'data': name} for name in names], 'hold-time': 70})
                time.sleep(.15)

            key(['ctrl', 'alt', 'delete'])
            time.sleep(2)
            lab.qmp_command(stream, 'input-send-event', {'events': [
                {'type': 'abs', 'data': {'axis': 'x', 'value': round(528 * 32767 / 1023)}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': round(516 * 32767 / 767)}},
                {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
            time.sleep(.15)
            lab.qmp_command(stream, 'input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
            time.sleep(3)
            codes = {' ': 'spc', '/': 'slash', '-': 'minus', '\n': 'ret'}
            for command in ('ifconfig', 'netstat -n', 'ps', 'tail -60 /var/log/syslog',
                            'cat /boot/system/settings/network/services'):
                for character in command + ' > /dev/dprintf\n':
                    key(['shift', 'dot'] if character == '>' else [codes.get(character, character)])
            time.sleep(3)
            lab.qmp_command(stream, 'screendump', {'filename': str(output / 'failure.ppm')})


def check_pci_config(client, output, credentials, after_reboot=False):
    name = 'pci-config-after-reboot.txt' if after_reboot else 'pci-config.txt'
    transcript = output / name
    shell.execute(client,
                  '/boot/home/config/non-packaged/bin/rock5_pci_config_probe qemu\n',
                  transcript, credentials, timeout=30)
    if 'ROCK5_PCI_CONFIG_PASS profile=qemu functions=2' not in transcript.read_text():
        raise RuntimeError('Missing read-only PCI configuration evidence')
    return {'status': 'pass', 'functions': 2, 'transcript': str(transcript)}


def run(manifest_path, el1=False, memory=False, power=False, normal=False, platform=False,
        transfer=False, services=False, cache=False, nvme=False, pci_config=False):
    if nvme and not (power and normal):
        raise ValueError('NVMe validation requires normal reboot and power-off')
    if pci_config and not nvme:
        raise ValueError('PCI configuration probe requires the extra NVMe fixture')
    manifest, image = lab.read_manifest(manifest_path)
    if not manifest.get('private_image'):
        raise ValueError('An authenticated private shell image is required')
    credentials = shell_image.read_credentials()
    output = lab.WORK / 'artifacts/qemu-shell' / lab.timestamp()
    output.mkdir(parents=True)
    print(json.dumps({'started': str(output)}), flush=True)
    if nvme:
        nvme_fixture = qemu_nvme.prepare(output)
    firmware = output / 'QEMU_EFI.fd'
    shutil.copyfile('/usr/share/qemu-efi-aarch64/QEMU_EFI.fd', firmware)
    subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'raw', '-b',
                    str(image), str(output / 'disk.qcow2')], check=True)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    network = f'user,id=nic,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:23'
    if transfer:
        fixture = prepare_transfer_peer(output)
        for offset, direction in enumerate(('receive', 'send', 'truncated')):
            peer_command = shlex.join([sys.executable, fixture['peer'], direction])
            network += f',guestfwd=tcp:10.0.2.100:{9000 + offset}-cmd:{peer_command}'
    command = ['qemu-system-aarch64', '-M',
               'virt' if el1 else 'virt,virtualization=on,gic-version=3',
               '-cpu', 'max', '-m', '2048', '-smp', '4', '-bios', str(firmware),
               '-device', 'usb-ehci,id=usb',
               '-drive', f'file={output / "disk.qcow2"},if=none,id=drv0,format=qcow2',
               '-device', 'usb-storage,bus=usb.0,drive=drv0',
               '-device', 'qemu-xhci,id=hid', '-device', 'usb-kbd,bus=hid.0',
               '-device', 'usb-tablet,bus=hid.0', '-device', 'ramfb',
               '-display', 'none', '-monitor', 'none',
               '-serial', f'file:{output / "serial.log"}',
               '-qmp', f'unix:{output / "qmp.sock"},server=on,wait=off',
               '-netdev', network,
               '-device', 'usb-net,bus=hid.0,netdev=nic',
               '-object', f'filter-dump,id=trace,netdev=nic,file={output / "network.pcap"}']
    if nvme:
        command += ['-drive', f'file={nvme_fixture["disk"]},if=none,id=nvme0,format=raw',
                    '-device', f'nvme,drive=nvme0,serial={nvme_fixture["serial"]}']
    result = {'artifact': manifest, 'command': command, 'evidence': str(output),
              'status': 'incomplete', 'expect': 'authenticated remote commands',
              'firmware_sha256': lab.digest(firmware),
              'power_mode': 'normal' if normal else 'quick'}
    if nvme:
        result['nvme'] = {'fixture': nvme_fixture}
    serial = output / 'serial.log'

    def wait_for_boot(previous=0):
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('QEMU exited before a usable boot')
            if serial.exists():
                data = serial.read_bytes()
                if re.search(rb'PANIC:|Welcome to Kernel Debugging Land', data):
                    raise RuntimeError('Kernel panic/debugger appeared')
                if data.count(b'ROCK5_SHELL_CONFIGURED 10.0.2.15') > previous:
                    time.sleep(2)
                    return
            time.sleep(1)
        raise TimeoutError('No new USB shell configuration marker')

    def login(value=credentials):
        # The configuration marker precedes network_server's asynchronous reload.
        # Record retries rather than losing the entire boot on an early connect.
        deadline = time.monotonic() + 45
        while True:
            try:
                return shell.login('127.0.0.1', port, value, timeout=10)
            except (OSError, EOFError, TimeoutError) as error:
                result.setdefault('login_retries', []).append(str(error))
                if time.monotonic() >= deadline:
                    raise
                time.sleep(1)

    with (output / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for_boot()
            incorrect = dict(credentials, password='deliberately-invalid-password')
            with login(incorrect) as client:
                if b'Login failed.' not in client.read_until(b'Login failed.', 15):
                    raise RuntimeError('Incorrect password was not rejected')
                result['incorrect_password_rejected'] = True
            with login() as client:
                time.sleep(2)
                commands = ('uname -a\nifconfig\ncat /boot/system/settings/network/services\n'
                            "su -c 'echo ROCK5_SU_OK' baron 0<&-\n")
                if memory:
                    commands += (
                        '/boot/home/config/non-packaged/bin/rock5_memory_probe 64 4 2\n'
                        'if /boot/home/config/non-packaged/bin/rock5_memory_probe 1 2 1 --inject-error; '
                        'then exit 1; else rock5_inject=$?; [ "$rock5_inject" -eq 1 ]; fi\n')
                if platform:
                    commands += '/boot/home/config/non-packaged/bin/rock5_platform_probe 1\n'
                if cache:
                    commands += '/boot/home/config/non-packaged/bin/rock5_cache_probe 32\n'
                if services:
                    helper = '/boot/home/config/non-packaged/bin/rock5_services_probe'
                    commands += (f'{helper}\nif {helper} --legacy-range; then exit 1; '
                                 'else rock5_legacy=$?; [ "$rock5_legacy" -eq 1 ]; fi\n')
                shell.execute(client, commands, output / 'remote-shell.txt', credentials)
                text = (output / 'remote-shell.txt').read_text()
                for expected in ('R1~beta6+development', 'inet addr: 10.0.2.15',
                                 'address 10.0.2.15', 'ROCK5_SU_OK'):
                    if expected not in text:
                        raise RuntimeError('Missing guest evidence: ' + expected)
                result['authenticated_commands'] = True
                if memory:
                    for expected in ('ROCK5_MEMORY_PASS bytes=67108864', 'MISMATCH word=0 ',
                                     'ROCK5_MEMORY_FAIL bytes=1048576'):
                        if expected not in text:
                            raise RuntimeError('Missing memory evidence: ' + expected)
                    result['memory_probe'] = '64 MiB passed; injected corruption detected'
                if platform:
                    if 'ROCK5_PLATFORM_PASS' not in text:
                        raise RuntimeError('Platform checks did not pass')
                    result['platform_probe'] = 'Pinned clocks, 32 fork/exec checks, eight recovered faults'
                if cache:
                    expected = 'ROCK5_CACHE_PASS checked=8192 mismatches=0 '
                    if expected not in text:
                        raise RuntimeError('Instruction replacement checks did not pass')
                    result['cache_probe'] = {'cpus': 4, 'rounds': 32, 'checks': 8192,
                                             'mismatches': 0}
                if transfer:
                    result['file_transfer'] = check_transfers(client, output, credentials, fixture)
                if services:
                    if 'ROCK5_SERVICES_PASS' not in text or 'ROCK5_SERVICES_FAIL' not in text:
                        raise RuntimeError('Missing service descriptor regression evidence')
                    result['services_probe'] = 'Reverse descriptors pass; legacy range fails'
                if nvme:
                    if pci_config:
                        result['pci_config'] = check_pci_config(client, output, credentials)
                    result['nvme'].update(qemu_nvme.check_initial(
                        client, output, credentials, nvme_fixture))
                if power:
                    previous = serial.read_bytes().count(b'ROCK5_SHELL_CONFIGURED 10.0.2.15')
                    result['software_reboot_requested_at'] = lab.timestamp()
                    client.write(b'echo ROCK5_REBOOT_REQUEST > /dev/dprintf; sync; shutdown '
                                 + (b'-r' if normal else b'-rq') + b'\r\n')
                    time.sleep(1)
                else:
                    client.write(b'exit\r\n')
            if power:
                wait_for_boot(previous)
                data = serial.read_bytes()
                conduit = b'HVC' if el1 else b'SMC'
                if b'PSCI: requesting system reset' not in data or b'via ' + conduit not in data:
                    raise RuntimeError('Missing firmware reset evidence')
                with login() as client:
                    time.sleep(2)
                    shell.execute(client, 'uname -a\nsystem_time\n', output / 'after-reboot.txt',
                                  credentials)
                    if nvme:
                        if pci_config:
                            result['pci_config_after_reboot'] = check_pci_config(
                                client, output, credentials, after_reboot=True)
                        result['nvme'].update(qemu_nvme.check_after_reboot(
                            client, output, credentials, nvme_fixture))
                    result.update(software_reboot='pass', psci_conduit=conduit.decode())
                    client.write(b'sync; shutdown ' + (b'' if normal else b'-q') + b'\r\n')
                    time.sleep(1)
                process.wait(timeout=60)
                if process.returncode != 0 or b'PSCI: requesting system off' not in serial.read_bytes():
                    raise RuntimeError('Missing successful firmware power-off evidence')
                result['software_power_off'] = 'pass'
                if nvme:
                    result['nvme'].update(qemu_nvme.verify_host(nvme_fixture))
            result['status'] = 'pass'
        except Exception as error:
            result.update(status='error', error=str(error))
            if process.poll() is None:
                try:
                    diagnose(output)
                except Exception as capture_error:
                    result['capture_error'] = str(capture_error)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
            lab.save(output / 'result.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest')
    parser.add_argument('--el1', action='store_true', help='Use EL1/HVC instead of EL2/SMC')
    parser.add_argument('--memory', action='store_true')
    parser.add_argument('--platform', action='store_true')
    parser.add_argument('--cache', action='store_true', help='Check ARM64 instruction replacement on each CPU')
    parser.add_argument('--transfer', action='store_true', help='Check binary round trip and truncated input')
    parser.add_argument('--services', action='store_true', help='Check reverse pipe descriptors')
    parser.add_argument('--nvme', action='store_true',
                        help='Check disposable NVMe I/O across normal reboot and shutdown')
    parser.add_argument('--pci-config', action='store_true',
                        help='Read known host/NVMe configuration pages (requires --nvme)')
    parser.add_argument('--power', action='store_true', help='Reboot, log in again, then power off')
    parser.add_argument('--normal', action='store_true', help='Use desktop shutdown (requires --power)')
    parser.add_argument('--result', help='Also save the full result at this local path')
    args = parser.parse_args()
    if args.normal and not args.power:
        parser.error('--normal requires --power')
    if args.nvme and not (args.power and args.normal):
        parser.error('--nvme requires --power --normal')
    if args.pci_config and not args.nvme:
        parser.error('--pci-config requires --nvme')
    if not os.path.ismount(lab.WORK):
        raise RuntimeError(f'Required filesystem is not mounted: {lab.WORK}')
    os.umask(0o077)
    result = run(args.manifest, args.el1, args.memory, args.power, args.normal, args.platform,
                 args.transfer, args.services, args.cache, args.nvme, args.pci_config)
    if args.result:
        lab.save(args.result, result)
    print(json.dumps({key: result.get(key) for key in
                      ('status', 'evidence', 'error', 'software_reboot', 'software_power_off')}))
    raise SystemExit(0 if result['status'] == 'pass' else 1)


if __name__ == '__main__':
    main()
