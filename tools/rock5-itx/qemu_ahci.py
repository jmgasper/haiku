"""Independent scratch disks for ARM64 AHCI I/O, flush and reboot checks."""
import hashlib
import os
from pathlib import Path
import re
import shlex

import lab
import shell


MIB = 1024 * 1024
HELPER = '/boot/home/config/non-packaged/bin/rock5_ahci_probe'
DRIVER = '/boot/system/add-ons/kernel/busses/scsi/ahci'


def prepare(output):
    root = output / 'ahci'
    root.mkdir()
    disks = []
    # QEMU 8.2 IDE requires 512-byte logical sectors. Exercise 512n and 512e;
    # distinct capacities identify the two paths without assuming SCSI IDs.
    for index, physical_sector in enumerate((512, 4096)):
        sector = 512
        capacity = (8192 + index * 1024) * MIB
        disk = root / f'disk-{index}.img'
        head = hashlib.shake_256(f'AHCI port {index} initial v1'.encode()).digest(8 * MIB)
        high = hashlib.shake_256(f'AHCI port {index} high v1'.encode()).digest(8 * MIB)
        guard = bytearray(hashlib.shake_256(f'AHCI port {index} guard v1'.encode()).digest(2 * MIB))
        initial_guard = hashlib.sha256(guard).hexdigest()
        with disk.open('xb') as stream:
            stream.truncate(capacity)
            for offset, data in ((0, head), (4096 * MIB, high), (6144 * MIB, guard)):
                stream.seek(offset)
                stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        operations = []
        for offset, size in ((17, 1), (127, 513), (541, 4097), (1031, 131073), (4099, 1048579)):
            guard[offset:offset + size] = head[:size]
            operations.append(dict(offset=6144 * MIB + offset, bytes=size,
                sha256=hashlib.sha256(guard).hexdigest()))
        disks.append(dict(index=index, sector=sector, physical_sector=physical_sector,
            bytes=capacity, disk=str(disk),
            serial=f'ROCK5-AHCI-{index}', head_sha256=hashlib.sha256(head).hexdigest(),
            high_sha256=hashlib.sha256(high).hexdigest(), initial_guard_sha256=initial_guard,
            operations=operations, final_guard_sha256=hashlib.sha256(guard).hexdigest()))
    fixture = dict(evidence=str(root), disks=disks)
    lab.save(root / 'fixture.json', fixture)
    return fixture


def command(fixture):
    args = ['-device', 'ich9-ahci,id=sata']
    for disk in fixture['disks']:
        i, sector = disk['index'], disk['sector']
        args += ['-drive', f'file={disk["disk"]},if=none,id=sata{i},format=raw,cache=writeback',
            '-device', f'ide-hd,bus=sata.{i},drive=sata{i},serial={disk["serial"]},'
            f'logical_block_size={sector},physical_block_size={disk["physical_sector"]}']
    return args


def check_hash(device, offset_mib, count_mib, expected):
    return (f'actual=$(dd if={shlex.quote(device)} bs=1048576 skip={offset_mib} count={count_mib} | sha256sum)\n'
            f'[ "${{actual%% *}}" = {expected} ]\n')


def check(client, output, credentials, fixture, manifest, after_reboot=False):
    root = Path(fixture['evidence'])
    phase = 'after-reboot' if after_reboot else 'first-boot'
    pins = manifest['ahci_test']
    script = 'set -o pipefail\n'
    for path, digest in ((HELPER, pins['probe_sha256']), (DRIVER, pins['driver_sha256'])):
        script += f'actual=$(sha256sum {path})\n[ "${{actual%% *}}" = {digest} ]\n'
    script += f'for device in /dev/disk/scsi/*/*/*/raw; do {HELPER} geometry "$device"; done\n'
    inventory = root / (phase + '-inventory.txt')
    shell.execute(client, script, inventory, credentials, timeout=60)
    rows = re.findall(r'(?m)^AHCI_GEOMETRY path=(/dev/disk/scsi/[0-9/]+raw) '
                      r'sector=(\d+) bytes=(\d+) readonly=0\r?$', inventory.read_text())
    if len(rows) != 2 or len(set(row[0] for row in rows)) != 2:
        raise RuntimeError('Expected exactly two distinct disposable AHCI disks')
    commands = ['set -o pipefail']
    for disk in fixture['disks']:
        matches = [row for row in rows if int(row[1]) == disk['sector'] and int(row[2]) == disk['bytes']]
        if len(matches) != 1:
            raise RuntimeError('AHCI disk geometry did not match its fixture')
        device = matches[0][0]
        if after_reboot and disk['guest_device'] != device:
            raise RuntimeError('AHCI disk path changed across reboot')
        disk['guest_device'] = device
        i = disk['index']
        job = 'set -e\nset -o pipefail\n'
        job += check_hash(device, 0, 8, disk['head_sha256'])
        job += check_hash(device, 4096, 8, disk['head_sha256'] if after_reboot else disk['high_sha256'])
        job += check_hash(device, 6144, 2, disk['final_guard_sha256'] if after_reboot else disk['initial_guard_sha256'])
        if not after_reboot:
            source = f'/boot/home/ahci-source-{i}.bin'
            job += f'dd if={device} of={source} bs=1048576 count=8\n'
            job += f'dd if={source} of={device} bs=1048576 seek=4096 count=8 conv=notrunc\n'
            job += check_hash(device, 4096, 8, disk['head_sha256'])
            for case, operation in enumerate(disk['operations']):
                job += (f'dd if={source} of={device} bs={operation["bytes"]} count=1 '
                        f'iflag=fullblock oflag=seek_bytes seek={operation["offset"]} conv=notrunc\n')
                job += check_hash(device, 6144, 2, operation['sha256'])
                job += f'echo AHCI_UNALIGNED_PASS disk={i} case={case}\n'
            job += f'{HELPER} flush {device}\n'
        job += f'echo AHCI_IO_PASS disk={i} phase={phase}\n'
        (root / f'{phase}-disk-{i}.sh').write_text(job)
        commands += ['(\n' + job + f') > /boot/home/ahci-io-{i}.log 2>&1 &', f'pid{i}=$!']
    commands += ['status=0', 'wait "$pid0" || status=1', 'wait "$pid1" || status=1',
                 'cat /boot/home/ahci-io-0.log /boot/home/ahci-io-1.log', '[ "$status" = 0 ]']
    transcript = root / (phase + '-io.txt')
    shell.execute(client, '\n'.join(commands) + '\n', transcript, credentials, timeout=180)
    text = transcript.read_text()
    for disk in fixture['disks']:
        i = disk['index']
        if f'AHCI_IO_PASS disk={i} phase={phase}' not in text:
            raise RuntimeError('Missing AHCI I/O completion')
        if not after_reboot:
            for case in range(len(disk['operations'])):
                if f'AHCI_UNALIGNED_PASS disk={i} case={case}' not in text:
                    raise RuntimeError('Missing AHCI unaligned I/O check')
            if f'AHCI_FLUSH path={disk["guest_device"]} method=B_FLUSH_DRIVE_CACHE status=pass' not in text:
                raise RuntimeError('Missing explicit AHCI drive-cache flush')
    lab.save(root / 'fixture.json', fixture)
    return dict(status='pass', disks=2, phase=phase, transcript=str(transcript), inventory=str(inventory))


def verify_host(fixture):
    hashes = []
    for disk in fixture['disks']:
        path = Path(disk['disk'])
        if path.stat().st_size != disk['bytes']:
            raise RuntimeError('AHCI fixture size changed')
        with path.open('rb') as stream:
            for offset, size, expected in ((0, 8 * MIB, disk['head_sha256']),
                    (4096 * MIB, 8 * MIB, disk['head_sha256']),
                    (6144 * MIB, 2 * MIB, disk['final_guard_sha256'])):
                stream.seek(offset)
                data = stream.read(size)
                digest = hashlib.sha256(data).hexdigest()
                if len(data) != size or digest != expected:
                    raise RuntimeError('Independent AHCI backing-file readback failed')
                hashes.append(dict(disk=disk['index'], offset=offset, bytes=size, sha256=digest))
    return dict(status='pass', hashes=hashes)
