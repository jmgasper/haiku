"""Disjoint FAT writers with per-write readback, explicit flush and guard hashes."""
import hashlib
from pathlib import Path
import re
import shlex

import lab
import mmc_file_test as files
import shell

WORKERS = 4
ROUNDS = 16
CHUNK = 2 * files.MIB
OFFSETS = tuple((1 + 4 * worker) * files.MIB for worker in range(WORKERS))


def model(source, initial):
    if len(source) != 8 * files.MIB or len(initial) != 16 * files.MIB:
        raise ValueError('Concurrent MMC fixture sizes differ')
    final = bytearray(initial)
    operations = []
    for worker, offset in enumerate(OFFSETS):
        for round_number in range(ROUNDS):
            source_offset = ((worker + round_number) % WORKERS) * CHUNK
            data = source[source_offset:source_offset + CHUNK]
            if final[offset:offset + CHUNK] == data:
                raise ValueError('Concurrent MMC write would retain previous bytes')
            final[offset:offset + CHUNK] = data
            operations.append(dict(worker=worker, round=round_number, offset=offset,
                source_offset=source_offset, bytes=CHUNK,
                region_sha256=hashlib.sha256(data).hexdigest()))
    return bytes(final), operations


def prepare(output):
    fixture = files.prepare(output)
    root = Path(fixture['evidence'])
    final, operations = model((root / 'source.bin').read_bytes(),
        (root / 'target-initial.bin').read_bytes())
    (root / 'target-final.bin').write_bytes(final)
    fixture.update(operations=operations, final_sha256=hashlib.sha256(final).hexdigest(),
        written_bytes=WORKERS * ROUNDS * CHUNK, concurrent_workers=WORKERS,
        concurrent_rounds=ROUNDS)
    lab.save(root / 'fixture.json', fixture)
    return fixture


def script(fixture, device, partition, fat_sha256, after_reboot=False):
    def readback(expected, marker):
        current = dict(fixture, final_sha256=expected)
        return files.script(current, device, partition, fat_sha256, True).replace(
            'MMC_FILE_IO_PASS phase=after-reboot', marker)
    final_marker = 'MMC_CONCURRENT_FINAL_PASS'
    if after_reboot:
        return readback(fixture['final_sha256'], final_marker) + 'echo MMC_CONCURRENT_PASS phase=after-reboot\n'
    commands = [readback(fixture['initial_sha256'], 'MMC_CONCURRENT_PREFLIGHT_PASS'),
        'mount ' + shlex.quote(partition) + ' /mmc-file-test',
        'mmc_worker() {', '    mmc_worker_id=$1', '    mmc_offset=$2',
        '    mmc_round=0', f'    while [ "$mmc_round" -lt {ROUNDS} ]; do',
        f'        mmc_source_chunk=$(((mmc_worker_id + mmc_round) % {WORKERS}))',
        '        case "$mmc_source_chunk" in']
    chunks = {op['source_offset'] // CHUNK: op['region_sha256'] for op in fixture['operations']}
    if sorted(chunks) != list(range(WORKERS)):
        raise ValueError('Concurrent MMC source chunks missing')
    for index, digest in sorted(chunks.items()):
        if not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise ValueError('Invalid concurrent MMC region hash')
        commands.append(f'            {index}) mmc_expected={digest} ;;')
    commands += ['            *) return 1 ;;', '        esac',
        f'        dd if=/mmc-file-test/HAIKUTST/SOURCE.BIN of=/mmc-file-test/HAIKUTST/TARGET.BIN bs={CHUNK} count=1 iflag=fullblock skip="$mmc_source_chunk" oflag=seek_bytes seek="$mmc_offset" conv=notrunc',
        '        sync', f'        {files.HELPER} flush {shlex.quote(device)}',
        f'        mmc_actual=$(dd if=/mmc-file-test/HAIKUTST/TARGET.BIN bs={CHUNK} count=1 iflag=fullblock,skip_bytes skip="$mmc_offset" | sha256sum)',
        '        [ "${mmc_actual%% *}" = "$mmc_expected" ]',
        '        echo "MMC_CONCURRENT_WRITE_PASS worker=$mmc_worker_id round=$mmc_round sha256=$mmc_expected"',
        '        mmc_round=$((mmc_round + 1))', '    done', '}']
    for worker, offset in enumerate(OFFSETS):
        commands += [f'(set -e; mmc_worker {worker} {offset}) > /boot/home/mmc-concurrent-{worker}.log 2>&1 &',
            f'mmc_pid_{worker}=$!']
    commands.append('mmc_workers_status=0')
    for worker in range(WORKERS):
        commands.append(f'if wait "$mmc_pid_{worker}"; then :; else mmc_workers_status=1; fi')
    for worker in range(WORKERS):
        commands.append(f'cat /boot/home/mmc-concurrent-{worker}.log')
    commands += ['[ "$mmc_workers_status" = 0 ]', 'sync', 'unmount /mmc-file-test',
        f'{files.HELPER} flush {shlex.quote(device)}',
        readback(fixture['final_sha256'], final_marker),
        'echo MMC_CONCURRENT_PASS phase=first-boot']
    return '\n'.join(commands) + '\n'


def validate(text, fixture, after_reboot=False):
    phase = 'after-reboot' if after_reboot else 'first-boot'
    if 'MMC_CONCURRENT_PASS phase=' + phase not in text or 'MMC_CONCURRENT_FINAL_PASS' not in text:
        raise ValueError('Concurrent MMC completion missing')
    if 'MMC_FILE_HASH ' + fixture['final_sha256'] not in text:
        raise ValueError('Concurrent MMC final hash missing')
    observed = re.findall(r'^MMC_CONCURRENT_WRITE_PASS worker=(\d+) round=(\d+) sha256=([0-9a-f]{64})\s*$', text, re.M)
    expected = [] if after_reboot else [(str(op['worker']), str(op['round']), op['region_sha256'])
        for op in fixture['operations']]
    if sorted(observed) != sorted(expected):
        raise ValueError('Concurrent MMC write/readback evidence differs')
    flushes = text.count('method=B_FLUSH_DRIVE_CACHE result=pass')
    if flushes != (0 if after_reboot else len(expected) + 1):
        raise ValueError('Concurrent MMC explicit flush evidence differs')
    return dict(status='pass', phase=phase, writes=len(expected), explicit_flushes=flushes)


def check(client, output, credentials, fixture, device, fat_sha256, after_reboot=False):
    root = Path(fixture['evidence'])
    phase = 'after-reboot' if after_reboot else 'first-boot'
    commands = script(fixture, device, device.removesuffix('/raw') + '/0', fat_sha256, after_reboot)
    (root / (phase + '.sh')).write_text(commands)
    transcript = root / (phase + '.txt')
    shell.execute(client, commands, transcript, credentials, timeout=300)
    value = validate(transcript.read_text(), fixture, after_reboot)
    return dict(value, transcript=str(transcript))
