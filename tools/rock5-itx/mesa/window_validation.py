"""Check complete window bitmaps and independently captured screen pixels."""
import hashlib
import re
import struct
import zlib
from pathlib import Path

import native_render_validation

SIZES = ((64, 64), (79, 47), (128, 72), (65, 63))
PALETTES = (
    (((0,0,0),(255,0,0),(0,255,0)), ((0,0,255),(255,255,0),(255,0,255)),
     ((255,255,255),(0,255,255),(0,0,255)), ((255,0,255),(0,0,0),(255,255,0))),
    (((255,255,255),(0,0,255),(255,0,0)), ((0,255,0),(255,0,255),(0,0,0)),
     ((255,255,0),(255,0,0),(0,255,255)), ((0,0,0),(0,255,255),(255,0,255))),
)


def expected_rgb(cycle, frame):
    width, height = SIZES[frame]
    palette = PALETTES[cycle][frame]
    regions = ((range(width//8, width*3//4), range(height//5, height*2//3)),
        (range(width//3, width*7//8), range(height//3, height*7//8)))
    pixels = bytearray()
    for y in reversed(range(height)):
        for x in range(width):
            colour = palette[0]
            for index, (xs, ys) in enumerate(regions, 1):
                if x in xs and y in ys:
                    colour = palette[index]
            pixels.extend(colour)
    return bytes(pixels)


def png(path, pixels, width, height):
    def chunk(kind, data):
        return (struct.pack('>I',len(data)) + kind + data
            + struct.pack('>I',zlib.crc32(kind+data)))
    rows = b''.join(b'\0'+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))
        + chunk(b'IDAT',zlib.compress(rows)) + chunk(b'IEND',b''))


def validate(text, output=None, software=False):
    mode = '--software' if software else '--native'
    assert text.count('ROCK5_WINDOW_READY version=1 mode='+mode) == 1
    assert text.count('ROCK5_WINDOW_VISIBLE cycle=1 frame=3') == 1
    assert text.count('ROCK5_WINDOW_PASS contexts=2 frames=8 resize=6 '
        'bitmap_pixels=42240 screen_pixels=42240 software='+str(int(software))) == 1
    for forbidden in ('ROCK5_MESA_FAILURE','ROCK5_MESA_SHADER_FAILURE','PANIC:',
            'Kernel Debugging Land', 'mutex->owner'):
        assert forbidden not in text
    assert re.findall(r'ROCK5_WINDOW_CONTEXT_PASS cycle=(\d+) frames=4 resize=3 retired_bitmap=1',
        text) == ['0','1']
    gl = re.findall(r'^ROCK5_WINDOW_GL cycle=(\d+) renderer=(.*?) version=(.*?)\s*$',text,re.M)
    assert len(gl) == 2 and [row[0] for row in gl] == ['0','1']
    for _, renderer, version in gl:
        assert (renderer == 'softpipe' if software else renderer == 'Mali-G610 (Panfrost)')
        assert version.startswith('OpenGL ES 3.') and 'Mesa 25.3.6' in version

    pattern = (r'^ROCK5_WINDOW_PIXELS_BEGIN kind=(bitmap|screen) cycle=(\d+) frame=(\d+) '
        r'width=(\d+) height=(\d+) stride=(\d+) format=BGRX8 origin=upper-left\n(.*?)'
        r'^ROCK5_WINDOW_PIXELS_END kind=(bitmap|screen) cycle=(\d+) frame=(\d+) mismatches=(\d+)\s*$')
    records = re.findall(pattern,text,re.M|re.S)
    wanted = [(cycle,frame,kind) for cycle in range(2) for frame in range(4)
        for kind in ('bitmap','screen')]
    assert len(records) == len(wanted)
    frames = []
    for record, (cycle,frame,kind) in zip(records,wanted):
        rk,rc,rf,width,height,stride,body,ek,ec,ef,errors = record
        assert (int(rc),int(rf),rk) == (cycle,frame,kind) == (int(ec),int(ef),ek)
        assert errors == '0'
        width,height,stride = map(int,(width,height,stride))
        assert (width,height) == SIZES[frame] and stride >= width*4 and stride%4 == 0
        lines = body.splitlines()
        assert len(lines) == height
        rgba = bytearray()
        rgb = bytearray()
        for y,line in enumerate(lines):
            assert re.fullmatch(r'row=\d{3} [0-9a-f]{'+str(width*8)+'}',line)
            assert line.startswith(f'row={y:03d} ')
            row = bytes.fromhex(line[8:])
            rgba.extend(row)
            for i in range(0,len(row),4):
                rgb.extend((row[i+2],row[i+1],row[i]))
                if kind == 'bitmap':
                    assert row[i+3] == 255
        assert rgb == expected_rgb(cycle,frame), (cycle,frame,kind)
        value = dict(cycle=cycle,frame=frame,kind=kind,width=width,height=height,
            stride=stride,pixels=width*height,bgrx_sha256=hashlib.sha256(rgba).hexdigest(),
            rgb_sha256=hashlib.sha256(rgb).hexdigest())
        if output:
            root=Path(output); root.mkdir(parents=True,exist_ok=True)
            stem=f'cycle{cycle}-frame{frame}-{kind}'
            raw=root/(stem+'.bgrx'); picture=root/(stem+'.png')
            raw.write_bytes(rgba); png(picture,rgb,width,height)
            value.update(raw=str(raw),png=str(picture))
        frames.append(value)
    screen = re.findall(r'^ROCK5_WINDOW_SCREEN_PASS cycle=(\d+) frame=(\d+) x=(\d+) y=(\d+) width=(\d+) height=(\d+)\s*$',text,re.M)
    assert len(screen) == 8
    for item, (cycle,frame) in zip(screen,[(c,f) for c in range(2) for f in range(4)]):
        c,f,x,y,w,h = map(int,item)
        assert (c,f) == (cycle,frame) and (w,h) == SIZES[frame]
        assert x+w <= 1920 and y+h <= 1080
    result=dict(status='pass',software=software,contexts=2,frames=frames,
        bitmap_pixels=42240,screen_pixels=42240,resizes=6,retired_bitmaps=True,
        renderer=gl[0][1],version=gl[0][2])
    if not software:
        result.update(native_render_validation.native_evidence(text))
        assert all(c['command_streams'] >= 5 for c in result['queue_cycles'])
    else:
        assert 'HAIKU_MESA_NATIVE_GPU' not in text
    return result
