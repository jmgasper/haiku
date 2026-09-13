"""Bounded FAT file writes, fresh mounts, explicit MMC flush and host readback."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess

import lab
import shell


MIB = 1024 * 1024
CARD_BYTES = 7818182656
PARTITION_OFFSET = 32 * MIB
PARTITION_BYTES = 300 * MIB
FAT_DRIVER = '/boot/system/add-ons/kernel/file_systems/fat'
HELPER = '/boot/home/config/non-packaged/bin/rock5_mmc_probe'


def patterns():
    source = hashlib.shake_256(b'ROCK5 MMC FAT source v1').digest(8 * MIB)
    initial = hashlib.shake_256(b'ROCK5 MMC FAT target v1').digest(16 * MIB)
    current = bytearray(initial)
    operations = []
    for offset, size in ((4 * MIB, 8 * MIB),
            *((2 * MIB + start, size) for start, size in
              ((17, 1), (127, 513), (541, 4097), (1031, 131073), (4099, 1048579)))):
        assert 0 <= offset < offset + size <= len(current) and size <= len(source)
        current[offset:offset + size] = source[:size]
        operations.append(dict(offset=offset, bytes=size,
            sha256=hashlib.sha256(current).hexdigest()))
    return source, initial, bytes(current), operations


def run_tool(args, log):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=90)
    Path(log).write_bytes(result.stdout)
    result.check_returncode()


def prepare(output):
    root = output / 'mmc-filesystem'
    root.mkdir()
    source, initial, final, operations = patterns()
    (root / 'source.bin').write_bytes(source)
    (root / 'target-initial.bin').write_bytes(initial)
    partition = root / 'fat-initial.img'
    with partition.open('xb') as stream:
        stream.truncate(PARTITION_BYTES)
    formatter = shutil.which('mkfs.fat') or '/usr/sbin/mkfs.fat'
    run_tool([formatter, '-F', '32', '-s', '8', '-n', 'MMC_TEST', '-i', '10448E11',
        str(partition)], root / 'format.log')
    run_tool(['mmd', '-i', str(partition), '::/HAIKUTST'], root / 'mkdir.log')
    for name, local in (('SOURCE.BIN', 'source.bin'), ('TARGET.BIN', 'target-initial.bin')):
        run_tool(['mcopy', '-i', str(partition), str(root / local), '::/HAIKUTST/' + name],
            root / (name + '-copy.log'))
    disk = root / 'emmc-filesystem.img'
    mbr = bytearray(512)
    # One FAT partition. The native fixture uses the board's existing GPT
    # partition instead; its exact device is independently checked there.
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x00\x00', 0x0c,
        b'\xfe\xff\xff', PARTITION_OFFSET // 512, PARTITION_BYTES // 512)
    mbr[510:] = b'\x55\xaa'
    with disk.open('xb') as stream:
        stream.truncate(CARD_BYTES)
        stream.write(mbr)
        stream.seek(PARTITION_OFFSET)
        with partition.open('rb') as data:
            shutil.copyfileobj(data, stream, MIB)
        stream.flush()
        os.fsync(stream.fileno())
    fixture = dict(evidence=str(root), disk=str(disk), bytes=CARD_BYTES,
        partition_offset=PARTITION_OFFSET, partition_bytes=PARTITION_BYTES,
        mbr_sha256=hashlib.sha256(mbr).hexdigest(),
        source_sha256=hashlib.sha256(source).hexdigest(),
        initial_sha256=hashlib.sha256(initial).hexdigest(),
        final_sha256=hashlib.sha256(final).hexdigest(), operations=operations,
        written_bytes=sum(item['bytes'] for item in operations))
    lab.save(root / 'fixture.json', fixture)
    return fixture


def command(fixture):
    return ['-device', 'sdhci-pci,id=mmc2',
        '-drive', f'file={fixture["disk"]},if=none,id=mmc-file-disk,format=raw,cache=writeback',
        '-device', 'emmc,drive=mmc-file-disk,bus=pcie.0/mmc2/sd-bus']


def script(fixture, device, partition, fat_sha256, after_reboot=False):
    # Mounts are explicit; every write targets the pre-existing regular file.
    # The full-file hash checks both data and all untouched surrounding bytes.
    mount = '/mmc-file-test'
    source = mount + '/HAIKUTST/SOURCE.BIN'
    target = mount + '/HAIKUTST/TARGET.BIN'
    commands = ['set -e', 'set -o pipefail', 'umask 077',
        f'actual=$(sha256sum {FAT_DRIVER})', f'[ "${{actual%% *}}" = {fat_sha256} ]',
        f'geometry=$({HELPER} geometry {shlex.quote(device)})', 'echo "$geometry"',
        f'case "$geometry" in *"sector=512 bytes={fixture["bytes"]} readonly=0"*) ;; *) exit 1 ;; esac',
        f'part_geometry=$({HELPER} geometry {shlex.quote(partition)})', 'echo "$part_geometry"',
        f'case "$part_geometry" in *"sector=512 bytes={fixture["partition_bytes"]} readonly=0"*) ;; *) exit 1 ;; esac',
        f'mkdir -p {mount}', f'mount -ro {shlex.quote(partition)} {mount}']

    def hashes(expected):
        return [f'test -f {source}', f'test -f {target}',
            f'[ "$(wc -c < {source})" = 8388608 ]', f'[ "$(wc -c < {target})" = 16777216 ]',
            f'actual=$(sha256sum {source})', f'[ "${{actual%% *}}" = {fixture["source_sha256"]} ]',
            f'actual=$(sha256sum {target})', f'echo "MMC_FILE_HASH $actual"',
            f'[ "${{actual%% *}}" = {expected} ]']

    commands += hashes(fixture['final_sha256'] if after_reboot else fixture['initial_sha256'])
    commands.append(f'unmount {mount}')
    if not after_reboot:
        for case, operation in enumerate(fixture['operations']):
            commands += [f'mount {shlex.quote(partition)} {mount}',
                f'dd if={source} of={target} bs={operation["bytes"]} count=1 '
                f'iflag=fullblock oflag=seek_bytes seek={operation["offset"]} conv=notrunc',
                'sync', f'unmount {mount}', f'{HELPER} flush {shlex.quote(device)}',
                f'mount -ro {shlex.quote(partition)} {mount}']
            commands += hashes(operation['sha256'])
            commands += [f'unmount {mount}', f'echo MMC_FILE_WRITE_PASS case={case}']
    commands.append('echo MMC_FILE_IO_PASS phase=' + ('after-reboot' if after_reboot else 'first-boot'))
    return '\n'.join(commands) + '\n'


def check(client, output, credentials, fixture, device, fat_sha256, after_reboot=False):
    root = Path(fixture['evidence'])
    phase = 'after-reboot' if after_reboot else 'first-boot'
    commands = script(fixture, device, device.removesuffix('/raw') + '/0',
        fat_sha256, after_reboot)
    (root / (phase + '.sh')).write_text(commands)
    transcript = root / (phase + '.txt')
    shell.execute(client, commands, transcript, credentials, timeout=240)
    text = transcript.read_text()
    assert 'MMC_FILE_IO_PASS phase=' + phase in text
    if not after_reboot:
        for case in range(len(fixture['operations'])):
            assert f'MMC_FILE_WRITE_PASS case={case}' in text
        assert text.count('method=B_FLUSH_DRIVE_CACHE result=pass') == len(fixture['operations'])
    return dict(status='pass', transcript=str(transcript), phase=phase)


def verify_host(fixture, initial=False, label='after-shutdown'):
    root = Path(fixture['evidence'])
    partition = root / (label + '-fat.img')
    disk = Path(fixture['disk'])
    if disk.stat().st_size != fixture['bytes']:
        raise RuntimeError('MMC filesystem card size changed')
    with disk.open('rb') as stream:
        assert hashlib.sha256(stream.read(512)).hexdigest() == fixture['mbr_sha256']
        stream.seek(fixture['partition_offset'] - MIB)
        assert stream.read(MIB) == bytes(MIB)
        with partition.open('xb') as destination:
            remaining = fixture['partition_bytes']
            while remaining:
                data = stream.read(min(MIB, remaining))
                assert data
                destination.write(data)
                remaining -= len(data)
        assert stream.read(MIB) == bytes(MIB)
    checker = shutil.which('fsck.fat') or '/usr/sbin/fsck.fat'
    run_tool([checker, '-n', str(partition)], root / (label + '-fsck.log'))
    hashes = []
    for name, size, expected in (('SOURCE.BIN', 8 * MIB, fixture['source_sha256']),
            ('TARGET.BIN', 16 * MIB, fixture['initial_sha256'] if initial else fixture['final_sha256'])):
        saved = root / (label + '-' + name)
        run_tool(['mcopy', '-i', str(partition), '::/HAIKUTST/' + name, str(saved)],
            root / (label + '-' + name + '-copy.log'))
        if saved.stat().st_size != size or lab.digest(saved) != expected:
            raise RuntimeError('Independent MMC file readback failed: ' + name)
        hashes.append(dict(name=name, bytes=size, sha256=expected))
    return dict(status='pass', hashes=hashes, filesystem_check=str(root / (label + '-fsck.log')))
