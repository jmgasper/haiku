"""Require native kernel provenance as well as every returned pixel."""
import re
import render_validation

def validate(text, output=None):
    result=render_validation.validate(text,output,software=False)
    result.update(native_evidence(text))
    return result


def native_evidence(text):
    result={}
    properties=re.findall(r'^HAIKU_MESA_NATIVE_GPU handle=(\d+) id=([0-9a-f]+) shader=([0-9a-f]+) firmware=([0-9a-f]+) csf=([0-9a-f]+) registers=(\d+) reserved=(\d+)\s*$',text,re.M)
    assert len(properties)==2
    for handle,gpu,shader,firmware,csf,registers,reserved in properties:
        assert int(handle)>0 and [int(x,16) for x in (gpu,shader,firmware,csf)]==[0xa8670005,0x50005,0x01050000,0x040a0412]
        assert (int(registers),int(reserved))==(96,4)
    # INT_MIN + B_STORAGE_ERROR_BASE offset 0x6000 + 3, pinned Haiku Errors.h.
    errors=re.findall(r'^HAIKU_MESA_NATIVE_ERROR op=([0-9a-f]+) bytes=(\d+) result=(-?\d+) errno=(-?\d+)\s*$',text,re.M)
    assert len(errors)==2
    assert all((int(op,16),int(size),int(ret),int(err))==(0x4d435340,48,-1,-2147483648+0x6000+3) for op,size,ret,err in errors)
    assert not re.search(r'Haiku CSF .* failed', text)
    cycles=[]
    for part in text.split('HAIKU_MESA_NATIVE_GPU ')[1:]:
        submitted={}; command_streams=0
        for handle,generation,address,size,waits,signals,sequence in re.findall(r'^HAIKU_MESA_NATIVE_SUBMIT handle=(\d+) generation=(\d+) address=([0-9a-f]+) bytes=(\d+) waits=(\d+) signals=(\d+) sequence=(\d+)\s*$',part,re.M):
            handle=int(handle);sequence=int(sequence);size=int(size);address=int(address,16)
            assert sequence==submitted.get(handle,0)+1
            assert int(generation)>0
            assert (size==0 and address==0) or (size>0 and size%8==0 and address>=65536 and address+size<(1<<47))
            command_streams += size>0
            assert int(waits)+int(signals)<=64 and int(signals)>0
            submitted[handle]=sequence
        queues=[];seen=set()
        for handle,query,state,error,pending,accepted,completed,failed in re.findall(r'^HAIKU_MESA_NATIVE_QUEUE handle=(\d+) query=(-?\d+) state=(\d+) error=(-?\d+) pending=(\d+) submitted=(\d+) completed=(\d+) failed=(\d+)\s*$',part,re.M):
            h,q,s,e,p,a,c,f=map(int,(handle,query,state,error,pending,accepted,completed,failed))
            assert h not in seen and q==e==p==f==0 and s in (1,2) and a==c==submitted.get(h,0)
            seen.add(h);queues.append(dict(handle=h,submitted=a,completed=c))
        assert set(submitted)<=seen and len(queues)>=2 and command_streams>=2
        cycles.append(dict(queues=queues,submitted=sum(submitted.values()),completed=sum(x['completed'] for x in queues),command_streams=command_streams))
    rows=[]
    for body in re.findall(r'^ROCK5_MESA_NATIVE_LIFETIME (.*)$',text,re.M):
        fields={key:int(value) for key,value in (item.split('=') for item in body.split())}
        rows.append(fields)
    assert [x.pop('cycle') for x in rows]==[99,0,1]
    assert rows[0]==rows[1]==rows[2]
    assert set(rows[0])==set('clients buffers bytes vms generations vm_pages heaps chunks heap_bytes heap_pages heap_generations sync_clients sync_objects sync_points sync_events sync_exports sync_waits kernel_areas user_maps'.split())
    assert rows[0]['clients']==rows[0]['sync_clients']==1
    assert all(value==0 for key,value in rows[0].items() if key not in ('clients','sync_clients'))
    result.update(native_gpu=True,gpu_properties=properties,queue_cycles=cycles,
        native_submissions=sum(x['submitted'] for x in cycles),native_completions=sum(x['completed'] for x in cycles),
        allocation_baseline=rows[0],expected_runtime_joins=2)
    return result
