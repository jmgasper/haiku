"""Check the original bytes of completed, separately captured process streams."""
import hashlib
import re


def validate(text, software=False, mode=None):
    raw=text.encode()
    runs=re.findall(rb'^ROCK5_APPLICATION_RUN_BEGIN cycle=(\d+) mode=(\S+)\n(.*?)'
        rb'^ROCK5_APPLICATION_RUN_EXIT cycle=(\d+) application=0 frames=0\s*$',raw,re.M|re.S)
    mode=(b'--software' if software else b'--native') if mode is None else mode.encode()
    assert [(int(c),m,int(end)) for c,m,body,end in runs]==[(0,mode,0),(1,mode,1)]
    assert raw.count(b'ROCK5_APPLICATION_LOG_BEGIN ')==raw.count(b'ROCK5_APPLICATION_LOG_END ')==4
    results=[]
    for cycle,_,body,_ in runs:
        headers=list(re.finditer(rb'^ROCK5_APPLICATION_LOG_BEGIN stream=(controller|renderer) bytes=(\d+) sha256=([0-9a-f]{64})\n',body,re.M))
        assert [m[1] for m in headers]==[b'controller',b'renderer']
        streams={};previous_end=0
        for header in headers:
            assert header.start()>=previous_end
            stream,size,sha=header.group(1,2,3);size=int(size)
            assert size<=64*1024*1024
            start=header.end();end=start+size;data=body[start:end]
            assert len(data)==size and hashlib.sha256(data).hexdigest().encode()==sha
            assert not data or data.endswith(b'\n')
            footer=b'ROCK5_APPLICATION_LOG_END stream='+stream+b'\n'
            assert body[end:end+len(footer)]==footer
            previous_end=end+len(footer)
            streams[stream]=data
            results.append(dict(cycle=int(cycle),stream=stream.decode(),bytes=size,sha256=sha.decode()))
        for marker in (b'HAIKU_MESA_NATIVE_',b'HAIKU_PAN_SW_POLYGON '):
            assert body.count(marker)==streams[b'renderer'].count(marker)
            assert marker not in streams[b'controller']
            if software:assert marker not in body
        for marker in (b'ROCK5_MESA_NATIVE_LIFETIME ',b'ROCK5_APPLICATION_STARTED ',
                b'ROCK5_APPLICATION_MENU ',b'ROCK5_APPLICATION_CAPTURE ',
                b'ROCK5_APPLICATION_ANIMATION ',b'ROCK5_APPLICATION_CONTROLLER_PASS '):
            assert body.count(marker)==streams[b'controller'].count(marker)
            assert marker not in streams[b'renderer']
    return dict(status='pass',software=software,cycles=2,streams=results,
        bytes=sum(r['bytes'] for r in results),source_sha256=hashlib.sha256(raw).hexdigest())
