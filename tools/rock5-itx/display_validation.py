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
HDMI_COUNT = 14

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

    def collect(label, count, required=True):
        samples = _samples(body, label)
        if sorted(samples) != list(range(expected_samples)):
            if required:
                raise ValidationError('%s samples incomplete: %s' % (label, sorted(samples)))
            return None
        decoded = [_words(samples[i], count) for i in range(expected_samples)]
        if any(sample != decoded[0] for sample in decoded[1:]):
            raise ValidationError('%s differs between samples' % label)
        return decoded[0]

    pmu = collect('PMU', PMU_COUNT)
    cru_select = collect('CRU_SELECT', CRU_SELECT_COUNT)
    cru_gate = collect('CRU_GATE', CRU_GATE_COUNT)
    sys_grf = collect('SYS_GRF', SYS_GRF_COUNT)
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
        result['hdmi1'] = dict(global_swdisable=hdmi[0], i2cm_status=hdmi[3], mainunit_status=hdmi[5],
            video_config=hdmi[6:9], video_control=hdmi[9], video_status=hdmi[10],
            packing=hdmi[11], monitor_config=hdmi[12], monitor_status=hdmi[13])
    return result
