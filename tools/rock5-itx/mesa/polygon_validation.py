"""Check full polygon-mode readbacks independently of the guest counters."""
import hashlib
from pathlib import Path
import re

NAMES = ('fill', 'line', 'fill-restored', 'point', 'line-indexed',
    'line-cull-back-front', 'line-cull-back-back', 'line-cull-front-front',
    'line-cull-front-back', 'line-cull-all', 'split-front', 'split-back',
    'line-edge-flag', 'final-fill', 'clipped-line', 'clipped-fill')
KINDS = ('fill','line','fill','point','line','line','empty','empty','line',
    'empty','line','fill','edge-flag','fill','clipped-line','clipped-fill')
EXPECTED = (1,2,1,3,2,2,0,0,2,0,2,1,2,1,4,5)


def validate(text, output=None, software=True, haiku=True, require_pass=True):
    headers = re.findall(r'^ROCK5_POLYGON_GL cycle=(\d+) renderer=(.*?) version=(.*?)$', text, re.M)
    assert [int(x[0]) for x in headers] == [0,1]
    assert text.count('ROCK5_POLYGON_GL ') == 2
    for _, renderer, version in headers:
        assert renderer == 'softpipe' if software else 'Mali-G610' in renderer and 'Panfrost' in renderer
        assert not version.startswith('OpenGL ES')
        if haiku:
            assert 'Mesa 25.3.6' in version
    if software:
        assert 'HAIKU_MESA_NATIVE_GPU' not in text
    frames = list(re.finditer(r'^ROCK5_POLYGON_PIXELS_BEGIN cycle=(\d+) case=(\d+) name=(\S+) width=64 height=64 format=RGBA8 origin=lower-left\n(.*?)^ROCK5_POLYGON_PIXELS_END (.*?)$', text, re.M | re.S))
    assert len(frames) == text.count('ROCK5_POLYGON_PIXELS_BEGIN ') == text.count('ROCK5_POLYGON_PIXELS_END ') == 32
    checked, pixels = [], {}
    if output is not None:
        output = Path(output); output.mkdir(parents=True, exist_ok=True)
    for i, frame in enumerate(frames):
        cycle, case = divmod(i,16)
        assert frame.group(1,2,3) == (str(cycle),str(case),NAMES[case])
        lines = frame[4].splitlines()
        assert len(lines) == 66
        guard = bytes([0x80 + 16*cycle + case])*64
        assert lines[0] == 'guard_before=' + guard.hex()
        assert lines[-1] == 'guard_after=' + guard.hex()
        rgba = bytearray()
        for y, line in enumerate(lines[1:-1]):
            assert re.fullmatch(r'row='+f'{y:02d}'+r' [0-9a-f]{512}',line)
            rgba += bytes.fromhex(line[7:])
        assert len(rgba) == 16384
        mask, other = set(), 0
        for p in range(4096):
            value = bytes(rgba[p*4:p*4+4])
            if value == bytes((255,0,0,255)):
                mask.add((p%64,p//64))
            elif value != bytes((0,0,0,255)):
                other += 1
        interior = len(mask & {(x,y) for x in range(28,36) for y in range(22,30)})
        edge = len(mask & {(x,y) for x in range(20,44) for y in range(7,9)})
        clip = len(mask & {(x,y) for x in range(15,17) for y in range(10,22)})
        count = len(mask)
        kind = KINDS[case]
        semantic = {
            'empty': count == 0,
            'fill': 1000 <= count <= 1200 and interior == 64,
            'line': 70 <= count <= 160 and interior == 0 and edge >= 20,
            'point': 3 <= count <= 27 and interior == 0 and edge == 0,
            'edge-flag': 70 <= count <= 160 and interior == 0 and edge == 0,
            'clipped-line': 100 <= count <= 220 and interior == 0 and edge >= 20 and clip >= 10,
            'clipped-fill': 1000 <= count <= 1100 and interior == 64 and clip == 12,
        }[kind]
        fields = re.fullmatch(r'cycle=(\d+) case=(\d+) front=([0-9a-f]{4}) back=([0-9a-f]{4}) error=([0-9a-f]{4}) coloured=(\d+) other=(\d+) interior=(\d+) edge=(\d+) clip_edge=(\d+) guard_errors=(\d+) expected=(\d+) pass=([01])',frame[5])
        assert fields is not None
        assert (int(fields[1]),int(fields[2])) == (cycle,case)
        front = 0x1b00 if case == 3 else 0x1b02 if case in (0,2,13,15) else 0x1b01
        back = 0x1b02 if case in (10,11) else front
        state_ok = (int(fields[3],16),int(fields[4],16),int(fields[5],16)) == (front,back,0)
        assert tuple(map(int,fields.group(6,7,8,9,10,11,12))) == (count,other,interior,edge,clip,0,EXPECTED[case])
        passed = semantic and state_ok and other == 0
        assert int(fields[13]) == int(passed)
        pixels[cycle,case] = bytes(rgba)
        row = dict(cycle=cycle,case=case,name=NAMES[case],kind=kind,pass_=passed,
            state_ok=state_ok,coloured=count,other=other,interior=interior,edge=edge,
            clip_edge=clip,rgba_sha256=hashlib.sha256(rgba).hexdigest())
        if output is not None:
            path = output/f'cycle{cycle}-case{case:02d}.rgba'; path.write_bytes(rgba)
            row['rgba'] = str(path)
        checked.append(row)
    endings = re.findall(r'^ROCK5_POLYGON_CONTEXT_END cycle=(\d+) cases=16 passed=(\d+) normal_cleanup=1 pass=([01])$',text,re.M)
    assert len(endings) == text.count('ROCK5_POLYGON_CONTEXT_END ') == 2
    for cycle, ending in enumerate(endings):
        count = sum(c['pass_'] for c in checked if c['cycle'] == cycle)
        assert tuple(map(int,ending)) == (cycle,count,int(count == 16))
        # Restoration and equivalent paths must retain identical complete images.
        # Reversed line direction can legitimately change endpoint coverage.
        for a,b in [(0,2),(0,13),(0,11),(1,4),(1,5),(1,10),(6,7),(6,9)]:
            assert pixels[cycle,a] == pixels[cycle,b], (cycle,a,b)
    passed = all(c['pass_'] for c in checked)
    assert re.findall(r'^ROCK5_POLYGON_RESULT contexts=2 cases=32 pass=([01])$',text,re.M) == [str(int(passed))]
    assert text.count('ROCK5_POLYGON_RESULT ') == 1
    if require_pass:
        assert passed, [c['name'] for c in checked if not c['pass_']]
    return dict(status='pass' if passed else 'rendering_failure',software=software,
        native_qualification=False,contexts=2,frames=32,pixels=131072,guard_bytes=4096,
        passed=sum(c['pass_'] for c in checked),frames_checked=checked,renderers=headers)
