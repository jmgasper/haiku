"""Bounded live-process ASID recycling checks and independent result validation."""
from pathlib import Path
import re
import shlex


def commands(manifest, name, idle_seconds=0):
    assert type(idle_seconds) is int and 0 <= idle_seconds <= 120
    expected = manifest['arm64_asid_test']
    probe = '/boot/home/config/non-packaged/bin/rock5_asid_probe'
    root = '/boot/home/rock5-lab/asid-' + name
    text = "set -eu\nset -o pipefail\numask 077\nsha256sum -c <<'ROCK5_ASID_HASHES'\n"
    text += expected['kernel_sha256'] + '  /boot/system/kernel_arm64\n'
    text += expected['probe_sha256'] + '  ' + probe + '\nROCK5_ASID_HASHES\n'
    text += 'mkdir -p ' + shlex.quote(root) + '\n'
    for label, children, rounds, corrupt, status in [
            ('live-pool', 280, 4, False, 0),
            ('corrupt-child', 8, 2, True, 1),
            ('after-cleanup', 64, 2, False, 0)]:
        log = shlex.quote(root + '/' + label + '.txt')
        text += 'echo ROCK5_ASID_CASE_BEGIN_' + label + '\nset +e\n'
        idle = idle_seconds if label == 'live-pool' else 0
        text += 'timeout --kill-after=3s ' + str(115 + idle) + 's ' + probe
        text += ' --children ' + str(children) + ' --rounds ' + str(rounds)
        if idle:
            text += ' --idle-seconds ' + str(idle)
        if corrupt:
            text += ' --corrupt-child 3'
        text += ' > ' + log + ' 2>&1\nrock5_asid_status=$?\nset -e\ncat ' + log + '\n'
        text += 'echo ROCK5_ASID_CASE_EXIT_' + label + '=$rock5_asid_status\n'
        text += '[ "$rock5_asid_status" -eq ' + str(status) + ' ]\n'
        text += 'echo ROCK5_ASID_CASE_END_' + label + '\n'
    text += 'echo ROCK5_ASID_CHECKS_COMPLETE_' + name + '\n'
    return text


def validate(transcript, name, cpus, idle_seconds=0):
    assert type(idle_seconds) is int and 0 <= idle_seconds <= 120
    body = Path(transcript).read_text()
    for path in ('/boot/system/kernel_arm64',
                 '/boot/home/config/non-packaged/bin/rock5_asid_probe'):
        assert path + ': OK\n' in body, transcript
    assert 'ROCK5_ASID_CHECKS_COMPLETE_' + name in body
    cases = []
    for label, children, rounds, corrupt in [
            ('live-pool', 280, 4, False),
            ('corrupt-child', 8, 2, True),
            ('after-cleanup', 64, 2, False)]:
        start = 'ROCK5_ASID_CASE_BEGIN_' + label + '\n'
        end = 'ROCK5_ASID_CASE_END_' + label + '\n'
        assert body.count(start) == body.count(end) == 1
        part = body.split(start)[1].split(end)[0]
        beginning = re.search(r'^ROCK5_ASID_POOL_BEGIN children=(\d+) rounds=(\d+) '
            r'cpus=(\d+) page_bytes=(\d+) private_va=(0x[0-9a-f]+)$', part, re.M)
        assert beginning and tuple(map(int, beginning.groups()[:4])) == (children, rounds, cpus, 4096)
        assert 'ROCK5_ASID_POOL_READY created=%d expected=%d ready=%d\n' % (children, children, children) in part
        idle = idle_seconds if label == 'live-pool' else 0
        idle_elapsed = None
        if idle:
            idle_begin = f'ROCK5_ASID_IDLE_BEGIN children={children} seconds={idle}\n'
            assert part.count(idle_begin) == 1
            idle_ends = re.findall(r'^ROCK5_ASID_IDLE_END children=(\d+) elapsed_us=(\d+)$', part, re.M)
            assert len(idle_ends) == 1 and int(idle_ends[0][0]) == children
            idle_elapsed = int(idle_ends[0][1])
            assert idle * 1000000 <= idle_elapsed < (idle + 90) * 1000000
            assert part.index(idle_begin) < part.index('ROCK5_ASID_IDLE_END') < part.index('ROCK5_ASID_POOL_ROUND')
        else:
            assert 'ROCK5_ASID_IDLE_BEGIN' not in part and 'ROCK5_ASID_IDLE_END' not in part
        ending = re.search(r'^ROCK5_ASID_POOL_END children=(\d+) rounds=(\d+) '
            r'completed=(\d+) exited=(\d+) remaining=(\d+)$', part, re.M)
        assert ending
        actual_children, actual_rounds, completed, exited, remaining = map(int, ending.groups())
        assert (actual_children, actual_rounds, remaining) == (children, rounds, 0)
        if corrupt:
            assert completed == 0 and exited == 0
            assert 'ROCK5_ASID_POOL_FAIL\n' in part and 'ROCK5_ASID_POOL_PASS\n' not in part
            assert 'ROCK5_ASID_CASE_EXIT_' + label + '=1\n' in part
        else:
            assert completed == rounds and exited == children
            assert 'ROCK5_ASID_POOL_PASS\n' in part and 'ROCK5_ASID_POOL_FAIL\n' not in part
            assert 'ROCK5_ASID_CASE_EXIT_' + label + '=0\n' in part
            for round_number in range(1, rounds + 1):
                assert 'ROCK5_ASID_POOL_ROUND round=%d verified=%d expected=%d\n' % (round_number, children, children) in part
        cases.append(dict(name=label, children=children, rounds=rounds,
            completed=completed, exited=exited, remaining=remaining,
            private_va=beginning[5], expected_corruption=corrupt,
            idle_seconds=idle, idle_elapsed_us=idle_elapsed))
    return dict(status='pass', name=name, cpus=cpus, transcript=str(transcript), cases=cases)
