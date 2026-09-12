"""Hash-checked I/O on a disposable QEMU NVMe namespace."""

import hashlib
import os
from pathlib import Path

import lab
import shell


REGION_BYTES = 8 * 1024 * 1024
HIGH_OFFSET = 4 * 1024 * 1024 * 1024
DISK_BYTES = 8 * 1024 * 1024 * 1024
DEVICE = '/dev/disk/nvme/0/raw'
GUARD_OFFSET = 6 * 1024 * 1024 * 1024
GUARD_BYTES = 2 * 1024 * 1024


def prepare(output):
    """Create a new sparse backing file; never accept an existing disk path."""
    disk = output / 'nvme-namespace.img'
    hashes = []
    with disk.open('xb') as stream:
        stream.truncate(DISK_BYTES)
        for offset, name in ((0, 'first'), (HIGH_OFFSET, 'second')):
            data = hashlib.shake_256(
                f'ROCK5 QEMU NVMe fixture {name} v1'.encode()).digest(REGION_BYTES)
            hashes.append(hashlib.sha256(data).hexdigest())
            stream.seek(offset)
            stream.write(data)
        guard = bytearray(hashlib.shake_256(
            b'ROCK5 QEMU NVMe sector guards v1').digest(GUARD_BYTES))
        stream.seek(GUARD_OFFSET)
        stream.write(guard)
        stream.flush()
        os.fsync(stream.fileno())
    unaligned = {'offset': GUARD_OFFSET, 'bytes': GUARD_BYTES,
                 'initial_sha256': hashlib.sha256(guard).hexdigest(), 'writes': []}
    source = hashlib.shake_256(b'ROCK5 QEMU NVMe fixture first v1').digest(REGION_BYTES)
    for offset, length in ((17, 1), (127, 513), (541, 4097),
                           (1031, 131073), (4099, 1048579)):
        guard[offset:offset + length] = source[:length]
        unaligned['writes'].append({'offset': GUARD_OFFSET + offset, 'bytes': length,
                                    'guard_sha256': hashlib.sha256(guard).hexdigest()})
    expected = output / 'nvme-unaligned-expected.bin'
    expected.write_bytes(guard)
    unaligned.update(expected_file=str(expected), final_sha256=hashlib.sha256(guard).hexdigest())
    fixture = {'disk': str(disk), 'bytes': DISK_BYTES, 'region_bytes': REGION_BYTES,
               'offsets': [0, HIGH_OFFSET], 'initial_sha256': hashes,
               'guest_device': DEVICE, 'serial': 'ROCK5-QEMU-NVME-1',
               'unaligned': unaligned}
    lab.save(output / 'nvme-fixture.json', fixture)
    return fixture


def read_commands(fixture, high_hash):
    return f'''set -o pipefail
test -e {DEVICE}
nvme_hash=$(dd if={DEVICE} bs=1048576 count=8 | sha256sum)
[ "${{nvme_hash%% *}}" = {fixture['initial_sha256'][0]} ]
nvme_hash=$(dd if={DEVICE} bs=1048576 skip=4096 count=8 | sha256sum)
[ "${{nvme_hash%% *}}" = {high_hash} ]
echo ROCK5_QEMU_NVME_READ_PASS
'''


def check_initial(client, output, credentials, fixture):
    first_hash, high_hash = fixture['initial_sha256']
    commands = 'ls -R /dev/disk\n' + read_commands(fixture, high_hash)
    commands += f'''dd if={DEVICE} of=/boot/home/nvme-source.bin bs=1048576 count=8
nvme_hash=$(sha256sum /boot/home/nvme-source.bin)
[ "${{nvme_hash%% *}}" = {first_hash} ]
dd if=/boot/home/nvme-source.bin of={DEVICE} bs=1048576 seek=4096 count=8 conv=notrunc,fsync
echo ROCK5_QEMU_NVME_WRITE_PASS
'''
    commands += read_commands(fixture, first_hash)
    transcript = output / 'nvme-io.txt'
    shell.execute(client, commands, transcript, credentials, timeout=120)
    text = transcript.read_text()
    if text.count('ROCK5_QEMU_NVME_READ_PASS') != 2 \
            or 'ROCK5_QEMU_NVME_WRITE_PASS' not in text:
        raise RuntimeError('Missing NVMe read/write evidence')
    result = {'reads_and_flushed_write': 'pass', 'written_bytes': REGION_BYTES}
    result['unaligned_io'] = check_unaligned(client, output, credentials, fixture)
    return result


def guard_read_commands(fixture, expected):
    guard = fixture['unaligned']
    return f'''nvme_hash=$(dd if={DEVICE} bs=1048576 skip={guard['offset'] // 1048576} count=2 | sha256sum)
[ "${{nvme_hash%% *}}" = {expected} ]
'''


def check_unaligned(client, output, credentials, fixture):
    guard = fixture['unaligned']
    commands = 'set -o pipefail\n' + guard_read_commands(fixture, guard['initial_sha256'])
    for index, operation in enumerate(guard['writes']):
        commands += f'''dd if=/boot/home/nvme-source.bin of={DEVICE} bs={operation['bytes']} count=1 iflag=fullblock oflag=seek_bytes seek={operation['offset']} conv=notrunc,fsync
'''
        commands += guard_read_commands(fixture, operation['guard_sha256'])
        commands += f'echo ROCK5_QEMU_NVME_UNALIGNED_PASS case={index}\n'
    transcript = output / 'nvme-unaligned.txt'
    shell.execute(client, commands, transcript, credentials, timeout=120)
    if transcript.read_text().count('ROCK5_QEMU_NVME_UNALIGNED_PASS case=') != len(guard['writes']):
        raise RuntimeError('Missing unaligned NVMe write/readback evidence')
    return {'status': 'pass', 'cases': len(guard['writes']), 'guard_bytes': guard['bytes']}


def check_after_reboot(client, output, credentials, fixture):
    commands = read_commands(fixture, fixture['initial_sha256'][0])
    commands += guard_read_commands(fixture, fixture['unaligned']['final_sha256'])
    commands += 'echo ROCK5_QEMU_NVME_GUARDS_REBOOT_PASS\n'
    transcript = output / 'nvme-after-reboot.txt'
    shell.execute(client, commands, transcript, credentials, timeout=60)
    if 'ROCK5_QEMU_NVME_READ_PASS' not in transcript.read_text():
        raise RuntimeError('Missing NVMe readback after reboot')
    if 'ROCK5_QEMU_NVME_GUARDS_REBOOT_PASS' not in transcript.read_text():
        raise RuntimeError('Missing NVMe sector guard readback after reboot')
    return {'readback_after_reboot': 'pass', 'sector_guards_after_reboot': 'pass'}


def verify_host(fixture):
    """Independently check the namespace after the emulator has powered off."""
    disk = Path(fixture['disk'])
    if disk.stat().st_size != fixture['bytes']:
        raise RuntimeError('NVMe backing file size changed')
    hashes = []
    with disk.open('rb') as stream:
        for offset in fixture['offsets']:
            stream.seek(offset)
            data = stream.read(fixture['region_bytes'])
            if len(data) != fixture['region_bytes']:
                raise RuntimeError('Truncated NVMe backing file region')
            hashes.append(hashlib.sha256(data).hexdigest())
        guard = fixture['unaligned']
        stream.seek(guard['offset'])
        guard_data = stream.read(guard['bytes'])
    if hashes != [fixture['initial_sha256'][0]] * 2:
        raise RuntimeError('Host NVMe data differs after guest shutdown')
    if len(guard_data) != guard['bytes'] or hashlib.sha256(guard_data).hexdigest() != guard['final_sha256']:
        raise RuntimeError('Host NVMe unaligned data or sector guards differ after shutdown')
    return {'host_readback_after_shutdown': 'pass', 'host_region_sha256': hashes,
            'host_sector_guard_sha256': hashlib.sha256(guard_data).hexdigest()}
