#!/usr/bin/env python3
"""Pack FFprobe packets for the native Media Kit test; verify pixels and PTS."""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def probe(path, packets=False):
    return json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams',
        *(['-show_packets'] if packets else []), '-show_data', '-of', 'json', str(path)]))


def unhex(data):
    return bytes.fromhex(''.join(line.split(':', 1)[1].split('  ', 1)[0]
                                for line in data.splitlines() if ':' in line))


def pack(source, output):
    info = probe(source, True)
    stream = info['streams'][0]
    extra = unhex(stream['extradata'])
    packets = info['packets']
    with open(output, 'wb') as f:
        f.write(struct.pack('<5I', 0x314b5055, stream['width'], stream['height'], len(extra), len(packets)))
        f.write(extra)
        for p in packets:
            data = unhex(p['data'])
            assert len(data) == int(p['size']) and 0 < len(data) <= 4 * 1024 * 1024
            pts = round(float(p['pts_time']) * 1000000)
            f.write(struct.pack('<IqI', len(data), pts, 'K' in p['flags']))
            f.write(data)
    print(f'packed {len(packets)} packets, {len(extra)} avcC bytes: {output}')


def verify(source, output):
    info = probe(source, True)
    stream = info['streams'][0]
    width, height = stream['width'], stream['height']
    frame_size = width * height * 3 // 2
    reference = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(source),
        '-an', '-pix_fmt', 'nv12', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'])
    times = sorted(round(float(p['pts_time']) * 1000000) for p in info['packets'])
    assert len(reference) == len(times) * frame_size
    first_key = next(round(float(p['pts_time']) * 1000000) for p in info['packets'][1:] if 'K' in p['flags'])
    data = Path(output).read_bytes()
    at = 0
    stages = {}
    compared = 0
    while at < len(data):
        stage, w, h, space, size, pts = struct.unpack_from('<5Iq', data, at)
        at += 28
        assert w == width and h == height and at + size <= len(data)
        index = len(stages.setdefault(stage, []))
        expected_times = [t for t in times if t >= first_key] if stage == 3 else times
        assert index < len(expected_times) and pts == expected_times[index], (stage, index, pts, expected_times[index])
        ref_index = times.index(pts)
        ref = reference[ref_index * frame_size:(ref_index + 1) * frame_size]
        if space == 0x49343230:
            luma = width * height
            ref = ref[:luma] + ref[luma::2] + ref[luma+1::2]
        elif space == 0x4000:  # B_YCbCr422: Y0 Cb Y1 Cr
            packed = bytearray(width * height * 2)
            luma = width * height
            for y in range(height):
                row = ref[y * width:(y + 1) * width]
                uv = ref[luma + y // 2 * width:luma + (y // 2 + 1) * width]
                packed[y * width * 2:(y + 1) * width * 2:2] = row
                packed[y * width * 2 + 1:(y + 1) * width * 2:2] = uv
            ref = packed
        else:
            assert space == 0x4e563132, hex(space)
        actual = data[at:at + size]
        assert len(ref) == size
        assert actual == ref, (stage, index, pts, "pixel mismatch")
        at += size
        compared += size
        stages[stage].append(pts)
    for stage, actual in stages.items():
        expected_times = [t for t in times if t >= first_key] if stage == 3 else times
        assert actual == expected_times
        print(f'stage {stage}: {len(actual)} pictures, exact pixels and presentation timestamps')
    assert stages
    print(f'PASS: {sum(map(len, stages.values()))} pictures, {compared} bytes; zero pixel or timestamp differences')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--pack', type=Path)
    action.add_argument('--verify', type=Path)
    args = parser.parse_args()
    if args.pack:
        pack(args.source, args.pack)
    else:
        verify(args.source, args.verify)
