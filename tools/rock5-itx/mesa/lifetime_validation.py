"""Check independent graphics-process lifetime, full pixels and native cleanup."""
import hashlib
from pathlib import Path
import re

from pipeline_validation import WIDTH, HEIGHT, expected
from window_validation import png


def queues(text, child):
    props = re.findall(r'^HAIKU_MESA_NATIVE_GPU handle=(\d+) id=([0-9a-f]+) '
        r'shader=([0-9a-f]+) firmware=([0-9a-f]+) csf=([0-9a-f]+) registers=(\d+) reserved=(\d+)\s*$', text, re.M)
    assert len(props) == 1
    handle, gpu, shader, firmware, csf, registers, reserved = props[0]
    assert int(handle) > 0 and [int(x, 16) for x in (gpu, shader, firmware, csf)] == [
        0xa8670005, 0x50005, 0x01050000, 0x040a0412]
    assert (int(registers), int(reserved)) == (96, 4)
    errors = re.findall(r'^HAIKU_MESA_NATIVE_ERROR op=([0-9a-f]+) bytes=(\d+) '
        r'result=(-?\d+) errno=(-?\d+)\s*$', text, re.M)
    assert len(errors) == (0 if child == 1 else 1)
    for op, size, result, error in errors:
        assert (int(op, 16), int(size), int(result), int(error)) == (
            0x4d435340, 48, -1, -2147483648 + 0x6000 + 3)
    submitted = {}
    commands = 0
    for handle, generation, address, size, waits, signals, sequence in re.findall(
            r'^HAIKU_MESA_NATIVE_SUBMIT handle=(\d+) generation=(\d+) address=([0-9a-f]+) '
            r'bytes=(\d+) waits=(\d+) signals=(\d+) sequence=(\d+)\s*$', text, re.M):
        handle, size, sequence, address = int(handle), int(size), int(sequence), int(address, 16)
        assert sequence == submitted.get(handle, 0) + 1 and int(generation) > 0
        assert (size == address == 0) or (size > 0 and size % 8 == 0 and 65536 <= address < address+size < 1 << 47)
        assert 0 < int(signals) <= 64 and int(signals) + int(waits) <= 64
        submitted[handle] = sequence
        commands += size > 0
    assert commands >= (2 if child == 0 else 1)
    final = []
    seen = set()
    for row in re.findall(r'^HAIKU_MESA_NATIVE_QUEUE handle=(\d+) query=(-?\d+) state=(\d+) '
            r'error=(-?\d+) pending=(\d+) submitted=(\d+) completed=(\d+) failed=(\d+)\s*$', text, re.M):
        h, q, s, e, p, a, c, f = map(int, row)
        assert h not in seen and q == e == p == f == 0 and s in (1, 2)
        assert a == c == submitted.get(h, 0)
        seen.add(h)
        final.append(dict(handle=h, submitted=a, completed=c))
    if child == 1:
        assert not final  # SIGKILL bypasses userspace queue-destruction logging.
    else:
        assert len(final) >= 2 and set(submitted) <= seen
    return dict(properties=props[0], submitted=sum(submitted.values()), command_streams=commands,
        final_queues=final, final_completion_records=sum(x['completed'] for x in final),
        rendered_before_exit=True)


def validate(text, output=None, software=False):
    mode = '--software' if software else '--native'
    assert text.count('ROCK5_LIFETIME_PARENT_READY version=1 mode='+mode) == 1
    assert text.count('ROCK5_LIFETIME_PASS processes=3 normal=2 killed=1 frames=4 '
        'pixels=25996 guard_bytes=512 software='+str(int(software))) == 1
    for forbidden in ('ROCK5_PIPELINE_FAILURE', 'ROCK5_PIPELINE_SHADER_FAILURE',
            'ROCK5_PIPELINE_LINK_FAILURE', 'ROCK5_LIFETIME_ABORT_LOG', 'PANIC:',
            'Kernel Debugging Land', 'DEBUGGER:', 'mutex->owner', 'Haiku CSF failed'):
        assert forbidden not in text
    assert not re.search(r'Haiku CSF .* failed', text)
    started = re.findall(r'^ROCK5_LIFETIME_STARTED child=(\d+) pid=(\d+)\s*$', text, re.M)
    assert [x[0] for x in started] == ['0', '1', '2'] and all(int(x[1]) > 0 for x in started)
    assert started[0][1] != started[1][1]
    assert re.findall(r'^ROCK5_LIFETIME_READY child=(\d+) phase=(\d+) pattern=(\d+)\s*$', text, re.M) == [
        ('0', '0', '0'), ('1', '0', '1'), ('0', '1', '1'), ('2', '0', '0')]
    assert re.findall(r'^ROCK5_LIFETIME_EXIT child=(\d+) killed=(\d+) signal=(\d+) exit=(-?\d+)\s*$', text, re.M) == [
        ('1', '1', '9', '-1'), ('0', '0', '0', '0'), ('2', '0', '0', '0')]
    assert text.count('ROCK5_LIFETIME_SURVIVOR_PASS child=0 new_pattern=1 same_context=1') == 1
    logs = re.findall(r'^ROCK5_LIFETIME_LOG_BEGIN child=(\d+)\n(.*?)'
        r'\nROCK5_LIFETIME_LOG_END child=(\d+) bytes=(\d+)\s*$', text, re.M | re.S)
    assert len(logs) == 3 and text.count('ROCK5_LIFETIME_LOG_BEGIN ') == 3
    frames = []
    workers = []
    for (index, body, end_index, size), child in zip(logs, (1, 0, 2)):
        assert int(index) == child == int(end_index) and len(body.encode()) == int(size)
        context = re.findall(r'^ROCK5_LIFETIME_CONTEXT child=(\d+) renderer=(.*?) version=(.*?)\s*$', body, re.M)
        assert len(context) == 1 and int(context[0][0]) == child
        assert context[0][1] == ('softpipe' if software else 'Mali-G610 (Panfrost)')
        assert re.fullmatch(r'OpenGL ES 3\.\d+ Mesa 25\.3\.6', context[0][2])
        patterns = (0, 1) if child == 0 else (child % 2,)
        assert re.findall(r'^ROCK5_LIFETIME_DRAW child=(\d+) phase=(\d+) pattern=(\d+)\s*$', body, re.M) == [
            (str(child), str(phase), str(pattern)) for phase, pattern in enumerate(patterns)]
        assert re.findall(r'^ROCK5_LIFETIME_LIVE child=(\d+) phase=(\d+) gpu_finished=1 resources_live=1\s*$', body, re.M) == [
            (str(child), str(phase)) for phase in range(len(patterns))]
        retired = re.findall(r'^ROCK5_LIFETIME_WORKER_PASS child=(\d+) draws=(\d+) destroyed=1\s*$', body, re.M)
        assert retired == ([] if child == 1 else [(str(child), str(len(patterns)))])
        records = re.findall(r'^ROCK5_PIPELINE_PIXELS_BEGIN cycle=(\d+) case=texture width=97 height=67 '
            r'format=RGBA8 origin=lower-left\n(.*?)'
            r'^ROCK5_PIPELINE_PIXELS_END cycle=(\d+) case=texture mismatches=0 guard_errors=0 '
            r'wait=([0-9a-f]+) wait_ns=(\d+)\s*$', body, re.M | re.S)
        assert len(records) == len(patterns) == body.count('ROCK5_PIPELINE_PIXELS_BEGIN ')
        for phase, ((cycle, data, end_cycle, wait, wait_ns), pattern) in enumerate(zip(records, patterns)):
            assert int(cycle) == pattern == int(end_cycle)
            assert int(wait, 16) in (0x911a, 0x911c) and 0 <= int(wait_ns) < 6_000_000_000
            lines = data.splitlines()
            guard = bytes([0xa0 + pattern * 6]) * 64
            assert len(lines) == HEIGHT + 2
            assert lines[0] == 'guard_before='+guard.hex() and lines[-1] == 'guard_after='+guard.hex()
            raw = bytearray()
            for y, line in enumerate(lines[1:-1]):
                assert re.fullmatch(r'row=\d{3} [0-9a-f]{776}', line) and line.startswith(f'row={y:03d} ')
                raw.extend(bytes.fromhex(line[8:]))
            assert raw == expected(pattern, 'texture')
            frame = dict(child=child, phase=phase, pattern=pattern, pixels=WIDTH*HEIGHT,
                rgba_sha256=hashlib.sha256(raw).hexdigest(), guard_bytes=128)
            if output:
                root = Path(output)
                root.mkdir(parents=True, exist_ok=True)
                stem = root/f'child-{child}-phase-{phase}'
                stem.with_suffix('.rgba').write_bytes(raw)
                rgb = bytearray()
                for y in reversed(range(HEIGHT)):
                    for x in range(WIDTH):
                        offset = (y*WIDTH+x)*4
                        rgb.extend(raw[offset:offset+3])
                png(stem.with_suffix('.png'), bytes(rgb), WIDTH, HEIGHT)
                frame.update(rgba=str(stem.with_suffix('.rgba')), png=str(stem.with_suffix('.png')))
            frames.append(frame)
        if not software:
            workers.append(dict(child=child, **queues(body, child)))
    result = dict(status='pass', software=software, processes=3, normal_exits=2, killed_exits=1,
        frames=frames, pixels=25996, guard_bytes=512, survivor_rendered=True, fresh_context=True)
    if software:
        assert 'HAIKU_MESA_NATIVE_' not in text and 'ROCK5_MESA_NATIVE_LIFETIME' not in text
    else:
        snapshots = []
        for row in re.findall(r'^ROCK5_MESA_NATIVE_LIFETIME (.*)$', text, re.M):
            snapshots.append({k:int(v) for k,v in (field.split('=') for field in row.split())})
        assert [x['cycle'] for x in snapshots] == [99, 10, 11, 12, 0, 13, 1]
        base = {k:v for k,v in snapshots[0].items() if k != 'cycle'}
        assert set(base) == set('clients buffers bytes vms generations vm_pages heaps chunks heap_bytes '
            'heap_pages heap_generations sync_clients sync_objects sync_points sync_events sync_exports '
            'sync_waits kernel_areas user_maps'.split())
        assert base['clients'] == base['sync_clients'] == 1
        assert all(v == 0 for k,v in base.items() if k not in ('clients', 'sync_clients'))
        for i in (4, 6):
            assert {k:v for k,v in snapshots[i].items() if k != 'cycle'} == base
        for i, live in ((1,1), (2,2), (3,1), (5,1)):
            row = snapshots[i]
            assert row['clients'] == row['sync_clients'] == live + 1 and row['user_maps'] == 0
            assert row['vms'] >= live and row['heaps'] >= live
            assert all(row[k] > 0 for k in ('buffers','bytes','sync_objects','kernel_areas'))
        result.update(workers=workers, allocation_snapshots=snapshots,
            native_submissions=sum(x['submitted'] for x in workers),
            final_completion_records=sum(x['final_completion_records'] for x in workers),
            expected_runtime_joins=2, killed_after_gpu_completion=True)
    return result
