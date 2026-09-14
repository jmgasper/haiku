"""Verify image notifications for children started after system sampling begins."""
from pathlib import Path
import re
import shlex


def child_commands(manifest, name, full_stack=False):
    expected = manifest['arm64_profile_test']
    profile = '/boot/system/bin/profile'
    probe = '/boot/home/config/non-packaged/bin/rock5_profile_probe'
    root = '/boot/home/rock5-lab/image-events-' + name
    command = "set -e\nset -o pipefail\numask 077\nsha256sum -c <<'ROCK5_CHILD_HASHES'\n"
    for path, key in [('/boot/system/kernel_arm64', 'kernel_sha256'),
            (profile, 'profile_sha256'), (probe, 'probe_sha256')]:
        command += expected[key] + '  ' + path + '\n'
    command += 'ROCK5_CHILD_HASHES\nmkdir -p ' + shlex.quote(root) + '\n'
    workload = ['set -eu', 'sleep 1']
    for index in range(4):
        workload += [probe + ' user 3 > ' + shlex.quote(root + '/child-' + str(index) + '.txt') + ' 2>&1 &',
                     'rock5_child_' + str(index) + '=$!']
    workload += ['rock5_children_status=0']
    for index in range(4):
        workload += ['wait "$rock5_child_' + str(index) + '" || rock5_children_status=1']
    workload += ['cat ' + ' '.join(shlex.quote(root + '/child-' + str(i) + '.txt') for i in range(4)),
                 '[ "$rock5_children_status" -eq 0 ]']
    command += 'cat > ' + shlex.quote(root + '/workload.sh') + " <<'ROCK5_CHILD_WORKLOAD'\n"
    command += '\n'.join(workload) + '\nROCK5_CHILD_WORKLOAD\n'
    options = '-a -k -f -s 32' if full_stack else '-a -k -s 1'
    command += profile + ' ' + options + ' -i 2000 /boot/system/bin/bash ' + shlex.quote(root + '/workload.sh') + '\n'
    command += 'echo ROCK5_PROFILE_CHILDREN_COMPLETE_' + name + '\n'
    return command


def validate_children(transcript, name, full_stack=False):
    body = Path(transcript).read_text()
    for path in ('/boot/system/kernel_arm64', '/boot/system/bin/profile',
            '/boot/home/config/non-packaged/bin/rock5_profile_probe'):
        assert path + ': OK\n' in body, transcript
    assert body.count('ROCK5_PROFILE_WORKLOAD_PASS mode=user ') == 4, transcript
    assert 'ROCK5_PROFILE_CHILDREN_COMPLETE_' + name in body
    assert not re.search(r'Failed to (?:start profiling|get next sample buffer|init symbol iterator)|system profiler: dropped|ROCK5_PROFILE_WORKLOAD_FAIL', body)
    blocks = [b for b in re.split(r'(?=profiling results for thread )', body)
        if b.startswith('profiling results for thread "rock5_profile_probe"')]
    assert len(blocks) == 4, (len(blocks), transcript)
    children = []
    for block in blocks:
        identity = re.match(r'profiling results for thread "rock5_profile_probe" \((\d+)\):', block)
        ticks = re.search(r'total ticks:\s+(\d+)', block)
        unknown = re.search(r'unknown ticks:\s+(\d+)', block)
        dropped = re.search(r'dropped ticks:\s+(\d+)', block)
        assert identity and ticks and unknown and dropped
        symbols = {name.strip(): int(hits) for hits, name in re.findall(
            r'^\s+(\d+)\s+\d+\s+\d+\.\d+\s+\d+\s+([^\r\n]+)$', block, re.M)}
        assert int(ticks[1]) >= 20 and int(dropped[1]) == 0
        assert int(unknown[1]) <= max(3, int(ticks[1]) // 50), (block, transcript)
        assert symbols.get('rock5_profile_user_leaf', 0) >= 10, (symbols, transcript)
        if full_stack:
            for caller in ('rock5_profile_user_middle', 'rock5_profile_user_outer'):
                assert symbols.get(caller, 0) >= symbols['rock5_profile_user_leaf'] * .8
        children.append(dict(thread_id=int(identity[1]), ticks=int(ticks[1]),
            unknown_ticks=int(unknown[1]), dropped_ticks=int(dropped[1]), symbols=symbols))
    assert len({c['thread_id'] for c in children}) == 4
    return dict(status='pass', name=name, full_stack=full_stack,
        transcript=str(transcript), children=children)
