"""Independent complete-image GLES texture/depth/stencil/blend/scissor oracle."""
import hashlib
from pathlib import Path
import re

import native_render_validation
from window_validation import png

WIDTH, HEIGHT = 97, 67
CASES = ('texture', 'depth', 'stencil', 'blend', 'scissor', 'render_texture')


def expected(cycle, case):
    # Deliberately compute each RGB channel, rather than copying the probe's
    # packed colour bitfield and rendering branches.
    pixels = bytearray()
    for row in range(HEIGHT):
        for column in range(WIDTH):
            x, y = (WIDTH-1-column, HEIGHT-1-row) if case == 'render_texture' else (column, row)
            a = 12 <= x < 72 and 13 <= y < 44
            b = 32 <= x < 84 and 22 <= y < 58
            rgb = [255 * (((x//7) >> channel ^ (y//5) >> channel ^ cycle >> channel) & 1)
                   for channel in range(3)]
            if case in ('depth', 'render_texture'):
                if b:
                    rgb = [0, 255, 0]
                elif a:
                    rgb = [255, 0, 0]
            elif case == 'stencil' and a and b:
                rgb = [0, 0, 255]
            elif case == 'blend':
                rgb[0] = min(255, rgb[0] + 255 * a)
                rgb[1] = min(255, rgb[1] + 255 * b)
            elif case == 'scissor' and a:
                rgb = [0, 255, 0] if b else [0, 0, 255]
            pixels.extend((*rgb, 255))
    return bytes(pixels)


def validate(text, output=None, software=False):
    mode = '--software' if software else '--native'
    assert text.count('ROCK5_PIPELINE_READY version=1 mode='+mode) == 1
    assert text.count('ROCK5_PIPELINE_PASS contexts=2 cases=12 pixels=77988 '
        'guard_bytes=1536 upload_bytes=54928 software='+str(int(software))) == 1
    for forbidden in ('ROCK5_PIPELINE_FAILURE', 'ROCK5_PIPELINE_SHADER_FAILURE',
            'ROCK5_PIPELINE_LINK_FAILURE', 'ROCK5_MESA_FAILURE', 'PANIC:',
            'Kernel Debugging Land', 'DEBUGGER:', 'mutex->owner'):
        assert forbidden not in text
    contexts = re.findall(r'^ROCK5_PIPELINE_GL cycle=(\d+) renderer=(.*?) version=(.*?)\s*$', text, re.M)
    assert [c for c, _, _ in contexts] == ['0', '1']
    for _, renderer, version in contexts:
        assert renderer == ('softpipe' if software else 'Mali-G610 (Panfrost)')
        assert re.fullmatch(r'OpenGL ES 3\.\d+ Mesa 25\.3\.6', version)
    assert re.findall(r'^ROCK5_PIPELINE_TARGET cycle=(\d+) width=97 height=67 '
        r'depth_bits=24 stencil_bits=8 upload_stride=408\s*$', text, re.M) == ['0', '1']
    assert re.findall(r'^ROCK5_PIPELINE_CONTEXT_PASS cycle=(\d+) cases=6 destroyed=1\s*$',
        text, re.M) == ['0', '1']
    assert re.findall(r'^ROCK5_PIPELINE_UPLOAD_PASS cycle=(\d+) bytes=27464 pixels=6499 '
        r'padding_bytes=1340 guard_bytes=128 unchanged=1\s*$', text, re.M) == ['0', '1']
    pattern = (r'^ROCK5_PIPELINE_PIXELS_BEGIN cycle=(\d+) case=(\w+) width=(\d+) height=(\d+) '
        r'format=RGBA8 origin=lower-left\n(.*?)'
        r'^ROCK5_PIPELINE_PIXELS_END cycle=(\d+) case=(\w+) mismatches=(\d+) '
        r'guard_errors=(\d+) wait=([0-9a-f]+) wait_ns=(\d+)\s*$')
    records = re.findall(pattern, text, re.M | re.S)
    assert len(records) == 12
    assert text.count('ROCK5_PIPELINE_PIXELS_BEGIN ') == 12
    assert text.count('ROCK5_PIPELINE_PIXELS_END ') == 12
    frames = []
    for record, (cycle, case) in zip(records, [(c, k) for c in range(2) for k in CASES]):
        c, name, w, h, body, ec, ename, errors, damaged, wait, wait_ns = record
        assert (int(c), name) == (cycle, case) == (int(ec), ename)
        assert (int(w), int(h)) == (WIDTH, HEIGHT) and errors == damaged == '0'
        assert int(wait, 16) in (0x911a, 0x911c) and 0 <= int(wait_ns) < 6_000_000_000
        lines = body.splitlines()
        assert len(lines) == HEIGHT + 2
        guard = bytes([0xa0 + cycle*6 + CASES.index(case)]) * 64
        assert lines[0] == 'guard_before='+guard.hex()
        assert lines[-1] == 'guard_after='+guard.hex()
        raw = bytearray()
        for y, line in enumerate(lines[1:-1]):
            assert re.fullmatch(r'row=\d{3} [0-9a-f]{776}', line)
            assert line.startswith(f'row={y:03d} ')
            raw.extend(bytes.fromhex(line[8:]))
        assert raw == expected(cycle, case), (cycle, case)
        result = dict(cycle=cycle, case=case, width=WIDTH, height=HEIGHT,
            pixels=WIDTH*HEIGHT, guard_bytes=128, rgba_sha256=hashlib.sha256(raw).hexdigest(),
            wait_ns=int(wait_ns))
        if output:
            root = Path(output)
            root.mkdir(parents=True, exist_ok=True)
            stem = root / f'cycle-{cycle}-{case}'
            stem.with_suffix('.rgba').write_bytes(raw)
            rgb = bytearray()
            for y in reversed(range(HEIGHT)):
                for x in range(WIDTH):
                    offset = (y*WIDTH+x)*4
                    rgb.extend(raw[offset:offset+3])
            png(stem.with_suffix('.png'), bytes(rgb), WIDTH, HEIGHT)
            result['rgba'] = str(stem.with_suffix('.rgba'))
            result['png'] = str(stem.with_suffix('.png'))
        frames.append(result)
    result = dict(status='pass', software=software, contexts=2, cases=12,
        pixels=77988, guard_bytes=1536, upload_bytes=54928,
        upload_padding_bytes=2680, upload_guard_bytes=256, frames=frames)
    if software:
        assert 'HAIKU_MESA_NATIVE_' not in text and 'ROCK5_MESA_NATIVE_LIFETIME' not in text
    else:
        result.update(native_render_validation.native_evidence(text))
        assert len(result['queue_cycles']) == 2
        assert all(c['command_streams'] >= 6 for c in result['queue_cycles'])
    return result
