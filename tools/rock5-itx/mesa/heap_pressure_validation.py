"""Complete-image oracle and live native heap-growth/accounting checks."""
import hashlib
from pathlib import Path
import re

import native_render_validation
from window_validation import png

WIDTH, HEIGHT = 97, 67
COLOURS = ((255, 0, 0, 255), (0, 255, 0, 255),
           (0, 0, 255, 255), (0, 255, 255, 255))


def validate(text, output=None, software=False):
    mode = '--software' if software else '--native'
    assert text.count('ROCK5_PRESSURE_READY version=1 mode='+mode+
        ' chunk_size=262144 initial_chunks=1 max_chunks=32') == 1
    assert text.count('ROCK5_PRESSURE_PASS contexts=2 frames=4 pixels=25996 '
        'guard_bytes=512 software='+str(int(software))) == 1
    for forbidden in ('ROCK5_PIPELINE_FAILURE', 'ROCK5_PIPELINE_SHADER_FAILURE',
            'ROCK5_PRESSURE_LINK_FAILURE', 'ROCK5_MESA_FAILURE', 'PANIC:',
            'Kernel Debugging Land', 'DEBUGGER:', 'mutex->owner'):
        assert forbidden not in text
    contexts = re.findall(r'^ROCK5_PRESSURE_GL cycle=(\d+) renderer=(.*?) version=(.*?)\s*$', text, re.M)
    assert [c for c, _, _ in contexts] == ['0', '1']
    for _, renderer, version in contexts:
        assert renderer == ('softpipe' if software else 'Mali-G610 (Panfrost)')
        assert re.fullmatch(r'OpenGL ES 3\.\d+ Mesa 25\.3\.6', version)
    assert re.findall(r'^ROCK5_PRESSURE_CONTEXT_PASS cycle=(\d+) rounds=2 destroyed=1\s*$',
        text, re.M) == ['0', '1']
    assert re.findall(r'^ROCK5_PRESSURE_VERTICES cycle=(\d+) vertices=98304 bytes=786432 explicit_buffer=1\s*$', text, re.M) == ['0', '1']
    pattern = (r'^ROCK5_PRESSURE_PIXELS_BEGIN cycle=(\d+) round=(\d+) quads=(\d+) '
        r'width=(\d+) height=(\d+) format=RGBA8 origin=lower-left\n(.*?)'
        r'^ROCK5_PRESSURE_PIXELS_END cycle=(\d+) round=(\d+) mismatches=(\d+) '
        r'guard_errors=(\d+) wait=([0-9a-f]+) render_us=(\d+)\s*$')
    records = re.findall(pattern, text, re.M | re.S)
    assert len(records) == 4
    assert text.count('ROCK5_PRESSURE_PIXELS_BEGIN ') == 4
    assert text.count('ROCK5_PRESSURE_PIXELS_END ') == 4
    frames = []
    for index, record in enumerate(records):
        cycle, round_ = divmod(index, 2)
        c, r, quads, w, h, body, ec, er, errors, damaged, wait, elapsed = record
        assert (int(c), int(r)) == (cycle, round_) == (int(ec), int(er))
        assert int(quads) == (8192, 16384)[round_]
        assert (int(w), int(h)) == (WIDTH, HEIGHT) and errors == damaged == '0'
        assert int(wait, 16) in (0x911a, 0x911c)
        # Host bound covers shader compilation and readback as well as submission.
        # The kernel retains its separate five-second per-job hardware deadline.
        assert 0 <= int(elapsed) < 60_000_000
        lines = body.splitlines()
        assert len(lines) == HEIGHT + 2
        guard = bytes([0xa0 + index]) * 64
        assert lines[0] == 'guard_before='+guard.hex()
        assert lines[-1] == 'guard_after='+guard.hex()
        raw = bytearray()
        for y, line in enumerate(lines[1:-1]):
            assert re.fullmatch(r'row=\d{3} [0-9a-f]{776}', line)
            assert line.startswith(f'row={y:03d} ')
            raw.extend(bytes.fromhex(line[8:]))
        assert raw == bytes(COLOURS[index]) * (WIDTH * HEIGHT), (cycle, round_)
        frame = dict(cycle=cycle, round=round_, quads=int(quads), vertices=int(quads)*6,
            width=WIDTH, height=HEIGHT, pixels=WIDTH*HEIGHT, guard_bytes=128,
            rgba_sha256=hashlib.sha256(raw).hexdigest(), render_us=int(elapsed))
        if output:
            root = Path(output)
            root.mkdir(parents=True, exist_ok=True)
            stem = root / f'cycle-{cycle}-round-{round_}'
            stem.with_suffix('.rgba').write_bytes(raw)
            png(stem.with_suffix('.png'), bytes(COLOURS[index][:3])*(WIDTH*HEIGHT), WIDTH, HEIGHT)
            frame.update(rgba=str(stem.with_suffix('.rgba')), png=str(stem.with_suffix('.png')))
        frames.append(frame)
    result = dict(status='pass', software=software, contexts=2, frames=frames,
        pixels=25996, guard_bytes=512, chunk_size=262144, initial_chunks=1, max_chunks=32)
    if software:
        assert 'HAIKU_MESA_NATIVE_' not in text and 'ROCK5_MESA_NATIVE_LIFETIME' not in text
        assert 'ROCK5_PRESSURE_HEAP' not in text
    else:
        result.update(native_render_validation.native_evidence(text))
        assert len(result['queue_cycles']) == 2
        heaps = re.findall(r'^ROCK5_PRESSURE_HEAP cycle=(\d+) phase=(\d+) clients=(\d+) '
            r'heaps=(\d+) chunks=(\d+) bytes=(\d+)\s*$', text, re.M)
        assert len(heaps) == text.count('ROCK5_PRESSURE_HEAP ') == 6
        states = []
        for index, values in enumerate(heaps):
            cycle, phase, clients, count, chunks, size = map(int, values)
            assert (cycle, phase) == divmod(index, 3) and (clients, count) == (2, 1)
            assert 1 <= chunks <= 32 and size == 4096 + chunks*262144
            assert chunks == 1 if phase == 0 else chunks > 1
            if phase == 2:
                assert chunks >= states[-1]['chunks']
            states.append(dict(cycle=cycle, phase=phase, chunks=chunks, bytes=size))
        result.update(heap_states=states,
            grown_chunks=sum(states[i]['chunks']-1 for i in (2, 5)))
    return result
