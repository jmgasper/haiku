"""Check command-fault errors, allocation restoration and fresh GLES pixels."""
import re
import pipeline_validation

IO_ERROR = -(1 << 31) + 1  # pinned Haiku Errors.h


def one(pattern, text):
    rows = re.findall('^' + pattern + r'\s*$', text, re.M)
    assert len(rows) == 1, (pattern, len(rows))
    return rows[0]


def validate(text, output=None, software=False):
    for forbidden in ('ROCK5_MALI_QUEUE_FAIL', 'ROCK5_PIPELINE_FAILURE',
            'PANIC:', 'Kernel Debugging Land', 'DEBUGGER:', 'mutex->owner'):
        assert forbidden not in text
    marker = 'ROCK5_PIPELINE_READY version=1 mode='
    assert text.count(marker) == 1
    before, graphics = text.split(marker)
    result = dict(status='pass', software=software, reset_verified=False,
        graphics=pipeline_validation.validate(marker + graphics, output, software))
    one('ROCK5_RECOVERY_GRAPHICS_PASS', graphics)
    if software:
        one('ROCK5_RECOVERY_NO_DEVICE', before)
        one('ROCK5_RECOVERY_SOFTWARE_SKIP', before)
        assert 'ROCK5_RECOVERY_NATIVE_PASS' not in text and 'ROCK5_RECOVERY_ARMED' not in text
        return result
    assert 'ROCK5_RECOVERY_NO_DEVICE' not in text and 'ROCK5_RECOVERY_SOFTWARE_SKIP' not in text
    one('ROCK5_RECOVERY_NATIVE_PASS contexts=2 failed_jobs=3 fences=3 fresh_stores=2', before)
    armed = one(r'ROCK5_RECOVERY_ARMED a=(\d+) b=(\d+) instruction=ff00000000000000 '
        r'fault_sequence=34 a_submitted=35 b_submitted=2 a_pending=(\d+) b_pending=1', before)
    a, b, pending = map(int, armed)
    assert 0 < a < b and 2 <= pending <= 34
    fences = re.findall(r'^ROCK5_RECOVERY_FENCE index=(\d+) result=(-?\d+)\s*$', before, re.M)
    assert [(int(i), int(e)) for i, e in fences] == [(i, IO_ERROR) for i in range(3)]
    rows = re.findall(r'^ROCK5_RECOVERY_QUEUE handle=(\d+) submitted=(\d+) completed=(\d+) '
        r'failed=(\d+) pending=(\d+) state=(\d+) error=(-?\d+) wait=(-?\d+)\s*$', before, re.M)
    assert [tuple(map(int, row)) for row in rows] == [
        (a, 35, 33, 34, 2, 3, IO_ERROR, IO_ERROR), (b, 2, 1, 2, 1, 3, IO_ERROR, IO_ERROR)]
    elapsed = int(one(r'ROCK5_RECOVERY_CLOSED elapsed_us=(\d+) contexts=2 fences=3', before))
    assert 0 < elapsed < 10_000_000
    fresh = int(one(r'ROCK5_RECOVERY_FRESH handle=(\d+) completed=2 words=4096', before))
    assert fresh > b
    snapshots = []
    for body in re.findall(r'^ROCK5_MESA_NATIVE_LIFETIME (.*)$', before, re.M):
        snapshots.append({k: int(v) for k, v in (item.split('=') for item in body.split())})
    assert [s.pop('cycle') for s in snapshots] == [0, 1]
    assert snapshots[0] == snapshots[1] == result['graphics']['allocation_baseline']
    assert before.index('ROCK5_RECOVERY_ARMED ') < before.index('ROCK5_RECOVERY_FENCE ')
    assert before.index('ROCK5_RECOVERY_QUEUE ') < before.index('ROCK5_RECOVERY_CLOSED ')
    assert before.index('ROCK5_RECOVERY_CLOSED ') < before.index('ROCK5_RECOVERY_FRESH ')
    result.update(fault_queue=a, affected_queue=b, fresh_queue=fresh,
        fault_sequence=34, failed_jobs=3, error_fences=3, raw_submissions=39,
        raw_successful_completions=36, allocation_baseline=snapshots[0], elapsed_us=elapsed)
    return result


def validate_uart(result, uart):
    assert not result['software']
    queues = re.findall(r'^mali_csf: recovery queue handle=(\d+) submitted=(\d+) '
        r'completed=(\d+) current=(\d+) pending=(\d+)\s*$', uart, re.M)
    assert [tuple(map(int, q)) for q in queues] == [
        (result['fault_queue'], 35, 33, 34, 2), (result['affected_queue'], 2, 1, 0, 1)]
    row = one(r'mali_csf: recovery result=(\d+) reset=(\d+)/(\d+) flags=(\S+) '
        r'irq=(\d+)/(\S+)/(\S+) quiescent=(\d+) restored=(\d+) recovered=(\d+) '
        r'job=(\S+) mmu=(\S+)', uart)
    values = [int(x, 0) for x in row]
    code, reset, cleanup, flags, count, irq, raw, quiet, restored, recovered, job, mmu = values
    assert (code, reset, cleanup, flags, count, irq) == (0, 0, 0, 7, 1, 256)
    assert raw & 0x103 == 0x100
    assert (quiet, restored, recovered, job, mmu) == (1, 1, 1, 0, 3 << 16)
    timing = one(r'mali_csf: recovery time start=(\d+) irq=(\d+) end=(\d+) cpu=(\d+) '
        r'power=(\d+)/(\d+)/(\S+)', uart)
    start, event, end, cpu, power, restore, power_flags = [int(x, 0) for x in timing]
    assert 0 <= start <= event <= end and event - start <= 100000 and 0 <= cpu < 8
    assert power == 14 and restore == 0 and power_flags == 7
    body = one(r'mali_csf: recovery idle (.*)', uart)
    idle = {k: tuple(int(x, 0) for x in v.split('/')) for k, v in
        (item.split('=') for item in body.split())}
    assert set(idle) == set('mask raw irq status mcu job_mask mmu_mask shader tiler l2 transition high'.split())
    assert idle['raw'][0] & 0x103 == 0 and idle['status'][0] & 0x93 == 0
    for key, value in idle.items():
        if key in ('raw', 'status'):
            assert len(value) == 1
        else:
            assert value == (0,) * (2 if key in ('shader', 'tiler', 'l2') else
                3 if key in ('transition', 'high') else 1)
    spaces = re.findall(r'^mali_csf: recovery AS(\d+) table=(\S+) attributes=(\S+) '
        r'config=(\S+) status=(\S+)\s*$', uart, re.M)
    assert [tuple(int(x, 0) for x in row) for row in spaces] == [(0, 0, 0, 1, 0), (1, 0, 0, 1, 0)]
    runtimes = re.findall(r'^mali_csf: runtime status=(-?\d+) engine=(\d+) fw=(\d+)/(\d+) '
        r'flags=(\S+) fault=(\S+) fatal=(\S+) address=(\S+) retained=(\d+)\s*$', uart, re.M)
    failed = [(i, tuple(int(x, 0) for x in row)) for i, row in enumerate(runtimes)
        if tuple(int(x, 0) for x in row[:4]) != (0, 0, 0, 0)]
    assert len(failed) == 1
    index, runtime = failed[0]
    status, engine, firmware, cleanup, flags, fault, fatal, address, retained = runtime
    assert status == IO_ERROR and engine != 0 and firmware == 16 and cleanup != 0
    assert flags & 1024 and flags & 128 and flags & (256 | 512) == 0
    assert fault != 0 or fatal != 0
    assert retained == 0 and index + 3 < len(runtimes)
    for row in runtimes[index+1:index+4]:
        assert tuple(int(x, 0) for x in row) == (0, 0, 0, 0, 255, 0, 0, 0, 0)
    return dict(status='pass', reset_verified=True, reset_irq_us=event-start,
        reset_elapsed_us=end-start, cpu=cpu, runtime_index=index, failed_runtime=list(runtime),
        final_idle=idle, address_spaces=spaces, affected_queues=queues,
        fresh_runtimes_after_fault=3)
