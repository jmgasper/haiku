"""Decode complete Mesa framebuffer readbacks and resource-lifetime evidence."""
import hashlib
import re
import struct
import zlib
from pathlib import Path

WIDTH = HEIGHT = 64

def expected_pixels(cycle, frame):
    assert cycle in (0, 1) and frame in (0, 1)
    # RGB tuples and integer pixel regions are independent of GPU clip coordinates.
    palette = {
        (0, 0): ((0,0,0), (255,0,0), (0,255,0)),
        (0, 1): ((0,0,255), (255,255,0), (255,0,255)),
        (1, 0): ((255,255,255), (0,255,255), (0,0,255)),
        (1, 1): ((255,0,255), (0,0,0), (255,255,0)),
    }[(cycle, frame)]
    first, second = (((8,48),(8,40)), ((24,56),(24,56))) if frame == 0 else (
        ((4,40),(12,52)), ((20,60),(4,44)))
    pixels = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            colour = palette[0]
            if x in range(*first[0]) and y in range(*first[1]): colour = palette[1]
            if x in range(*second[0]) and y in range(*second[1]): colour = palette[2]
            pixels.extend((*colour, 255))
    return bytes(pixels)

def png(path, pixels):
    """Write the actual lower-left-origin RGBA readback with normal PNG orientation."""
    def chunk(kind, data):
        return struct.pack('>I',len(data)) + kind + data + struct.pack('>I',zlib.crc32(kind+data))
    rows = b''.join(b'\0' + pixels[y*WIDTH*4:(y+1)*WIDTH*4] for y in reversed(range(HEIGHT)))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR',struct.pack('>IIBBBBB',WIDTH,HEIGHT,8,6,0,0,0))
        + chunk(b'IDAT',zlib.compress(rows)) + chunk(b'IEND',b''))

def validate(text, output=None, software=False):
    mode = '--software' if software else '--native'
    assert text.count('ROCK5_MESA_PROBE_READY version=2 mode='+mode) == 1
    assert text.count('ROCK5_MESA_RENDER_PASS contexts=2 rounds=4 pixels=16384 guard_bytes=512 software='+str(int(software))) == 1
    for forbidden in ('ROCK5_MESA_FAILURE','ROCK5_MESA_SHADER_FAILURE','PANIC:','Kernel Debugging Land'):
        assert forbidden not in text, forbidden
    assert re.findall(r'ROCK5_MESA_CONTEXT_PASS cycle=(\d+) rounds=2 destroyed=1',text) == ['0','1']
    assert re.findall(r'ROCK5_MESA_PBUFFER_PASS cycle=(\d+) width=64 height=64 swap_preserved=1',text) == ['0','1']
    assert text.count('ROCK5_MESA_BOUND_TERMINATE_PASS cycle=1 reinitialized=1 retired_handles=1') == 1
    gl = re.findall(r'^ROCK5_MESA_GL cycle=(\d+) renderer=(.*?) version=(.*?) vendor=(.*?)\s*$',text,re.M)
    assert len(gl) == 2 and [item[0] for item in gl] == ['0','1']
    for _, renderer, version, vendor in gl:
        if software: assert renderer == 'softpipe'
        else: assert 'Mali-G610' in renderer and 'Panfrost' in renderer and 'softpipe' not in renderer
        assert version.startswith('OpenGL ES 3.') and 'Mesa 25.3.6' in version
        assert vendor == 'Mesa'
    pattern = (r'^ROCK5_MESA_PIXELS_BEGIN cycle=(\d+) round=(\d+) width=64 height=64 '
        r'format=RGBA8 origin=lower-left\n(.*?)'
        r'^ROCK5_MESA_PIXELS_END cycle=(\d+) round=(\d+) mismatches=(\d+) '
        r'guard_errors=(\d+) wait=([0-9a-f]{4}) wait_ns=(\d+)\s*$')
    records = re.findall(pattern,text,re.M|re.S)
    assert len(records) == 4
    frames = []
    for record, expected_pair in zip(records,((0,0),(0,1),(1,0),(1,1))):
        cycle,frame,body,endcycle,endframe,mismatches,guard_errors,wait,ns = record
        cycle,frame = int(cycle),int(frame)
        assert (cycle,frame) == expected_pair == (int(endcycle),int(endframe))
        assert mismatches == guard_errors == '0'
        assert wait in ('911a','911c') and 0 <= int(ns) < 6000000000
        lines = body.splitlines()
        assert len(lines) == HEIGHT+2
        guard = bytes([0xa0+cycle*2+frame])*64
        assert lines[0] == 'guard_before='+guard.hex()
        assert lines[-1] == 'guard_after='+guard.hex()
        pixels = bytearray()
        for y,line in enumerate(lines[1:-1]):
            assert re.fullmatch(r'row=\d{2} [0-9a-f]{512}',line)
            assert line.startswith(f'row={y:02d} ')
            pixels.extend(bytes.fromhex(line[7:]))
        pixels = bytes(pixels)
        assert pixels == expected_pixels(cycle,frame), f'pixel mismatch cycle={cycle} frame={frame}'
        data = dict(cycle=cycle,frame=frame,bytes=len(pixels),pixels=WIDTH*HEIGHT,
            sha256=hashlib.sha256(pixels).hexdigest(),wait=wait,wait_ns=int(ns),guard_bytes=128)
        if output:
            directory=Path(output); directory.mkdir(parents=True,exist_ok=True)
            raw=directory/f'cycle{cycle}-frame{frame}.rgba'
            picture=directory/f'cycle{cycle}-frame{frame}.png'
            raw.write_bytes(pixels); png(picture,pixels)
            data.update(raw=str(raw),png=str(picture))
        frames.append(data)
    return dict(status='pass',software=software,renderer=gl[0][1],version=gl[0][2],
        contexts=2,frames=frames,pixels_checked=16384,guard_bytes_checked=512,
        pbuffer_readback=True,pbuffer_swap_preserved=True,bound_terminate_reinitialize=True)
