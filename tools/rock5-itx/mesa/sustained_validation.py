"""Expand actual RGBA runs and validate every frame of sustained rendering."""
import hashlib
import json
from pathlib import Path
import re
import struct

import native_render_validation

WIDTH, HEIGHT = 97, 67
PIXELS = WIDTH * HEIGHT
COLOURS = (bytes.fromhex('ff0000ff'), bytes.fromhex('00ff00ff'),
           bytes.fromhex('0000ffff'), bytes.fromhex('00ffffff'))
FRAME = re.compile(
    r'^ROCK5_SUSTAIN_FRAME cycle=(\d+) frame=(\d+) quads=(\d+) '
    r'width=(\d+) height=(\d+) format=RGBA8 wait=([0-9a-f]+) '
    r'render_us=(\d+) mismatches=(\d+) guard_errors=(\d+) '
    r'guard_before=([0-9a-f]+) guard_after=([0-9a-f]+) rgba_runs=(\S+)\s*$', re.M)


def decode_runs(encoded):
    """Bound expansion before allocating; retain alpha and unexpected colours."""
    values = encoded.split(',')
    assert 1 <= len(values) <= PIXELS
    raw = bytearray()
    previous = None
    for value in values:
        match = re.fullmatch(r'([1-9]\d*):([0-9a-f]{8})', value)
        assert match
        length = int(match[1])
        pixel = bytes.fromhex(match[2])
        assert 1 <= length <= PIXELS and pixel != previous
        assert len(raw) + length * 4 <= PIXELS * 4
        raw.extend(pixel * length)
        previous = pixel
    assert len(raw) == PIXELS * 4
    return bytes(raw)


def validate(text, output=None, software=False):
    tag = 'ROCK5_SUSTAIN'
    mode = '--software' if software else '--native'
    assert text.count(tag+'_READY version=2 mode='+mode+
        ' chunk_size=262144 initial_chunks=1 encoding=rgba-rle') == 1
    for forbidden in ('ROCK5_PIPELINE_FAILURE', 'ROCK5_PIPELINE_SHADER_FAILURE',
            'ROCK5_SUSTAIN_LINK_FAILURE', 'ROCK5_MESA_FAILURE', 'PANIC:',
            'Kernel Debugging Land', 'DEBUGGER:', 'mutex->owner'):
        assert forbidden not in text
    contexts = re.findall(r'^ROCK5_SUSTAIN_GL cycle=(\d+) renderer=(.*?) version=(.*?)\s*$',
        text, re.M)
    assert [c for c, _, _ in contexts] == ['0', '1']
    for _, renderer, version in contexts:
        assert renderer == ('softpipe' if software else 'Mali-G610 (Panfrost)')
        assert re.fullmatch(r'OpenGL ES 3\.\d+ Mesa 25\.3\.6', version)
    vertices = re.findall(r'^ROCK5_SUSTAIN_VERTICES cycle=(\d+) '
        r'vertices=98304 bytes=786432 explicit_buffer=1\s*$', text, re.M)
    assert vertices == ['0', '1']
    beginnings = list(re.finditer(r'^ROCK5_SUSTAIN_CONTEXT_BEGIN cycle=(\d+) '
        r'max_chunks=(\d+) min_frames=(\d+) max_frames=(\d+) min_render_us=(\d+) '
        r'max_wall_us=(\d+) stable_render_us=(\d+) stable_frames=(\d+) '
        r'sample_frames=(\d+)\s*$', text, re.M))
    endings = list(re.finditer(r'^ROCK5_SUSTAIN_CONTEXT_PASS cycle=(\d+) '
        r'frames=(\d+) pixels=(\d+) guard_bytes=(\d+) render_us=(\d+) wall_us=(\d+) '
        r'destroyed=1\s*$', text, re.M))
    assert len(beginnings) == len(endings) == 2
    assert text.count(tag+'_CONTEXT_BEGIN ') == text.count(tag+'_CONTEXT_PASS ') == 2
    root = Path(output) if output else None
    if root:
        root.mkdir(parents=True, exist_ok=True)
    cycles = []
    all_pixels = hashlib.sha256()
    frame_count = 0
    incremental_total = 0
    for cycle, (begin, end) in enumerate(zip(beginnings, endings)):
        assert begin.start() < end.start()
        if cycle:
            assert endings[cycle-1].end() < begin.start()
        expected_maximum = 1 if cycle else 32
        assert tuple(map(int, begin.groups())) == (cycle, expected_maximum,
            32 if software else 1024, 32 if software else 65536,
            0 if software else 60000000, 180000000,
            0 if software else 20000000, 0 if software else 1024,
            0 if software else 128)
        ec, count, pixels, guards, elapsed, wall = map(int, end.groups())
        assert ec == cycle and 0 <= elapsed <= wall < 180000000
        assert (count == 32) if software else (1024 <= count <= 65536 and elapsed >= 60000000)
        assert pixels == count * PIXELS and guards == count * 128
        body = text[begin.end():end.start()]
        matches = list(FRAME.finditer(body))
        assert len(matches) == body.count(tag+'_FRAME ') == count
        durations = 0
        incremental = 0
        previous = 0
        samples = []
        cumulative_times = []
        aggregate = hashlib.sha256()
        metadata = (root/f'cycle-{cycle}.frames.jsonl').open('w') if root else None
        try:
            for frame, match in enumerate(matches):
                c, f, quads, width, height, wait, duration, errors, damaged, before, after, runs = match.groups()
                assert (int(c), int(f)) == (cycle, frame)
                assert (int(width), int(height)) == (WIDTH, HEIGHT)
                assert int(quads) == (8192, 16384)[frame % 2]
                assert int(wait, 16) in (0x911a, 0x911c)
                duration = int(duration)
                assert 0 <= duration < 60000000 and errors == damaged == '0'
                guard = bytes([0x40 + (cycle * 53 + frame) % 191]).hex() * 64
                assert before == after == guard
                raw = decode_runs(runs)
                assert raw == COLOURS[(cycle + frame) % 4] * PIXELS, (cycle, frame)
                digest = hashlib.sha256(raw).hexdigest()
                index = struct.pack('<III', cycle, frame, len(raw))
                aggregate.update(index); aggregate.update(raw)
                all_pixels.update(index); all_pixels.update(raw)
                if frame < 4:
                    samples.append(digest)
                counters = re.findall(r'Incremental rendering was triggered (\d+) time\(s\)',
                    body[previous:match.start()])
                if software or cycle == 0:
                    assert not counters
                    passes = 0
                else:
                    assert len(counters) == 1 and 0 < int(counters[0]) <= 64
                    passes = int(counters[0])
                previous = match.end()
                incremental += passes
                durations += duration
                cumulative_times.append(durations)
                if metadata:
                    metadata.write(json.dumps(dict(cycle=cycle, frame=frame,
                        quads=int(quads), pixels=PIXELS, guard_bytes=128,
                        rgba_sha256=digest, render_us=duration, incremental_passes=passes))+'\n')
        finally:
            if metadata:
                metadata.close()
        assert durations == elapsed
        assert not re.search(r'Incremental rendering was triggered ', body[previous:])
        progress = list(re.finditer(r'^ROCK5_SUSTAIN_HEAP_PROGRESS cycle=(\d+) '
            r'frames=(\d+) render_us=(\d+) clients=(\d+) heaps=(\d+) '
            r'chunks=(\d+) bytes=(\d+)\s*$', body, re.M))
        assert len(progress) == body.count(tag+'_HEAP_PROGRESS ')
        heap_history = []
        last_change_frame, last_change_us = 2, cumulative_times[1]
        if software:
            assert not progress
        else:
            assert count % 128 == 0 and len(progress) == count // 128
            warm = re.findall(r'^ROCK5_SUSTAIN_HEAP cycle='+str(cycle)+
                r' phase=1 clients=2 heaps=1 chunks=(\d+) bytes=(\d+)\s*$', body, re.M)
            assert len(warm) == 1
            previous_chunks = int(warm[0][0])
            for index, observation in enumerate(progress, 1):
                c, f, timestamp, clients, heaps, chunks, size = map(int, observation.groups())
                assert c == cycle and f == index * 128
                assert timestamp == cumulative_times[f-1]
                assert matches[f-1].end() <= observation.start()
                if f < count:
                    assert observation.end() <= matches[f].start()
                assert clients == 2 and heaps == 1
                assert previous_chunks <= chunks <= expected_maximum
                assert size == 4096 + chunks * 262144
                if chunks != previous_chunks:
                    last_change_frame, last_change_us = f, timestamp
                previous_chunks = chunks
                heap_history.append(dict(frames=f, render_us=timestamp, chunks=chunks, bytes=size))
            assert count - last_change_frame >= 1024
            assert elapsed - last_change_us >= 20000000
        value = dict(cycle=cycle, max_chunks=expected_maximum, frames=count,
            pixels=pixels, guard_bytes=guards, render_us=elapsed, wall_us=wall,
            aggregate_sha256=aggregate.hexdigest(), first_four_rgba_sha256=samples,
            incremental_passes=incremental)
        if not software:
            value.update(heap_progress=heap_history, stable_frames=count-last_change_frame,
                stable_render_us=elapsed-last_change_us)
        if root:
            value['frame_metadata'] = str(root/f'cycle-{cycle}.frames.jsonl')
        cycles.append(value)
        frame_count += count
        incremental_total += incremental
    assert text.count(tag+'_FRAME ') == frame_count
    assert text.count(tag+'_HEAP_PROGRESS ') == sum(len(c.get('heap_progress', [])) for c in cycles)
    total = re.findall(r'^ROCK5_SUSTAIN_PASS contexts=2 frames=(\d+) pixels=(\d+) '
        r'guard_bytes=(\d+) render_us=(\d+) software=(\d+)\s*$', text, re.M)
    assert len(total) == text.count(tag+'_PASS ') == 1
    assert tuple(map(int, total[0])) == (frame_count, frame_count * PIXELS,
        frame_count * 128, sum(c['render_us'] for c in cycles), int(software))
    counters = list(map(int, re.findall(r'Incremental rendering was triggered (\d+) time\(s\)', text)))
    assert sum(counters) == incremental_total
    assert len(counters) == (0 if software else cycles[1]['frames'])
    result = dict(status='pass', software=software, contexts=cycles, frames=frame_count,
        pixels=frame_count * PIXELS, guard_bytes=frame_count * 128,
        render_us=sum(c['render_us'] for c in cycles),
        wall_us=sum(c['wall_us'] for c in cycles), incremental_passes=incremental_total,
        aggregate_sha256=all_pixels.hexdigest(), lossless_actual_readbacks=True)
    if software:
        assert 'HAIKU_MESA_NATIVE_' not in text and 'ROCK5_MESA_NATIVE_LIFETIME' not in text
        assert tag+'_HEAP ' not in text
        return result
    result.update(native_render_validation.native_evidence(text))
    assert len(result['queue_cycles']) == 2
    for context, queues in zip(cycles, result['queue_cycles']):
        assert queues['command_streams'] >= context['frames']
        assert queues['submitted'] == queues['completed'] >= context['frames']
    heaps = re.findall(r'^ROCK5_SUSTAIN_HEAP cycle=(\d+) phase=(\d+) clients=(\d+) '
        r'heaps=(\d+) chunks=(\d+) bytes=(\d+)\s*$', text, re.M)
    assert len(heaps) == text.count(tag+'_HEAP ') == 6
    states = []
    for index, row in enumerate(heaps):
        cycle, phase, clients, count, chunks, size = map(int, row)
        assert (cycle, phase) == divmod(index, 3) and clients == 2 and count == 1
        assert 1 <= chunks <= (1 if cycle else 32) and size == 4096 + chunks * 262144
        assert (chunks == 1) if cycle or phase == 0 else (chunks > 1)
        if phase == 2:
            assert chunks >= states[-1]['chunks']
            assert chunks == cycles[cycle]['heap_progress'][-1]['chunks']
        states.append(dict(cycle=cycle, phase=phase, chunks=chunks, bytes=size))
    result.update(heap_states=states, grown_chunks=states[2]['chunks']-1)
    return result
