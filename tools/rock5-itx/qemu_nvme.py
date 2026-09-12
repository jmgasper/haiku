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
        stream.flush()
        os.fsync(stream.fileno())
    fixture = {'disk': str(disk), 'bytes': DISK_BYTES, 'region_bytes': REGION_BYTES,
               'offsets': [0, HIGH_OFFSET], 'initial_sha256': hashes,
               'guest_device': DEVICE, 'serial': 'ROCK5-QEMU-NVME-1'}
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
    return {'reads_and_flushed_write': 'pass', 'written_bytes': REGION_BYTES}


def check_after_reboot(client, output, credentials, fixture):
    commands = read_commands(fixture, fixture['initial_sha256'][0])
    transcript = output / 'nvme-after-reboot.txt'
    shell.execute(client, commands, transcript, credentials, timeout=60)
    if 'ROCK5_QEMU_NVME_READ_PASS' not in transcript.read_text():
        raise RuntimeError('Missing NVMe readback after reboot')
    return {'readback_after_reboot': 'pass'}


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
    if hashes != [fixture['initial_sha256'][0]] * 2:
        raise RuntimeError('Host NVMe data differs after guest shutdown')
    return {'host_readback_after_shutdown': 'pass', 'host_region_sha256': hashes}
