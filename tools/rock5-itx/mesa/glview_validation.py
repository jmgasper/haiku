"""Independent full-pixel checks for the normal Haiku OpenGL Kit fixture."""
import hashlib
import re
from pathlib import Path

import native_render_validation
from window_validation import png

SIZES = ((64, 64), (79, 47), (128, 72), (65, 63))
PALETTES = (
    (((0,0,0),(255,0,0),(0,255,0)), ((0,0,255),(255,255,0),(255,0,255)),
     ((255,255,255),(0,255,255),(0,0,255)), ((255,0,255),(0,0,0),(255,255,0))),
    (((255,255,255),(0,0,255),(255,0,0)), ((0,255,0),(255,0,255),(0,0,0)),
     ((255,255,0),(255,0,0),(0,255,255)), ((0,0,0),(0,255,255),(255,0,255))),
)


def expected_rgb(cycle, view, frame):
    width, height = SIZES[3-frame if view else frame]
    palette = PALETTES[(cycle+view)%2][frame]
    pixels = bytearray()
    for y in reversed(range(height)):
        for x in range(width):
            colour = palette[0]
            if width//8 <= x < width*3//4 and height//5 <= y < height*2//3:
                colour = palette[1]
            if width//3 <= x < width*7//8 and height//3 <= y < height*7//8:
                colour = palette[2]
            pixels.extend(colour)
    return bytes(pixels)


def validate(text, output=None, software=False):
    mode = '--software' if software else '--native'
    assert text.count('ROCK5_GLVIEW_READY version=1 mode='+mode) == 1
    assert text.count('ROCK5_GLVIEW_VISIBLE cycle=1 frames=8') == 1
    assert text.count('ROCK5_GLVIEW_PASS cycles=2 contexts=4 frames=16 '
        'gl_pixels=84480 screen_pixels=84480 guard_bytes=2048 software='+str(int(software))) == 1
    for forbidden in ('ROCK5_GLVIEW_FAILURE', 'ROCK5_MESA_FAILURE', 'PANIC:',
            'Kernel Debugging Land', 'DEBUGGER:', 'mutex->owner', 'No EGL renderer'):
        assert forbidden not in text
    assert re.findall(r'ROCK5_GLVIEW_CYCLE_PASS cycle=(\d+) views=2 frames=8',text) == ['0','1']
    contexts = re.findall(r'^ROCK5_GLVIEW_CONTEXT cycle=(\d+) view=(\d+) '
        r'context=(0x[0-9a-f]+) renderer=(.*?) version=(.*?) recursive_lock=1\s*$', text, re.M)
    assert [(int(c),int(v)) for c,v,*_ in contexts] == [(0,0),(0,1),(1,0),(1,1)]
    for _, _, address, renderer, version in contexts:
        assert int(address,16) > 0
        assert renderer == ('softpipe' if software else 'Mali-G610 (Panfrost)')
        assert re.fullmatch(r'\d+\.\d+.*Mesa 25\.3\.6',version)
    assert contexts[0][2] != contexts[1][2] and contexts[2][2] != contexts[3][2]
    wanted = [(c,(f+t)%2,f) for c in range(2) for f in range(4) for t in range(2)]
    presentations = re.findall(r'^ROCK5_GLVIEW_PRESENT cycle=(\d+) view=(\d+) '
        r'frame=(\d+) update_complete=1 capture_count=1\s*$', text, re.M)
    assert [tuple(map(int,row)) for row in presentations] == wanted
    pattern = (r'^ROCK5_GLVIEW_PIXELS_BEGIN kind=(gl|screen) cycle=(\d+) view=(\d+) '
        r'frame=(\d+) width=(\d+) height=(\d+) stride=(\d+) format=(RGBA8|BGRX8) '
        r'origin=(lower-left|upper-left)\n(.*?)'
        r'^ROCK5_GLVIEW_PIXELS_END kind=(gl|screen) cycle=(\d+) view=(\d+) frame=(\d+) mismatches=(\d+)\s*$')
    records = re.findall(pattern,text,re.M|re.S)
    assert len(records) == 32
    frames = []
    for record, (cycle,view,frame,kind) in zip(records,
            [(c,v,f,k) for c,v,f in wanted for k in ('gl','screen')]):
        k,c,v,f,w,h,stride,fmt,origin,body,ek,ec,ev,ef,errors = record
        assert (int(c),int(v),int(f),k) == (cycle,view,frame,kind) == (int(ec),int(ev),int(ef),ek)
        width,height,stride = int(w),int(h),int(stride)
        assert (width,height) == SIZES[3-frame if view else frame]
        assert stride >= width*4 and stride%4 == 0 and errors == '0'
        assert (fmt,origin) == (('RGBA8','lower-left') if kind=='gl' else ('BGRX8','upper-left'))
        lines = body.splitlines()
        assert len(lines) == height
        rows=[]; raw=bytearray()
        for y,line in enumerate(lines):
            assert re.fullmatch(r'row=\d{3} [0-9a-f]{'+str(width*8)+'}',line)
            assert line.startswith(f'row={y:03d} ')
            pixels=bytes.fromhex(line[8:]); raw.extend(pixels)
            rgb=bytearray()
            for offset in range(0,len(pixels),4):
                if kind=='gl':
                    assert pixels[offset+3] == 255
                    rgb.extend(pixels[offset:offset+3])
                else:
                    rgb.extend((pixels[offset+2],pixels[offset+1],pixels[offset]))
            rows.append(rgb)
        rgb=b''.join(reversed(rows) if kind=='gl' else rows)
        assert rgb == expected_rgb(cycle,view,frame), (cycle,view,frame,kind)
        value=dict(cycle=cycle,view=view,frame=frame,kind=kind,width=width,height=height,
            stride=stride,pixels=width*height,raw_sha256=hashlib.sha256(raw).hexdigest(),
            rgb_sha256=hashlib.sha256(rgb).hexdigest())
        if output:
            root=Path(output); root.mkdir(parents=True,exist_ok=True)
            stem=f'cycle{cycle}-view{view}-frame{frame}-{kind}'
            saved=root/(stem+'.raw'); picture=root/(stem+'.png')
            saved.write_bytes(raw); png(picture,rgb,width,height)
            value.update(raw=str(saved),png=str(picture))
        frames.append(value)
    guards=re.findall(r'^ROCK5_GLVIEW_GUARDS cycle=(\d+) view=(\d+) frame=(\d+) '
        r'before=([0-9a-f]+) after=([0-9a-f]+)\s*$',text,re.M)
    assert len(guards)==16
    for (c,v,f,before,after),expected in zip(guards,wanted):
        assert (int(c),int(v),int(f))==expected
        assert before==after=='a5'*64
    screen=re.findall(r'^ROCK5_GLVIEW_SCREEN_PASS cycle=(\d+) view=(\d+) frame=(\d+) '
        r'x=(\d+) y=(\d+) width=(\d+) height=(\d+)\s*$',text,re.M)
    assert len(screen)==16
    for row,(c,v,f) in zip(screen,wanted):
        rc,rv,rf,x,y,w,h=map(int,row)
        assert (rc,rv,rf)==(c,v,f) and (w,h)==SIZES[3-f if v else f]
        assert x+w<=1920 and y+h<=1080
    result=dict(status='pass',software=software,cycles=2,contexts=4,frames=frames,
        gl_pixels=84480,screen_pixels=84480,guard_bytes=2048,recursive_locks=True,
        completed_window_updates=True,screen_captures=16,
        distinct_live_contexts=True,renderer=contexts[0][3],version=contexts[0][4])
    if not software:
        result.update(native_render_validation.native_evidence(text))
        assert all(len(c['queues'])>=3 and c['command_streams']>=16 for c in result['queue_cycles'])
    else:
        assert 'HAIKU_MESA_NATIVE_GPU' not in text
    return result
