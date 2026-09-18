"""Decode and check native RK3588 display observation transcripts.

The probe prints raw register words; this module re-derives the video-port
timing, interface routing and hot-plug state independently of the probe's own
decoded lines and requires every sample to agree. Nothing here proves scanout
or a working display; it establishes which firmware-programmed state Haiku
inherited, with no register writes.
"""
import re

PMU_COUNT = 11
CRU_SELECT_COUNT = 4
CRU_GATE_COUNT = 6
SYS_GRF_COUNT = 3
VOP_SYS_COUNT = 27
VOP_OVL_COUNT = 7
VOP_VP_COUNT = 14
VOP_CLUSTER_COUNT = 8
VOP_ESMART_COUNT = 7
HDMI_COUNT = 10

FLAG_READ_ONLY = 1
FLAG_VOP_READ = 2
FLAG_HDMI_READ = 4
FLAG_VOP_SKIPPED = 8
FLAG_HDMI_SKIPPED = 16

INTERFACES = ['dp0', 'dp1', 'edp0', 'hdmi0', 'edp1', 'hdmi1', 'mipi0', 'mipi1', 'rgb']


class ValidationError(AssertionError):
    pass


def _words(line, count):
    values = [int(word, 16) for word in line.split(' ', 1)[1].split(',')]
    if len(values) != count:
        raise ValidationError('unexpected word count in %r' % line)
    return values


def _samples(body, label):
    pattern = re.compile(r'^ROCK5_DISPLAY_' + re.escape(label) + r' sample=(\d+) ([0-9a-f,]+)$', re.M)
    found = {}
    for match in pattern.finditer(body):
        index = int(match.group(1))
        if index in found:
            raise ValidationError('duplicate %s sample %d' % (label, index))
        found[index] = 'x ' + match.group(2)
    return found


def decode_port(words):
    """Video port timing from DSP_CTRL, HTOTAL/HS_END, HACT_ST_END, VTOTAL/VS_END, VACT_ST_END."""
    control, htotal, hact, vtotal, vact = words[0], words[10], words[11], words[12], words[13]
    return dict(standby=(control >> 31) & 1, out_mode=control & 0xf,
        htotal=(htotal >> 16) & 0x1fff, hsync_end=htotal & 0x1fff,
        hactive_start=(hact >> 16) & 0x1fff, hactive_end=hact & 0x1fff,
        vtotal=(vtotal >> 16) & 0x1fff, vsync_end=vtotal & 0x1fff,
        vactive_start=(vact >> 16) & 0x1fff, vactive_end=vact & 0x1fff,
        width=max(0, (hact & 0x1fff) - ((hact >> 16) & 0x1fff)),
        height=max(0, (vact & 0x1fff) - ((vact >> 16) & 0x1fff)))


def decode_interfaces(dsp_if_en):
    enabled = {name: (dsp_if_en >> bit) & 1 for bit, name in enumerate(INTERFACES)}
    muxes = dict(dp0=(dsp_if_en >> 12) & 3, dp1=(dsp_if_en >> 14) & 3,
        hdmi_edp0=(dsp_if_en >> 16) & 3, hdmi_edp1=(dsp_if_en >> 18) & 3)
    return enabled, muxes


def validate(body, expected_samples=3):
    """Return the decoded observation or raise ValidationError."""
    if 'ROCK5_DISPLAY_WRITE_OPEN_REJECTED' not in body:
        raise ValidationError('writable open was not rejected')
    if body.count('ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS') != 1:
        raise ValidationError('resource description did not pass exactly once')
    resources = re.search(r'^ROCK5_DISPLAY_RESOURCES (.*)$', body, re.M)
    if resources is None:
        raise ValidationError('resource line missing')
    fields = dict(item.split('=', 1) for item in resources.group(1).split(' '))
    for key, value in (('vop', '0xfdd90000/0x4200'), ('hdmi', '0xfdea0000/0x20000'),
            ('hdptx', '0xfed70000/0x2000'), ('pmu', '0xfd8d8000/0x400'),
            ('cru', '0xfd7c0000/0x5c000'), ('vop_irq', '188'),
            ('hdmi_irqs', '205,206,207,208,393'), ('vop_pd', '24'), ('hdmi_pd', '26'),
            ('vop_port', '1'), ('board', 'radxa,rock-5-itx')):
        if fields.get(key) != value:
            raise ValidationError('resource %s=%r, expected %r' % (key, fields.get(key), value))
    summary = re.search(r'^ROCK5_DISPLAY_OBSERVATION_PASS samples=(\d+) register_writes=0'
        r' consistent=(\d) vop_read=(\d) hdmi_read=(\d)$', body, re.M)
    if summary is None or int(summary.group(1)) != expected_samples:
        raise ValidationError('observation summary missing or wrong sample count')
    if summary.group(2) != '1':
        raise ValidationError('probe reported inconsistent samples')
    headers = list(re.finditer(r'^ROCK5_DISPLAY_SNAPSHOT sample=(\d+) version=1 flags=(0x[0-9a-f]+)'
        r' start_us=(\d+) end_us=(\d+)$', body, re.M))
    if [int(h.group(1)) for h in headers] != list(range(expected_samples)):
        raise ValidationError('snapshot headers incomplete')
    flags = {int(h.group(2), 16) for h in headers}
    if len(flags) != 1:
        raise ValidationError('snapshot flags differ between samples')
    flags = flags.pop()
    if flags & FLAG_READ_ONLY == 0:
        raise ValidationError('snapshot not marked read-only')
    for start, end in ((int(h.group(3)), int(h.group(4))) for h in headers):
        if end < start or end - start > 5000000:
            raise ValidationError('implausible snapshot duration')
    vop_read = bool(flags & FLAG_VOP_READ)
    hdmi_read = bool(flags & FLAG_HDMI_READ)
    if vop_read == bool(flags & FLAG_VOP_SKIPPED) or hdmi_read == bool(flags & FLAG_HDMI_SKIPPED):
        raise ValidationError('contradictory read/skip flags %#x' % flags)
    if int(summary.group(3)) != vop_read or int(summary.group(4)) != hdmi_read:
        raise ValidationError('summary flags disagree with snapshot flags')

    def collect(label, count, required=True, stable_masks=None):
        samples = _samples(body, label)
        if sorted(samples) != list(range(expected_samples)):
            if required:
                raise ValidationError('%s samples incomplete: %s' % (label, sorted(samples)))
            return None
        decoded = [_words(samples[i], count) for i in range(expected_samples)]
        masks = stable_masks or [0xffffffff] * count
        for sample in decoded[1:]:
            if any((a & m) != (b & m) for a, b, m in zip(sample, decoded[0], masks)):
                raise ValidationError('%s differs between samples' % label)
        return decoded[0]

    pmu = collect('PMU', PMU_COUNT)
    cru_select = collect('CRU_SELECT', CRU_SELECT_COUNT)
    cru_gate = collect('CRU_GATE', CRU_GATE_COUNT)
    # SOC_STATUS1 carries live status in its low half; only the HDMI hot-plug
    # bits (16-31) must agree between samples.
    sys_grf = collect('SYS_GRF', SYS_GRF_COUNT, stable_masks=[0xffffffff, 0xffffffff, 0xffff0000])
    vop_grf = collect('VOP_GRF', 1)
    vo1_grf = collect('VO1_GRF', 2)
    hdptx_grf = collect('HDPTX1_GRF', 2)
    repair = pmu[10]
    power = dict(vop_on=(repair >> 16) & 1, vo0_on=(repair >> 17) & 1, vo1_on=(repair >> 18) & 1,
        power_gate2=pmu[7])
    gates = dict(vop=cru_gate[0] & 0x300, hdmi_pclk=cru_gate[3] & 0x4)
    # The driver's own gating rule, re-derived here from the raw words.
    if vop_read != (power['vop_on'] == 1 and gates['vop'] == 0):
        raise ValidationError('VOP read flag disagrees with power/clock words')
    if hdmi_read != (power['vo1_on'] == 1 and gates['hdmi_pclk'] == 0):
        raise ValidationError('HDMI read flag disagrees with power/clock words')
    status1 = sys_grf[2]
    hpd = dict(hdmi0_level=(status1 >> 19) & 1, hdmi0_int=(status1 >> 16) & 1,
        hdmi1_level=(status1 >> 27) & 1, hdmi1_int=(status1 >> 24) & 1)
    result = dict(status='pass', samples=expected_samples, flags=flags, vop_read=vop_read,
        hdmi_read=hdmi_read, power=power, gates=gates, hpd=hpd,
        pmu=pmu, cru_select=cru_select, cru_gate=cru_gate, sys_grf=sys_grf,
        vop_grf=vop_grf[0], vo1_grf=vo1_grf, hdptx_grf=hdptx_grf,
        hdptx1=dict(pll_lock=(hdptx_grf[1] >> 3) & 1, clock_ready=(hdptx_grf[1] >> 2) & 1,
            phy_ready=(hdptx_grf[1] >> 1) & 1, sideband_ready=hdptx_grf[1] & 1))
    if vop_read:
        vop_sys = collect('VOP_SYS', VOP_SYS_COUNT)
        vop_ovl = collect('VOP_OVL', VOP_OVL_COUNT)
        ports = [collect('VOP_VP%d' % port, VOP_VP_COUNT) for port in range(4)]
        clusters = [collect('VOP_CLUSTER%d' % window, VOP_CLUSTER_COUNT) for window in range(4)]
        esmarts = [collect('VOP_ESMART%d' % window, VOP_ESMART_COUNT) for window in range(4)]
        enabled, muxes = decode_interfaces(vop_sys[4])
        timings = [decode_port(words) for words in ports]
        result.update(vop_version=vop_sys[1], interfaces=enabled, interface_muxes=muxes,
            ports=timings, overlay=dict(control=vop_ovl[0], layer_select=vop_ovl[1],
                port_select=vop_ovl[2]),
            windows=dict(
                clusters=[dict(control=w[0], address=w[2], virtual=w[3], active=w[4],
                    display=w[5], display_start=w[6], cluster_control=w[7]) for w in clusters],
                esmarts=[dict(control=w[0], region_control=w[1], address=w[2], virtual=w[3],
                    active=w[4], display=w[5], display_start=w[6]) for w in esmarts]))
        active_ports = [i for i, t in enumerate(timings) if t['standby'] == 0 and t['width'] > 0]
        result['active_ports'] = active_ports
        for line in re.finditer(r'^ROCK5_DISPLAY_VP_TIMING sample=0 port=(\d) standby=(\d)'
                r' out_mode=(\d+) htotal=(\d+) hsync_end=(\d+) hactive=(\d+)-(\d+) vtotal=(\d+)'
                r' vsync_end=(\d+) vactive=(\d+)-(\d+) width=(\d+) height=(\d+)$', body, re.M):
            port = int(line.group(1))
            expected = timings[port]
            observed = dict(standby=int(line.group(2)), out_mode=int(line.group(3)),
                htotal=int(line.group(4)), hsync_end=int(line.group(5)),
                hactive_start=int(line.group(6)), hactive_end=int(line.group(7)),
                vtotal=int(line.group(8)), vsync_end=int(line.group(9)),
                vactive_start=int(line.group(10)), vactive_end=int(line.group(11)),
                width=int(line.group(12)), height=int(line.group(13)))
            if observed != expected:
                raise ValidationError('probe timing decode disagrees for port %d' % port)
    if hdmi_read:
        hdmi = collect('HDMI1', HDMI_COUNT)
        result['hdmi1'] = dict(global_swdisable=hdmi[0], video_disabled=(hdmi[0] >> 6) & 1,
            i2cm_control0=hdmi[1], i2cm_control1=hdmi[2], audio_config=hdmi[3],
            hdcp2_config=hdmi[4], link_config=hdmi[5], frl=hdmi[5] & 1, dvi=(hdmi[5] >> 4) & 1,
            pktsched_config1=hdmi[6], pktsched_enable=hdmi[7], interrupt_status=hdmi[8],
            interrupt_mask=hdmi[9])
    return result


EDID_HEADER = bytes([0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00])


def decode_edid_base(block):
    """Independent decode of the EDID base block fields the probe reports."""
    if len(block) != 128 or block[:8] != EDID_HEADER:
        raise ValidationError('EDID base block header mismatch')
    if sum(block) & 0xff:
        raise ValidationError('EDID base block checksum failure')
    vendor = block[8] << 8 | block[9]
    manufacturer = ''.join(chr(ord('A') - 1 + ((vendor >> shift) & 0x1f)) for shift in (10, 5, 0))
    dtd = block[54:72]
    return dict(manufacturer=manufacturer, product=block[10] | block[11] << 8,
        serial=block[12] | block[13] << 8 | block[14] << 16 | block[15] << 24,
        week=block[16], year=1990 + block[17], version='%d.%d' % (block[18], block[19]),
        extensions=block[126], digital=block[20] >> 7,
        pixel_khz=(dtd[0] | dtd[1] << 8) * 10,
        width=dtd[2] | (dtd[4] & 0xf0) << 4, hblank=dtd[3] | (dtd[4] & 0x0f) << 8,
        height=dtd[5] | (dtd[7] & 0xf0) << 4, vblank=dtd[6] | (dtd[7] & 0x0f) << 8,
        hsync_offset=dtd[8] | (dtd[11] & 0xc0) << 2, hsync_width=dtd[9] | (dtd[11] & 0x30) << 4,
        vsync_offset=dtd[10] >> 4 | (dtd[11] & 0x0c) << 2, vsync_width=(dtd[10] & 0x0f) | (dtd[11] & 0x03) << 4,
        flags=dtd[17], width_mm=dtd[12] | (dtd[14] & 0xf0) << 4, height_mm=dtd[13] | (dtd[14] & 0x0f) << 8)


def validate_edid(body):
    """Return the decoded EDID from a native --edid transcript or raise ValidationError."""
    if 'ROCK5_DISPLAY_EDID_REQUEST_CHECKS_PASS' not in body:
        raise ValidationError('EDID request boundary checks missing')
    if body.count('ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS') != 1:
        raise ValidationError('resource description did not pass exactly once')
    blocks = {}
    for match in re.finditer(r'^ROCK5_DISPLAY_EDID block=(\d) result=(\d+) flags=(0x[0-9a-f]+) bytes=(\d+)'
            r' polls=(\d+) control=([0-9a-f]{8})/([0-9a-f]{8}) status=([0-9a-f]{8})/([0-9a-f]{8})'
            r' hpd=([0-9a-f]{8}) start_us=(\d+) end_us=(\d+) hex=([0-9a-f]{256})$', body, re.M):
        index = int(match.group(1))
        if index in blocks:
            raise ValidationError('duplicate EDID block %d' % index)
        data = bytes.fromhex(match.group(13))
        if int(match.group(2)) != 0 or int(match.group(4)) != 128:
            raise ValidationError('EDID block %d result %s bytes %s' % (index, match.group(2), match.group(4)))
        if sum(data) & 0xff:
            raise ValidationError('EDID block %d checksum failure' % index)
        control_after, status_after = int(match.group(7), 16), int(match.group(9), 16)
        if control_after & 0x1e or status_after & 0x5:
            raise ValidationError('I2C master left busy after block %d' % index)
        hpd = int(match.group(10), 16)
        if not hpd & (1 << 24):
            raise ValidationError('EDID read without HDMI1 hot-plug level')
        start, end = int(match.group(11)), int(match.group(12))
        if end < start or end - start > 20000000:
            raise ValidationError('implausible EDID block duration')
        flags = int(match.group(3), 16)
        if bool(flags & 1) != (index >= 2) or flags & 2:
            raise ValidationError('unexpected EDID flags %#x for block %d' % (flags, index))
        blocks[index] = dict(data=data, polls=int(match.group(5)), micros=end - start, flags=flags)
    if 0 not in blocks:
        raise ValidationError('EDID base block missing')
    base = decode_edid_base(blocks[0]['data'])
    expected = list(range(1 + min(base['extensions'], 3)))
    if sorted(blocks) != expected:
        raise ValidationError('EDID blocks %s, expected %s' % (sorted(blocks), expected))
    info = re.search(r'^ROCK5_DISPLAY_EDID_INFO (.*)$', body, re.M)
    if info is None:
        raise ValidationError('EDID info line missing')
    fields = dict(item.split('=', 1) for item in info.group(1).split(' '))
    checks = dict(manufacturer=base['manufacturer'], product='%#x' % base['product'],
        serial='%#x' % base['serial'], week=str(base['week']), year=str(base['year']),
        version=base['version'], extensions=str(base['extensions']), digital=str(base['digital']),
        preferred='%dx%d' % (base['width'], base['height']), pixel_khz=str(base['pixel_khz']),
        hblank=str(base['hblank']), vblank=str(base['vblank']), hsync_offset=str(base['hsync_offset']),
        hsync_width=str(base['hsync_width']), vsync_offset=str(base['vsync_offset']),
        vsync_width=str(base['vsync_width']), flags='%#x' % base['flags'],
        size_mm='%dx%d' % (base['width_mm'], base['height_mm']), checksum='ok')
    for key, value in checks.items():
        if fields.get(key) != value:
            raise ValidationError('probe EDID %s=%r, independent decode %r' % (key, fields.get(key), value))
    for index in expected[1:]:
        tag = blocks[index]['data'][0]
        line = 'ROCK5_DISPLAY_EDID_EXTENSION block=%d tag=%#x revision=%d checksum=ok' % (index, tag, blocks[index]['data'][1])
        if body.count(line) != 1:
            raise ValidationError('extension line for block %d missing' % index)
    summary = re.search(r'^ROCK5_DISPLAY_EDID_PASS blocks=(\d) extensions=(\d+) polls=(\d+)'
        r' register_writes=i2c_master_only$', body, re.M)
    if summary is None or int(summary.group(1)) != len(expected) or int(summary.group(2)) != base['extensions']:
        raise ValidationError('EDID summary missing or inconsistent')
    if int(summary.group(3)) != sum(b['polls'] for b in blocks.values()):
        raise ValidationError('EDID poll total inconsistent')
    refresh = None
    if base['pixel_khz'] and base['width'] and base['height']:
        refresh = base['pixel_khz'] * 1000 / ((base['width'] + base['hblank']) * (base['height'] + base['vblank']))
    # String keys keep the result identical across a JSON round trip.
    return dict(status='pass', base=base, blocks={str(index): b['data'].hex() for index, b in blocks.items()},
        polls={str(index): b['polls'] for index, b in blocks.items()},
        micros={str(index): b['micros'] for index, b in blocks.items()}, preferred_refresh_hz=refresh)
