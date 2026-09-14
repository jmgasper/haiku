import re
from pathlib import Path

def profile_commands(manifest, name, mode, options, seconds):
    profile = '/boot/system/bin/profile'
    probe = '/boot/home/config/non-packaged/bin/rock5_profile_probe'
    expected = manifest['arm64_profile_test']
    commands = "sha256sum -c <<'ROCK5_PROFILE_HASHES'\n"
    for path, key in [('/boot/system/kernel_arm64', 'kernel_sha256'),
                      (profile, 'profile_sha256'), (probe, 'probe_sha256')]:
        commands += expected[key] + '  ' + path + '\n'
    commands += 'ROCK5_PROFILE_HASHES\n'
    commands += f'{profile} {options} -i 2000 {probe} {mode} {seconds}\n'
    commands += f'echo ROCK5_PROFILE_COMPLETED_{name}\n'
    return commands

def validate_profile(evidence, name, mode, options, seconds):
    body = evidence.read_text()
    for component in ('/boot/system/kernel_arm64', '/boot/system/bin/profile',
            '/boot/home/config/non-packaged/bin/rock5_profile_probe'):
        assert component + ': OK\n' in body, (component, evidence)
    assert 'FAILED' not in body and 'WORKLOAD_FAIL' not in body, evidence
    assert f'ROCK5_PROFILE_WORKLOAD_PASS mode={mode} ' in body, evidence
    assert f'ROCK5_PROFILE_COMPLETED_{name}' in body, evidence
    assert not re.search(r'Failed to (?:start profiling|get next sample buffer)|system profiler: dropped', body), evidence
    blocks = re.split(r'(?=profiling results for thread )', body)
    blocks = [b for b in blocks if re.match(r'profiling results for thread "rock5_profile_probe"', b)]
    assert len(blocks) == 1, ('workload result section', evidence)
    block = blocks[0]
    symbols = {}
    for match in re.finditer(r'^\s+(\d+)\s+\d+\s+\d+\.\d+\s+\d+\s+([^\r\n]+)$', block, re.M):
        symbols[match[2].strip()] = int(match[1])
    expected_symbol = ('rock5_profile_user_leaf' if mode == 'user' else
                       ('_user_get_thread_info' if '-k' in options else 'main') if mode == 'syscalls' else 'rock5_profile_bad_fp')
    assert symbols.get(expected_symbol, 0) >= 10, (expected_symbol, symbols, evidence)
    if mode == 'user' and '-f' in options:
        for caller in ('rock5_profile_user_middle', 'rock5_profile_user_outer'):
            assert symbols.get(caller, 0) >= symbols[expected_symbol] * 0.8, (caller, symbols, evidence)
    if '-k' not in options:
        assert 'kernel_arm64' not in block, evidence
    if mode == 'syscalls':
        # The frameless SVC wrapper saves the syscall loop's FP, so its record
        # returns to main. Do not require the omitted immediate caller.
        assert symbols.get('main', 0) >= 10, (symbols, evidence)
        copies = re.search(r'WORKLOAD_PASS mode=syscalls loops=\d+ invalid_copies=(\d+)', body)
        assert copies and int(copies[1]) >= 1024, evidence
    if mode == 'syscalls' and '-k' in options:
        assert 'kernel_arm64' in block, evidence
        assert any(n > 0 and re.search(r'_user_get_thread_info|copy_thread_info|user_memcpy', s)
                   for s, n in symbols.items()), (symbols, evidence)
    item = dict(status='pass', name=name, mode=mode, options=options, seconds=seconds,
                transcript=str(evidence), workload_symbols=symbols)
    return item


def teardown_commands(manifest, name):
    expected = manifest['arm64_profile_test']
    paths = [('/boot/system/kernel_arm64', 'kernel_sha256'),
        ('/boot/system/bin/profile', 'profile_sha256'),
        ('/boot/home/config/non-packaged/bin/rock5_platform_probe', 'platform_sha256')]
    commands = "set -e\nset -o pipefail\nsha256sum -c <<'ROCK5_TEARDOWN_HASHES'\n"
    commands += ''.join(expected[key] + '  ' + path + '\n' for path, key in paths)
    commands += 'ROCK5_TEARDOWN_HASHES\n'
    commands += '/boot/system/bin/profile -a -k -f -s 32 -i 2000 '
    commands += "/boot/system/bin/bash -c 'set -e; for round in 1 2 3; do "
    commands += 'echo ROCK5_PROFILE_PROCESS_ROUND_$round; '
    commands += "/boot/home/config/non-packaged/bin/rock5_platform_probe 1; done'\n"
    commands += 'echo ROCK5_TEARDOWN_COMPLETED_' + name + '\n'
    return commands


def validate_teardown(evidence, name):
    body = evidence.read_text()
    for component in ('/boot/system/kernel_arm64', '/boot/system/bin/profile',
            '/boot/home/config/non-packaged/bin/rock5_platform_probe'):
        assert component + ': OK\n' in body, (component, evidence)
    assert body.count('ROCK5_PLATFORM_PASS') == 3, evidence
    for round_number in range(1, 4):
        assert 'ROCK5_PROFILE_PROCESS_ROUND_' + str(round_number) in body
    assert 'ROCK5_TEARDOWN_COMPLETED_' + name in body
    assert not re.search(r'Failed to (?:start profiling|get next sample buffer)|system profiler: dropped|ROCK5_PLATFORM_FAIL', body)
    assert 'kernel_arm64' in body and 'profiling results for thread ' in body
    return dict(status='pass', name=name, rounds=3, fork_exec_checks=96,
        recovered_faults=24, transcript=str(evidence))
