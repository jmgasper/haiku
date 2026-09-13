"""SD and eMMC scratch-card tests; native RK3588 attachment remains separate."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex

import lab
import mmc_file_test
import shell


MIB = 1024 * 1024
HELPER = '/boot/home/config/non-packaged/bin/rock5_mmc_probe'
COMPONENTS = {
    'probe_sha256': HELPER,
    'sdhci_sha256': '/boot/system/add-ons/kernel/busses/mmc/sdhci',
    'bus_sha256': '/boot/system/add-ons/kernel/bus_managers/mmc',
    'disk_sha256': '/boot/system/add-ons/kernel/drivers/disk/mmc/mmc_disk',
}
SOURCE_SHA256 = '9e30ad1b8b9f7b4463001582d1ab297f39cfccea5d08540c0ca6d6672785883a'


def emulator():
    manifest = json.loads((lab.WORK / 'state/qemu-mmc-toolchain.json').read_text())
    binary = Path(manifest['binary']).resolve()
    if not binary.is_relative_to(lab.WORK) or manifest['source_sha256'] != SOURCE_SHA256:
        raise ValueError('MMC fixture requires the pinned local QEMU 10.2.0 build')
    if lab.digest(binary) != manifest['binary_sha256']:
        raise ValueError('MMC emulator binary hash changed; rebuild the toolchain receipt')
    return binary, manifest


def prepare(output, filesystem=False):
    root = output / 'mmc'
    root.mkdir()
    disks = []
    for index, kind in enumerate(('sd-card', 'emmc')):
        capacity = (8192 + index * 1024) * MIB
        path = root / (kind + '.img')
        initial = hashlib.shake_256(f'MMC {kind} initial v1'.encode()).digest(8 * MIB)
        high = hashlib.shake_256(f'MMC {kind} high v1'.encode()).digest(8 * MIB)
        guard = bytearray(hashlib.shake_256(f'MMC {kind} guard v1'.encode()).digest(2 * MIB))
        initial_guard = hashlib.sha256(guard).hexdigest()
        with path.open('xb') as stream:
            stream.truncate(capacity)
            for offset, data in ((0, initial), (5120 * MIB, high), (6144 * MIB, guard)):
                stream.seek(offset)
                stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        operations = []
        for offset, size in ((17, 1), (127, 513), (541, 4097), (1031, 131073), (4099, 1048579)):
            guard[offset:offset + size] = initial[:size]
            operations.append(dict(offset=6144 * MIB + offset, bytes=size,
                sha256=hashlib.sha256(guard).hexdigest()))
        disks.append(dict(index=index, kind=kind, sector=512, bytes=capacity, disk=str(path),
            head_sha256=hashlib.sha256(initial).hexdigest(), high_sha256=hashlib.sha256(high).hexdigest(),
            initial_guard_sha256=initial_guard, operations=operations,
            final_guard_sha256=hashlib.sha256(guard).hexdigest()))
    fixture = dict(evidence=str(root), disks=disks)
    if filesystem:
        fixture['filesystem'] = mmc_file_test.prepare(output)
    lab.save(root / 'fixture.json', fixture)
    return fixture


def command(fixture):
    args = []
    for disk in fixture['disks']:
        i = disk['index']
        args += ['-device', f'sdhci-pci,id=mmc{i}',
            '-drive', f'file={disk["disk"]},if=none,id=mmc-disk{i},format=raw,cache=writeback',
            '-device', f'{disk["kind"]},drive=mmc-disk{i},bus=pcie.0/mmc{i}/sd-bus']
    if 'filesystem' in fixture:
        args += mmc_file_test.command(fixture['filesystem'])
    return args


def check_hash(device, offset, size, expected):
    return (f'actual=$(dd if={shlex.quote(device)} bs=1048576 skip={offset} count={size} | sha256sum)\n'
            f'[ "${{actual%% *}}" = {expected} ]\n')


def find_device(rows, sector, capacity):
    matches = [row for row in rows if int(row[1]) == sector and int(row[2]) == capacity]
    if len(matches) != 1:
        raise RuntimeError('MMC/SD disk geometry did not match the fixture uniquely')
    return matches[0][0]


def check(client, output, credentials, fixture, manifest, after_reboot=False):
    root = Path(fixture['evidence'])
    phase = 'after-reboot' if after_reboot else 'first-boot'
    script = 'set -o pipefail\n'
    for key, path in COMPONENTS.items():
        script += f'actual=$(sha256sum {path})\n[ "${{actual%% *}}" = {manifest["mmc_test"][key]} ]\n'
    script += f'for device in /dev/disk/mmc/*/raw; do {HELPER} geometry "$device"; done\n'
    inventory = root / (phase + '-inventory.txt')
    shell.execute(client, script, inventory, credentials, timeout=60)
    rows = re.findall(r'(?m)^MMC_GEOMETRY path=(/dev/disk/mmc/[0-9]+/raw) '
                      r'sector=(\d+) bytes=(\d+) readonly=0\r?$', inventory.read_text())
    count = 3 if 'filesystem' in fixture else 2
    if len(rows) != count or len(set(row[0] for row in rows)) != count:
        raise RuntimeError(f'Expected exactly {count} distinct disposable MMC/SD disks')
    commands = ['set -o pipefail']
    for disk in fixture['disks']:
        device = find_device(rows, disk['sector'], disk['bytes'])
        # Discovery is asynchronous. Re-identify this uniquely sized fixture
        # on every boot; the content hashes below establish data identity.
        disk.setdefault('guest_devices', {})[phase] = device
        disk['guest_device'] = device
        i = disk['index']
        job = 'set -e\nset -o pipefail\n'
        job += check_hash(device, 0, 8, disk['head_sha256'])
        job += check_hash(device, 5120, 8, disk['head_sha256'] if after_reboot else disk['high_sha256'])
        job += check_hash(device, 6144, 2, disk['final_guard_sha256'] if after_reboot else disk['initial_guard_sha256'])
        if not after_reboot:
            source = f'/boot/home/mmc-source-{i}.bin'
            job += f'dd if={device} of={source} bs=1048576 count=8\n'
            job += f'dd if={source} of={device} bs=1048576 seek=5120 count=8 conv=notrunc\n'
            job += check_hash(device, 5120, 8, disk['head_sha256'])
            for case, operation in enumerate(disk['operations']):
                job += (f'dd if={source} of={device} bs={operation["bytes"]} count=1 '
                    f'iflag=fullblock oflag=seek_bytes seek={operation["offset"]} conv=notrunc\n')
                job += check_hash(device, 6144, 2, operation['sha256'])
                job += f'echo MMC_UNALIGNED_PASS disk={i} case={case}\n'
            mode = 'flush' if disk['kind'] == 'emmc' else 'sd-flush'
            job += f'{HELPER} {mode} {device}\n'
        job += f'echo MMC_IO_PASS disk={i} phase={phase}\n'
        (root / f'{phase}-disk-{i}.sh').write_text(job)
        commands += ['(\n' + job + f') > /boot/home/mmc-io-{i}.log 2>&1 &', f'pid{i}=$!']
    commands += ['status=0', 'wait "$pid0" || status=1', 'wait "$pid1" || status=1',
                 'cat /boot/home/mmc-io-0.log /boot/home/mmc-io-1.log', '[ "$status" = 0 ]']
    transcript = root / (phase + '-io.txt')
    shell.execute(client, '\n'.join(commands) + '\n', transcript, credentials, timeout=180)
    text = transcript.read_text()
    for disk in fixture['disks']:
        i = disk['index']
        if f'MMC_IO_PASS disk={i} phase={phase}' not in text:
            raise RuntimeError('Missing MMC/SD I/O completion')
        if not after_reboot:
            for case in range(len(disk['operations'])):
                if f'MMC_UNALIGNED_PASS disk={i} case={case}' not in text:
                    raise RuntimeError('Missing MMC/SD partial-sector check')
            expected = 'pass' if disk['kind'] == 'emmc' else 'unsupported'
            if f'MMC_FLUSH path={disk["guest_device"]} method=B_FLUSH_DRIVE_CACHE result={expected}' not in text:
                raise RuntimeError('Missing explicit MMC/SD flush result')
    result = dict(status='pass', disks=2, phase=phase, transcript=str(transcript), inventory=str(inventory))
    if 'filesystem' in fixture:
        files = fixture['filesystem']
        device = find_device(rows, 512, files['bytes'])
        files.setdefault('guest_devices', {})[phase] = device
        files['guest_device'] = device
        result['filesystem'] = mmc_file_test.check(client, output, credentials,
            files, device, manifest['mmc_test']['fat_sha256'], after_reboot)
    lab.save(root / 'fixture.json', fixture)
    return result


def verify_host(fixture):
    hashes = []
    for disk in fixture['disks']:
        path = Path(disk['disk'])
        if path.stat().st_size != disk['bytes']:
            raise RuntimeError('MMC/SD backing-file size changed')
        with path.open('rb') as stream:
            for offset, size, expected in ((0, 8 * MIB, disk['head_sha256']),
                    (5120 * MIB, 8 * MIB, disk['head_sha256']),
                    (6144 * MIB, 2 * MIB, disk['final_guard_sha256'])):
                stream.seek(offset)
                data = stream.read(size)
                digest = hashlib.sha256(data).hexdigest()
                if len(data) != size or digest != expected:
                    raise RuntimeError('Independent MMC/SD backing-file readback failed')
                hashes.append(dict(disk=disk['index'], offset=offset, bytes=size, sha256=digest))
    result = dict(status='pass', hashes=hashes)
    if 'filesystem' in fixture:
        result['filesystem'] = mmc_file_test.verify_host(fixture['filesystem'])
    return result
