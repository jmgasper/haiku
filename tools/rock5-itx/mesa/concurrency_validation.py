"""Validate actual simultaneous-process draw intervals, full pixels and cleanup."""
import hashlib
import re

WIDTH, HEIGHT = 97, 67

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
    assert len(final) >= 2 and set(submitted) <= seen
    return dict(properties=props[0], submitted=sum(submitted.values()), command_streams=commands,
        final_queues=final, final_completion_records=sum(x['completed'] for x in final),
        rendered_before_exit=True)


def validate(text, output=None, software=False):
    del output  # The raw transcript retains every actual pixel and guard byte.
    rounds = 8 if software else 64
    mode = '--software' if software else '--native'
    prefix = 'ROCK5_CONCURRENT_'
    assert re.findall(r'^ROCK5_CONCURRENT_PARENT_READY (.*)$', text, re.M) == [
        f'version=1 mode={mode} rounds={rounds} required_overlap={0 if software else 32}']
    for forbidden in ('ROCK5_PIPELINE_FAILURE', 'ROCK5_PIPELINE_SHADER_FAILURE',
            'ROCK5_PIPELINE_LINK_FAILURE', prefix+'LINK_FAILURE', prefix+'ABORT_LOG',
            'PANIC:', 'Kernel Debugging Land', 'DEBUGGER:', 'mutex->owner'):
        assert forbidden not in text
    assert not re.search(r'Haiku CSF .* failed', text)
    started = re.findall(r'^ROCK5_CONCURRENT_STARTED child=(\d+) pid=(\d+)\s*$', text, re.M)
    assert [int(x[0]) for x in started] == [0, 1, 2]
    assert all(int(x[1]) > 0 for x in started) and started[0][1] != started[1][1]
    assert re.findall(r'^ROCK5_CONCURRENT_EXIT (.*)$', text, re.M) == [
        f'child={child} killed=0 signal=0 exit=0' for child in (1, 0, 2)]
    assert text.count(prefix+'SURVIVOR_PASS child=0 same_context=1') == 1
    logs = re.findall(r'^ROCK5_CONCURRENT_LOG_BEGIN child=(\d+)\n(.*?)'
        r'\nROCK5_CONCURRENT_LOG_END child=(\d+) bytes=(\d+)\s*$', text, re.M | re.S)
    assert len(logs) == text.count(prefix+'LOG_BEGIN ') == text.count(prefix+'LOG_END ') == 3
    frames, workers, lookup = [], [], {}
    for (index, body, end_index, size), child in zip(logs, (1, 0, 2)):
        assert int(index) == child == int(end_index) and len(body.encode()) == int(size)
        context = re.findall(r'^ROCK5_CONCURRENT_GL child=(\d+) renderer=(.*?) version=(.*?)\s*$', body, re.M)
        assert len(context) == 1 and int(context[0][0]) == child
        assert context[0][1] == ('softpipe' if software else 'Mali-G610 (Panfrost)')
        assert re.fullmatch(r'OpenGL ES 3\.\d+ Mesa 25\.3\.6', context[0][2])
        assert re.findall(r'^ROCK5_CONCURRENT_VERTICES (.*)$', body, re.M) == [
            f'child={child} vertices=98304 bytes=786432 explicit_buffer=1']
        count = rounds+1 if child == 0 else rounds if child == 1 else 1
        assert re.findall(r'^ROCK5_CONCURRENT_WORKER_PASS (.*)$', body, re.M) == [
            f'child={child} frames={count} destroyed=1']
        rows = re.findall(r'^ROCK5_CONCURRENT_FRAME (.*)$', body, re.M)
        assert len(rows) == count and body.count(prefix+'FRAME ') == count
        previous_end = 0
        for frame, row in enumerate(rows):
            fields = re.fullmatch(r'child=(\d+) frame=(\d+) quads=(\d+) start_at=(\d+) '
                r'start=(\d+) end=(\d+) wait=(911a|911c) mismatches=0 guard_errors=0 '
                r'guard_before=([0-9a-f]{128}) guard_after=([0-9a-f]{128}) '
                r'rgba_runs=([0-9a-f:,]+)', row)
            assert fields, (child, frame, row[:180])
            who, number, quads, start_at, begin, end, wait, before, after, runs = fields.groups()
            who, number, quads, start_at, begin, end = map(int, (who, number, quads, start_at, begin, end))
            assert (who, number, quads) == (child, frame, (8192,16384)[(child+frame)%2])
            assert 0 < start_at <= begin < end and begin >= previous_end
            previous_end = end
            canary = bytes([0x40+(child*53+frame)%191])*64
            assert bytes.fromhex(before) == bytes.fromhex(after) == canary
            raw = bytearray()
            for run in runs.split(','):
                match = re.fullmatch(r'([1-9][0-9]*):([0-9a-f]{8})', run)
                assert match
                n, pixel = int(match[1]), bytes.fromhex(match[2])
                assert len(raw)//4 + n <= WIDTH*HEIGHT
                raw.extend(pixel*n)
            colour = (1,2,4,6)[(child+frame)%4]
            expected = bytes([255 if colour & (1 << c) else 0 for c in range(3)]+[255])
            assert len(raw) == WIDTH*HEIGHT*4 and raw == expected*(WIDTH*HEIGHT)
            result = dict(child=child,frame=frame,quads=quads,start_at=start_at,start=begin,end=end,
                render_us=end-begin,pixels=WIDTH*HEIGHT,guard_bytes=128,
                rgba_sha256=hashlib.sha256(raw).hexdigest())
            frames.append(result); lookup[child,frame] = result
        if not software:
            workers.append(dict(child=child,**queues(body,child)))
    assert len(frames) == 2*rounds+2
    # Ready records are independent pipe messages captured by the parent.
    ready = [tuple(map(int,row)) for row in re.findall(
        r'^ROCK5_CONCURRENT_READY child=(\d+) frame=(\d+) start=(\d+) end=(\d+)\s*$',text,re.M)]
    wanted = [(0,4294967295,0,0),(1,4294967295,0,0)]
    for frame in range(rounds):
        for child in (0,1):
            value=lookup[child,frame]
            wanted.append((child,frame,value['start'],value['end']))
    value=lookup[0,rounds]; wanted.append((0,rounds,value['start'],value['end']))
    wanted.append((2,4294967295,0,0))
    value=lookup[2,0]; wanted.append((2,0,value['start'],value['end']))
    assert ready == wanted
    rows = re.findall(r'^ROCK5_CONCURRENT_ROUND frame=(\d+) start_at=(\d+) overlap_us=(\d+)\s*$', text,re.M)
    assert len(rows) == rounds
    overlaps=[]
    for frame,(number,start_at,overlap) in enumerate(rows):
        number,start_at,overlap=map(int,(number,start_at,overlap))
        a,b=lookup[0,frame],lookup[1,frame]
        assert number == frame and start_at == a['start_at'] == b['start_at']
        actual=max(0,min(a['end'],b['end'])-max(a['start'],b['start']))
        assert overlap == actual
        overlaps.append(actual)
    overlap_count=sum(x>0 for x in overlaps); overlap_total=sum(overlaps)
    assert software or overlap_count >= 32
    total=2*rounds+2
    assert re.findall(r'^ROCK5_CONCURRENT_PASS (.*)$',text,re.M) == [
        f'processes=3 normal=3 frames={total} pixels={total*WIDTH*HEIGHT} '
        f'guard_bytes={total*128} overlapping_rounds={overlap_count} '
        f'overlap_us={overlap_total} software={int(software)}']
    result=dict(status='pass',software=software,processes=3,normal_exits=3,frames=frames,
        pixels=total*WIDTH*HEIGHT,guard_bytes=total*128,paired_rounds=rounds,
        overlapping_rounds=overlap_count,overlap_us=overlap_total,
        interval_scope='Application draw, fence wait and readback; excludes barrier and logging',
        survivor_rendered=True,fresh_context=True)
    if software:
        assert 'HAIKU_MESA_NATIVE_' not in text and 'ROCK5_MESA_NATIVE_LIFETIME' not in text
    else:
        snapshots=[]
        for row in re.findall(r'^ROCK5_MESA_NATIVE_LIFETIME (.*)$',text,re.M):
            snapshots.append({k:int(v) for k,v in (field.split('=') for field in row.split())})
        assert [x['cycle'] for x in snapshots] == [99,10,11,12,13,0,14,1]
        base={k:v for k,v in snapshots[0].items() if k!='cycle'}
        assert set(base) == set('clients buffers bytes vms generations vm_pages heaps chunks heap_bytes '
            'heap_pages heap_generations sync_clients sync_objects sync_points sync_events sync_exports '
            'sync_waits kernel_areas user_maps'.split())
        assert base['clients'] == base['sync_clients'] == 1
        assert all(v == 0 for k,v in base.items() if k not in ('clients','sync_clients'))
        for i in (5,7):
            assert {k:v for k,v in snapshots[i].items() if k!='cycle'} == base
        for i,live in ((1,1),(2,2),(3,2),(4,1),(6,1)):
            row=snapshots[i]
            assert row['clients'] == row['sync_clients'] == live+1 and row['user_maps'] == 0
            assert row['vms'] >= live and row['heaps'] >= live
            assert all(row[k]>0 for k in ('buffers','bytes','sync_objects','kernel_areas'))
        result.update(workers=workers,allocation_snapshots=snapshots,
            native_submissions=sum(x['submitted'] for x in workers),
            native_completions=sum(x['final_completion_records'] for x in workers),
            expected_runtime_joins=2)
    return result
