"""Check finite shader work, termination, complete pixels and close-time evidence."""
from functools import lru_cache
import hashlib
import re
from lifetime_validation import queues

WIDTH, HEIGHT = 97, 67

@lru_cache(maxsize=8)
def coefficients(iterations):
    # Linear recurrence, independently implemented from the guest's exponentiation.
    a, b = 1, 0
    for _ in range(iterations):
        a = a * 1664525 & 0xffffffff
        b = (b * 1664525 + 1013904223) & 0xffffffff
    return a, b

def expected(iterations, seed):
    a, b = coefficients(iterations)
    return b''.join(((a*(p+seed)+b) & 0xffffff).to_bytes(3,'little') + b'\xff'
        for p in range(WIDTH*HEIGHT))

def validate(text, output=None, software=False):
    del output
    mode = '--software' if software else '--native'
    prefix = 'ROCK5_PENDING_'
    assert re.findall(r'^ROCK5_PENDING_PARENT_READY (.*)$',text,re.M) == [
        f'version=1 mode={mode} target_us=100000 maximum_us=2000000 maximum_iterations=1048576']
    for forbidden in ('ROCK5_PIPELINE_FAILURE','ROCK5_PIPELINE_SHADER_FAILURE',
            'ROCK5_PIPELINE_LINK_FAILURE',prefix+'ABORT_LOG','PANIC:',
            'Kernel Debugging Land','DEBUGGER:','mutex->owner'):
        assert forbidden not in text
    assert not re.search(r'Haiku CSF .* failed',text)
    started = re.findall(r'^ROCK5_PENDING_STARTED child=(\d+) pid=(\d+)\s*$',text,re.M)
    assert [int(x[0]) for x in started] == [0,1,2]
    assert all(int(x[1]) > 0 for x in started) and started[0][1] != started[1][1]
    assert re.findall(r'^ROCK5_PENDING_EXIT (.*)$',text,re.M) == [
        'child=1 killed=1 signal=9 exit=-1','child=0 killed=0 signal=0 exit=0',
        'child=2 killed=0 signal=0 exit=0']
    assert text.count(prefix+'SURVIVOR_PASS child=0 same_context=1') == 1
    logs = re.findall(r'^ROCK5_PENDING_LOG_BEGIN child=(\d+)\n(.*?)'
        r'\nROCK5_PENDING_LOG_END child=(\d+) bytes=(\d+)\s*$',text,re.M|re.S)
    assert len(logs) == text.count(prefix+'LOG_BEGIN ') == text.count(prefix+'LOG_END ') == 3
    frames, workers, lookup = [], [], {}
    for (index,body,end_index,size), child in zip(logs,(1,0,2)):
        assert int(index) == child == int(end_index) and len(body.encode()) == int(size)
        context = re.findall(r'^ROCK5_PENDING_GL child=(\d+) renderer=(.*?) version=(.*?)\s*$',body,re.M)
        assert len(context) == 1 and int(context[0][0]) == child
        assert context[0][1] == ('softpipe' if software else 'Mali-G610 (Panfrost)')
        assert re.fullmatch(r'OpenGL ES 3\.\d+ Mesa 25\.3\.6',context[0][2])
        rows = re.findall(r'^ROCK5_PENDING_FRAME (.*)$',body,re.M)
        count = len(rows)
        assert count == body.count(prefix+'FRAME ')
        assert (1 <= count <= 6 if child == 1 else count == (2 if child == 0 else 1))
        assert not (software and child == 1) or count == 1
        assert re.findall(r'^ROCK5_PENDING_WORKER_PASS (.*)$',body,re.M) == (
            [] if child == 1 else [f'child={child} frames={count} destroyed=1'])
        previous_end = 0
        for frame,row in enumerate(rows):
            fields = re.fullmatch(r'child=(\d+) frame=(\d+) iterations=(\d+) seed=(\d+) '
                r'start=(\d+) end=(\d+) wait=(911a|911c) mismatches=0 guard_errors=0 '
                r'guard_before=([0-9a-f]{128}) guard_after=([0-9a-f]{128}) '
                r'rgba=([0-9a-f]{51992})',row)
            assert fields, (child,frame,row[:160])
            who,number,iterations,seed,begin,end,wait,before,after,rgba = fields.groups()
            who,number,iterations,seed,begin,end = map(int,(who,number,iterations,seed,begin,end))
            assert (who,number,seed) == (child,frame,child*101+frame)
            assert iterations == (1024*4**frame if child == 1 and not software else 32)
            assert 0 < begin < end and begin >= previous_end
            previous_end = end
            canary = bytes([0x40+child*16+frame])*64
            assert bytes.fromhex(before) == bytes.fromhex(after) == canary
            raw = bytes.fromhex(rgba)
            assert raw == expected(iterations,seed)
            value = dict(child=child,frame=frame,iterations=iterations,seed=seed,start=begin,
                end=end,render_us=end-begin,pixels=WIDTH*HEIGHT,guard_bytes=128,
                rgba_sha256=hashlib.sha256(raw).hexdigest())
            frames.append(value);lookup[child,frame]=value
            if child == 1 and not software:
                assert end-begin < 2000000
                assert end-begin < 100000 if frame+1<count else end-begin >= 100000
        if child == 1:
            calibration = lookup[child,count-1]
            assert re.findall(r'^ROCK5_PENDING_CALIBRATED (.*)$',body,re.M) == [
                f'child=1 frames={count} iterations={calibration["iterations"]} '
                f'render_us={calibration["render_us"]} software={int(software)}']
            fences = re.findall(r'^ROCK5_PENDING_FENCE child=1 frame=(\d+) iterations=(\d+) '
                r'seed=(\d+) start=(\d+) observed=(\d+) wait=(911a|911b|911c) software=(0|1)\s*$',body,re.M)
            assert len(fences) == body.count(prefix+'FENCE ') == 1
            number,iterations,seed,begin,observed,wait,sw = fences[0]
            number,iterations,seed,begin,observed,sw = map(int,(number,iterations,seed,begin,observed,sw))
            assert (number,iterations,seed,sw) == (count,calibration['iterations'],calibration['seed'],int(software))
            assert observed >= begin > calibration['end']
            assert software or wait == '911b'
            fence = dict(frame=number,iterations=iterations,seed=seed,start=begin,observed=observed,wait=int(wait,16))
            killed_body = body
        else:
            assert prefix+'CALIBRATED' not in body and prefix+'FENCE' not in body
        if not software:
            workers.append(dict(child=child,**queues(body,child)))
    count = len(frames)
    assert re.findall(r'^ROCK5_PENDING_PASS (.*)$',text,re.M) == [
        f'processes=3 normal=2 killed=1 frames={count} pixels={count*WIDTH*HEIGHT} '
        f'guard_bytes={count*128} software={int(software)}']
    ready = [tuple(map(int,row)) for row in re.findall(r'^ROCK5_PENDING_READY child=(\d+) '
        r'frame=(\d+) iterations=(\d+) start=(\d+) end=(\d+)\s*$',text,re.M)]
    def record(child,frame):
        v=lookup[child,frame]
        return (child,frame,v['iterations'],v['start'],v['end'])
    assert ready == [(0,4294967295,0,0,0),record(0,0),(1,4294967295,0,0,0),
        record(1,calibration['frame']), (1,fence['frame'],fence['iterations'],fence['start'],fence['observed']),
        record(0,1),(2,4294967295,0,0,0),record(2,0)]
    kills = re.findall(r'^ROCK5_PENDING_KILL child=1 requested=(\d+) fence_observed=(\d+)\s*$',text,re.M)
    assert len(kills) == 1
    requested,observed=map(int,kills[0])
    assert observed == fence['observed'] <= requested < lookup[0,1]['start']
    result=dict(status='pass',software=software,processes=3,normal_exits=2,killed_exits=1,
        frames=frames,pixels=count*WIDTH*HEIGHT,guard_bytes=count*128,calibration=calibration,
        fence=fence,kill_requested=requested,survivor_rendered=True,fresh_context=True,
        pending_at_close_verified=False)
    if software:
        assert 'HAIKU_MESA_NATIVE_' not in text and 'ROCK5_MESA_NATIVE_LIFETIME' not in text
    else:
        snapshots=[{k:int(v) for k,v in (f.split('=') for f in row.split())}
            for row in re.findall(r'^ROCK5_MESA_NATIVE_LIFETIME (.*)$',text,re.M)]
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
            assert row['clients'] == row['sync_clients'] == live+1 and row['user_maps']==0
            assert row['vms'] >= live and row['heaps'] >= live
            assert all(row[k]>0 for k in ('buffers','bytes','sync_objects','kernel_areas'))
        submissions = [(int(h),int(seq),int(size)) for h,size,seq in re.findall(
            r'^HAIKU_MESA_NATIVE_SUBMIT handle=(\d+) generation=\d+ address=[0-9a-f]+ '
            r'bytes=(\d+) waits=\d+ signals=\d+ sequence=(\d+)\s*$',killed_body,re.M)]
        # Each submission was logged by the killed worker, before its fence report.
        assert submissions
        result.update(workers=workers,allocation_snapshots=snapshots,killed_submissions=submissions,
            native_submissions=sum(x['submitted'] for x in workers),
            final_completion_records=sum(x['final_completion_records'] for x in workers),
            expected_runtime_joins=2)
    return result


def validate_close(result, serial):
    """Require the real driver's pending state from the same native boot's UART."""
    assert not result['software'] and result['fence']['wait'] == 0x911b
    submissions = result['killed_submissions']
    final = {h:seq for h,seq,size in submissions}
    command_sequences = {(h,seq) for h,seq,size in submissions if size > 0}
    rows = [tuple(map(int,row)) for row in re.findall(r'^mali_csf: close pending handle=(\d+) '
        r'pending=(\d+) submitted=(\d+) completed=(\d+) current=(\d+) failed=(\d+) error=(-?\d+)\s*$',serial,re.M)]
    found = []
    for h,pending,submitted,completed,current,failed,error in rows:
        if h not in final: continue
        assert pending > 0 and submitted == final[h] and 0 <= completed < submitted
        assert pending == submitted-completed and failed == error == 0
        assert current == 0 or completed < current <= submitted
        assert any(handle==h and completed<seq<=submitted for handle,seq in command_sequences)
        found.append(dict(handle=h,pending=pending,submitted=submitted,completed=completed,current=current))
    assert found and len({r['handle'] for r in found}) == len(found)
    return dict(status='pass',pending_at_close_verified=True,queues=found,
        scope='Work pending at client close; active work may complete before queue retirement')
