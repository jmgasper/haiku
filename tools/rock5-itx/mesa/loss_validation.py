"""Check shared-context loss, ignored readbacks, cleanup and fresh rendering."""
import hashlib
from pathlib import Path
import re

import pipeline_validation
from recovery_validation import IO_ERROR, one


def validate(text, output=None, software=False):
    for forbidden in ('ROCK5_MALI_QUEUE_FAIL', 'ROCK5_LOSS_SHADER_ERROR',
            'ROCK5_PIPELINE_FAILURE', 'HAIKU_MESA_NATIVE_ERROR', 'PANIC:',
            'Kernel Debugging Land', 'DEBUGGER:', 'mutex->owner'):
        assert forbidden not in text
    marker = 'ROCK5_PIPELINE_READY version=1 mode='
    assert text.count(marker) == 1
    before, graphics = text.split(marker)
    result = dict(status='pass', software=software, reset_verified=False,
        graphics=pipeline_validation.validate(marker + graphics, output, software))
    one('ROCK5_LOSS_GRAPHICS_PASS', graphics)
    one(r'ROCK5_LOSS_PASS contexts=2 shared=1 before_pixels=12998 before_guards=256 '
        + ('notifications=0 unchanged_bytes=0 software=1' if software else
           'notifications=2 unchanged_bytes=52248 software=0'), before)
    contexts = re.findall(r'^ROCK5_LOSS_CONTEXT child=(\d+) shared=1 strategy=(\S+) renderer=(.*?)\s*$', before, re.M)
    assert contexts == [(str(i), '0000' if software else '8252',
        'softpipe' if software else 'Mali-G610 (Panfrost)') for i in range(2)]
    assert re.findall(r'^ROCK5_LOSS_READY child=(\d+) pixels=6499 guards=128\s*$', before, re.M) == ['0', '1']
    pattern = (r'^ROCK5_LOSS_PIXELS_BEGIN child=(\d+) kind=(\w+) seed=(\d+) width=97 height=67 canary=([0-9a-f]{2})\n'
        r'(.*?)^ROCK5_LOSS_PIXELS_END child=(\d+) kind=(\w+)\s*$')
    frames = re.findall(pattern, before, re.M | re.S)
    expected = [(i, 'before') for i in range(2)]
    if not software:
        expected += [(i, 'ignored') for i in range(2)]
    assert len(frames) == before.count('ROCK5_LOSS_PIXELS_BEGIN ') == before.count('ROCK5_LOSS_PIXELS_END ') == len(expected)
    images = []
    for record, (child, kind) in zip(frames, expected):
        c, k, seed, canary, body, end_child, end_kind = record
        assert (int(c), k) == (int(end_child), end_kind) == (child, kind)
        assert int(seed) == (1 if child == 0 else 6)
        assert int(canary, 16) == (0xa0 if kind == 'before' else 0xd0) + child
        rows = body.splitlines()
        assert len(rows) == 69
        assert rows[0] == 'guard_before=' + canary * 64
        assert rows[-1] == 'guard_after=' + canary * 64
        raw = bytearray()
        for y, row in enumerate(rows[1:-1]):
            assert re.fullmatch(r'row=\d{3} [0-9a-f]{776}', row)
            assert row.startswith(f'row={y:03d} ')
            raw.extend(bytes.fromhex(row[8:]))
        if kind == 'before':
            # Reuse the independently checked texture-pattern oracle.
            assert raw == pipeline_validation.expected(int(seed), 'texture')
        else:
            assert raw == bytes([int(canary, 16)]) * (97 * 67 * 4)
        image = dict(child=child, kind=kind, pixels=6499, guard_bytes=128,
            rgba_sha256=hashlib.sha256(raw).hexdigest())
        if output:
            root = Path(output); root.mkdir(parents=True, exist_ok=True)
            saved = root / f'loss-{child}-{kind}.rgba'; saved.write_bytes(raw)
            image['rgba'] = str(saved)
        images.append(image)
    result.update(contexts=2, shared=True, frames=images, before_pixels=12998,
        before_guard_bytes=256, notifications=0 if software else 2,
        unchanged_bytes=0 if software else 52248)
    if software:
        one('ROCK5_LOSS_SOFTWARE_SKIP_RESET', before)
        for marker in ('ROCK5_LOSS_ARMED', 'ROCK5_LOSS_NOTIFIED',
                'ROCK5_LOSS_FAULT', 'HAIKU_MESA_NATIVE_', 'ROCK5_MESA_NATIVE_LIFETIME'):
            assert marker not in before
        return result
    assert 'ROCK5_LOSS_SOFTWARE_SKIP_RESET' not in before
    raw_queue = int(one(r'ROCK5_LOSS_ARMED handle=(\d+) sequence=34 instruction=ff00000000000000', before))
    assert int(one(r'ROCK5_LOSS_RESET_READY handle=(\d+) state=0 error=0', before)) == raw_queue
    assert int(one(r'ROCK5_LOSS_RAW_CLOSED handle=(\d+)', before)) == raw_queue
    assert int(one(r'ROCK5_LOSS_FENCE result=(-?\d+)', before)) == IO_ERROR
    fault = one(r'ROCK5_LOSS_FAULT handle=(\d+) submitted=34 completed=33 failed=34 pending=1 reset=3 error=(-?\d+) pending_observed=([01])', before)
    assert tuple(map(int, fault[:2])) == (raw_queue, IO_ERROR)
    pending = re.findall(r'^ROCK5_LOSS_RESET_PENDING handle=(\d+) state=2 error=(-?\d+)\s*$', before, re.M)
    assert [tuple(map(int, p)) for p in pending] == ([(raw_queue, IO_ERROR)] if fault[2] == '1' else [])
    notifications = re.findall(r'^ROCK5_LOSS_NOTIFIED child=(\d+) first=8255 second=0000 read_error=0507 clear_error=0507 sync_error=0507 signaled=9119 unchanged=26124\s*$', before, re.M)
    assert notifications == ['0', '1']
    for left, right in [('ROCK5_LOSS_READY child=1', 'ROCK5_LOSS_ARMED '),
            ('ROCK5_LOSS_ARMED ', 'ROCK5_LOSS_FAULT '),
            ('ROCK5_LOSS_FAULT ', 'ROCK5_LOSS_RAW_CLOSED '),
            ('ROCK5_LOSS_RAW_CLOSED ', 'ROCK5_LOSS_NOTIFIED child=0'),
            ('ROCK5_LOSS_NOTIFIED child=0', 'ROCK5_LOSS_NOTIFIED child=1')]:
        assert before.index(left) < before.index(right)
    assert 'HAIKU_MESA_NATIVE_SUBMIT ' not in before[before.index('ROCK5_LOSS_ARMED '):]
    gpu = one(r'HAIKU_MESA_NATIVE_GPU handle=(\d+) id=a8670005 shader=50005 firmware=01050000 csf=040a0412 registers=96 reserved=4', before)
    bootstrap = int(gpu)
    retired = re.findall(r'^HAIKU_MESA_NATIVE_QUEUE handle=(\d+) query=0 state=3 error=(-?\d+) pending=0 submitted=(\d+) completed=(\d+) failed=0\s*$', before, re.M)
    assert len(retired) == before.count('HAIKU_MESA_NATIVE_QUEUE ') == 3
    queues = {}
    for handle, error, submitted, completed in retired:
        handle, error, submitted, completed = map(int, (handle, error, submitted, completed))
        assert error == IO_ERROR and completed == submitted
        assert 0 < handle < raw_queue and handle not in queues
        queues[handle] = submitted
    assert queues[bootstrap] == 0
    draw_queues = set(queues) - {bootstrap}
    assert len(draw_queues) == 2 and all(2 <= queues[h] <= 8 for h in draw_queues)
    submits = re.findall(r'^HAIKU_MESA_NATIVE_SUBMIT handle=(\d+) generation=(\d+) address=([0-9a-f]+) bytes=(\d+) waits=(\d+) signals=(\d+) sequence=(\d+)\s*$', before, re.M)
    assert len(submits) == before.count('HAIKU_MESA_NATIVE_SUBMIT ') == sum(queues.values())
    assert {int(row[0]) for row in submits} == draw_queues
    for handle in draw_queues:
        selected = [row for row in submits if int(row[0]) == handle]
        assert [int(row[6]) for row in selected] == list(range(1, queues[handle] + 1))
        assert any(int(row[3]) > 0 for row in selected)
        assert all(int(row[3]) <= 1048576 and int(row[4]) + int(row[5]) <= 64 for row in selected)
    reset_queries = re.findall(r'^HAIKU_MESA_NATIVE_RESET handle=(\d+) state=(\d+) error=(-?\d+)\s*$', before, re.M)
    assert {int(row[0]) for row in reset_queries} == draw_queues
    for handle in draw_queues:
        rows = [(int(s), int(e)) for h, s, e in reset_queries if int(h) == handle]
        assert rows.count((0, 0)) >= 1 and rows.count((3, IO_ERROR)) >= 2
        assert set(rows) == {(0, 0), (3, IO_ERROR)}
    snapshots = []
    for body in re.findall(r'^ROCK5_MESA_NATIVE_LIFETIME (.*)$', before, re.M):
        snapshots.append({k: int(v) for k, v in (item.split('=') for item in body.split())})
    assert [s.pop('cycle') for s in snapshots] == [99, 0]
    assert snapshots[0] == snapshots[1] == result['graphics']['allocation_baseline']
    result.update(raw_queue=raw_queue, bootstrap_queue=bootstrap, mesa_queues=queues,
        raw_submissions=34, raw_successful_completions=33, error_fences=1,
        allocation_baseline=snapshots[0], pending_query_observed=bool(pending))
    return result


def split_uart(uart):
    """Separate the earlier two-queue recovery from this four-queue recovery."""
    markers = list(re.finditer(r'^mali_csf: recovery queue handle=', uart, re.M))
    assert len(markers) == 6
    return uart[:markers[2].start()], uart[markers[2].start():]


def validate_uart(result, uart):
    """Validate the actual second-fault UART suffix without synthetic records."""
    assert not result['software']
    rows = re.findall(r'^mali_csf: recovery queue handle=(\d+) submitted=(\d+) '
        r'completed=(\d+) current=(\d+) pending=(\d+)\s*$', uart, re.M)
    queues = [tuple(map(int, row)) for row in rows]
    expected = [(int(handle), count, count, 0, 0)
        for handle, count in result['mesa_queues'].items()]
    expected.append((result['raw_queue'], 34, 33, 34, 1))
    assert len(queues) == 4 and sorted(queues) == sorted(expected)
    row = one(r'mali_csf: recovery result=(\d+) reset=(\d+)/(\d+) flags=(\S+) '
        r'irq=(\d+)/(\S+)/(\S+) quiescent=(\d+) restored=(\d+) recovered=(\d+) '
        r'job=(\S+) mmu=(\S+)', uart)
    code, reset, cleanup, flags, count, irq, raw, quiet, restored, recovered, job, mmu = [int(x, 0) for x in row]
    assert (code, reset, cleanup, flags, count, irq) == (0, 0, 0, 7, 1, 256)
    assert raw & 0x103 == 0x100
    assert (quiet, restored, recovered, job, mmu) == (1, 1, 1, 0, 3 << 16)
    row = one(r'mali_csf: recovery time start=(\d+) irq=(\d+) end=(\d+) cpu=(\d+) '
        r'power=(\d+)/(\d+)/(\S+)', uart)
    start, event, end, cpu, power, restore, power_flags = [int(x, 0) for x in row]
    assert 0 <= start <= event <= end and event-start <= 100000 and 0 <= cpu < 8
    assert (power, restore, power_flags) == (14, 0, 7)
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
    expected_runtime = (IO_ERROR, 10, 16, 17, 1215, 0, 0xff49, 0x120000000, 0)
    assert [tuple(int(x, 0) for x in row) for row in runtimes] == [
        expected_runtime, (0, 0, 0, 0, 255, 0, 0, 0, 0), (0, 0, 0, 0, 255, 0, 0, 0, 0)]
    return dict(status='pass', reset_verified=True, reset_irq_us=event-start,
        reset_elapsed_us=end-start, cpu=cpu, runtime_index=0,
        failed_runtime=list(expected_runtime), affected_queues=queues,
        final_idle=idle, address_spaces=spaces, fresh_runtimes_after_fault=2)
