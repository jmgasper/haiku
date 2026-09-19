"""Decode and check native RK3588 display observation transcripts.

The probe prints raw register words; this module re-derives the video-port
timing, interface routing and hot-plug state independently of the probe's own
decoded lines and requires every sample to agree. The observation part proves
no scanout; it establishes which firmware-programmed state Haiku inherited,
with no register writes. The scanout part checks the opt-in window swap
transcript and, with an image library, whether a captured frame shows the
driver's colour-bar pattern.
"""
import re

PMU_COUNT = 11
CRU_SELECT_COUNT = 4
CRU_GATE_COUNT = 9
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
FLAG_DP_READ = 32
FLAG_DP_SKIPPED = 64
FLAG_DP_AUX_READ = 128
FLAG_GPIO_READ = 256
FLAG_GPIO_SKIPPED = 512
USBDP_GRF_COUNT = 7
VO0_GRF_COUNT = 3
IOC_COUNT = 2
GPIO_COUNT = 6
DP_COUNT = 12
DP_AUX_COUNT = 3

INTERFACES = ['dp0', 'dp1', 'edp0', 'hdmi0', 'edp1', 'hdmi1', 'mipi0', 'mipi1', 'rgb']


class ValidationError(AssertionError):
    pass


def chex(value):
    """Mirror C's %#x, which prints zero without the 0x prefix."""
    return '%#x' % value if value else '0'


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
    rejected = body.count('ROCK5_DISPLAY_WRITE_OPEN_REJECTED\n')
    allowed = body.count('ROCK5_DISPLAY_WRITE_OPEN_ALLOWED\n')
    if rejected + allowed != 1:
        raise ValidationError('writable open outcome missing or ambiguous')
    write_open = 'rejected' if rejected else 'allowed'
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
            ('vop_port', '1'), ('board', 'radxa,rock-5-itx'), ('usbdp', '0xfed90000/0x10000'),
            ('usbdp_grf', '0xfd5cc000/0x4000'), ('vo0_grf', '0xfd5a6000/0x2000'),
            ('ioc', '0xfd5f0000/0x10000'), ('gpio3', '0xfec40000/0x100'), ('dp', '0xfde60000/0x4000'),
            ('dp_pd', '25')):
        if fields.get(key) != value:
            raise ValidationError('resource %s=%r, expected %r' % (key, fields.get(key), value))
    summary = re.search(r'^ROCK5_DISPLAY_OBSERVATION_PASS samples=(\d+) register_writes=0'
        r' consistent=(\d) vop_read=(\d) hdmi_read=(\d) dp_read=(\d) gpio_read=(\d)$', body, re.M)
    if summary is None or int(summary.group(1)) != expected_samples:
        raise ValidationError('observation summary missing or wrong sample count')
    if summary.group(2) != '1':
        raise ValidationError('probe reported inconsistent samples')
    headers = list(re.finditer(r'^ROCK5_DISPLAY_SNAPSHOT sample=(\d+) version=2 flags=(0x[0-9a-f]+|0)'
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
    dp_read = bool(flags & FLAG_DP_READ)
    dp_aux_read = bool(flags & FLAG_DP_AUX_READ)
    gpio_read = bool(flags & FLAG_GPIO_READ)
    if dp_read == bool(flags & FLAG_DP_SKIPPED) or gpio_read == bool(flags & FLAG_GPIO_SKIPPED):
        raise ValidationError('contradictory DisplayPort/GPIO read/skip flags %#x' % flags)
    if dp_aux_read and not dp_read:
        raise ValidationError('AUX words flagged read without the DisplayPort block')
    if int(summary.group(3)) != vop_read or int(summary.group(4)) != hdmi_read:
        raise ValidationError('summary flags disagree with snapshot flags')
    if int(summary.group(5)) != dp_read or int(summary.group(6)) != gpio_read:
        raise ValidationError('summary DisplayPort/GPIO flags disagree with snapshot flags')

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
    usbdp_grf = collect('USBDP1_GRF', USBDP_GRF_COUNT)
    vo0_grf = collect('VO0_GRF', VO0_GRF_COUNT)
    ioc = collect('IOC', IOC_COUNT)
    repair = pmu[10]
    power = dict(vop_on=(repair >> 16) & 1, vo0_on=(repair >> 17) & 1, vo1_on=(repair >> 18) & 1,
        power_gate2=pmu[7])
    gates = dict(vop=cru_gate[0] & 0x300, hdmi_pclk=cru_gate[3] & 0x4,
        pclk_dp1=(cru_gate[8] >> 5) & 1, dp_aux=(cru_gate[8] >> 3) & 1, dp_hdcp=(cru_gate[8] >> 9) & 1,
        usbdp_pclk=(cru_gate[4] >> 4) & 1, usbdp_immortal=(cru_gate[6] >> 15) & 1, gpio3=(cru_gate[7] >> 2) & 1)
    # The driver's DisplayPort and GPIO gating, re-derived from the raw words.
    if dp_read != (power['vo0_on'] == 1 and gates['pclk_dp1'] == 0):
        raise ValidationError('DisplayPort read flag disagrees with power/clock words')
    if dp_aux_read != (dp_read and gates['dp_aux'] == 0):
        raise ValidationError('DisplayPort AUX read flag disagrees with the AUX clock gate')
    if gpio_read != (gates['gpio3'] == 0):
        raise ValidationError('GPIO read flag disagrees with the GPIO3 clock gate')
    # The driver's own gating rule, re-derived here from the raw words. The
    # HDMI TX block is unreachable while the HDPTX PHY is powered down (DPMS
    # off), which the GRF shows as the PLL-enable bit (CON0 bit 7) clear.
    phy_pll = (hdptx_grf[0] >> 7) & 1
    if vop_read != (power['vop_on'] == 1 and gates['vop'] == 0):
        raise ValidationError('VOP read flag disagrees with power/clock words')
    if hdmi_read != (power['vo1_on'] == 1 and gates['hdmi_pclk'] == 0 and phy_pll == 1):
        raise ValidationError('HDMI read flag disagrees with power/clock/PLL words')
    status1 = sys_grf[2]
    hpd = dict(hdmi0_level=(status1 >> 19) & 1, hdmi0_int=(status1 >> 16) & 1,
        hdmi1_level=(status1 >> 27) & 1, hdmi1_int=(status1 >> 24) & 1)
    result = dict(status='pass', samples=expected_samples, flags=flags, vop_read=vop_read,
        hdmi_read=hdmi_read, dp_read=dp_read, dp_aux_read=dp_aux_read, gpio_read=gpio_read,
        power=power, gates=gates, hpd=hpd,
        pmu=pmu, cru_select=cru_select, cru_gate=cru_gate, sys_grf=sys_grf,
        vop_grf=vop_grf[0], vo1_grf=vo1_grf, hdptx_grf=hdptx_grf,
        hdptx1=dict(pll_lock=(hdptx_grf[1] >> 3) & 1, clock_ready=(hdptx_grf[1] >> 2) & 1,
            phy_ready=(hdptx_grf[1] >> 1) & 1, sideband_ready=hdptx_grf[1] & 1),
        usbdp_grf=usbdp_grf, vo0_grf=vo0_grf, ioc=ioc,
        # GPIO3_D5 (dp1_hpdin_m0 is mux function 5) carries the second connector's hot-plug.
        dp_pin=dict(mux=(ioc[1] >> 4) & 0xf, lane_mux_word=vo0_grf[0]))
    if gpio_read:
        gpio = collect('GPIO3', GPIO_COUNT, stable_masks=[0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0, 0xffffffff])
        result['gpio3'] = dict(data=gpio[0:2], direction=gpio[2:4], external_port=gpio[4], version=gpio[5])
        result['dp_pin'].update(level=(gpio[4] >> 29) & 1, output=(gpio[3] >> 13) & 1)
    if dp_read:
        dp = collect('DP1', DP_COUNT)
        result['dp1'] = dict(version=dp[0], version_type=dp[1], id=dp[2], config=dp[3:6], cctl=dp[6],
            soft_reset=dp[7], vsample_control=dp[8], video_config1=dp[9], phyif_control=dp[10],
            phyif_powerdown=dp[11])
        if dp_aux_read:
            aux = collect('DP1_AUX', DP_AUX_COUNT, stable_masks=[0, 0, 0xffffffff])
            # DW DP HPD_STATUS: bit 8 is the current level, bits 11:9 the state machine.
            result['dp1'].update(aux_status=aux[0], general_interrupt=aux[1], hpd_status=aux[2],
                hpd_level=(aux[2] >> 8) & 1, hpd_state=(aux[2] >> 9) & 7)
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
    result['write_open'] = write_open
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
    for match in re.finditer(r'^ROCK5_DISPLAY_EDID block=(\d) result=(\d+) flags=(0x[0-9a-f]+|0) bytes=(\d+)'
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
    checks = dict(manufacturer=base['manufacturer'], product=chex(base['product']),
        serial=chex(base['serial']), week=str(base['week']), year=str(base['year']),
        version=base['version'], extensions=str(base['extensions']), digital=str(base['digital']),
        preferred='%dx%d' % (base['width'], base['height']), pixel_khz=str(base['pixel_khz']),
        hblank=str(base['hblank']), vblank=str(base['vblank']), hsync_offset=str(base['hsync_offset']),
        hsync_width=str(base['hsync_width']), vsync_offset=str(base['vsync_offset']),
        vsync_width=str(base['vsync_width']), flags=chex(base['flags']),
        size_mm='%dx%d' % (base['width_mm'], base['height_mm']), checksum='ok')
    for key, value in checks.items():
        if fields.get(key) != value:
            raise ValidationError('probe EDID %s=%r, independent decode %r' % (key, fields.get(key), value))
    for index in expected[1:]:
        tag = blocks[index]['data'][0]
        line = 'ROCK5_DISPLAY_EDID_EXTENSION block=%d tag=%s revision=%d checksum=ok' % (index, chex(tag), blocks[index]['data'][1])
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


SCANOUT_LINE = re.compile(
    r'^ROCK5_DISPLAY_SCANOUT action=(query|show|restore) result=(\d+) flags=(\d+) port=(\d) window=(\d)'
    r' before=([0-9a-f]{8}) after=([0-9a-f]{8}) firmware=([0-9a-f]{8}) pattern=([0-9a-f]{8})'
    r' region_control=([0-9a-f]{8}) virtual=(\d+) active=([0-9a-f]{8}) display=([0-9a-f]{8})'
    r' start=([0-9a-f]{8}) if_en=([0-9a-f]{8}) cfg_done=([0-9a-f]{8}) polls=(\d+) start_us=(\d+) end_us=(\d+)$', re.M)
SCANOUT_STEPS = [('query', 0), ('show', 1), ('query', 1), ('restore', 0), ('query', 0)]
SCANOUT_GEOMETRY = dict(region_control=1, virtual=1920, active=0x0437077f, display=0x0437077f, start=0)
# 0xAARRGGBB bar colours of DisplayScanout.h as RGB, then the border grey.
PATTERN_COLORS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255),
    (255, 0, 0), (0, 0, 255), (0, 0, 0)]
PATTERN_BORDER = (64, 64, 64)
PATTERN_BAR_WIDTH = (1920 - 64) // 8


def validate_scanout(body, observation=None):
    """Return the decoded scanout swap from a native --scanout transcript or raise ValidationError.

    `observation` is a decoded observation of the same boot; the swap must
    have used the window and port it reported and the firmware address it saw.
    """
    if 'ROCK5_DISPLAY_SCANOUT_REQUEST_CHECKS_PASS' not in body:
        raise ValidationError('scanout request boundary checks missing')
    if body.count('ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS') != 1:
        raise ValidationError('resource description did not pass exactly once')
    if 'ROCK5_DISPLAY_SCANOUT_ABORT' in body:
        raise ValidationError('scanout probe aborted')
    steps = []
    for match in SCANOUT_LINE.finditer(body):
        start, end = int(match.group(18)), int(match.group(19))
        steps.append(dict(action=match.group(1), result=int(match.group(2)), flags=int(match.group(3)),
            port=int(match.group(4)), window=int(match.group(5)), before=int(match.group(6), 16),
            after=int(match.group(7), 16), firmware=int(match.group(8), 16), pattern=int(match.group(9), 16),
            region_control=int(match.group(10), 16), virtual=int(match.group(11)),
            active=int(match.group(12), 16), display=int(match.group(13), 16), start=int(match.group(14), 16),
            if_en=int(match.group(15), 16), cfg_done=int(match.group(16), 16), polls=int(match.group(17)),
            micros=end - start, offset=match.start()))
    if [(s['action'], s['flags']) for s in steps] != SCANOUT_STEPS:
        raise ValidationError('scanout steps %r' % [(s['action'], s['flags']) for s in steps])
    for step in steps:
        if step['result'] != 0:
            raise ValidationError('scanout %s result %d' % (step['action'], step['result']))
        if step['micros'] < 0 or step['micros'] > 5000000:
            raise ValidationError('implausible scanout %s duration' % step['action'])
        if step['polls'] > 5000 or (step['action'] == 'query' and step['polls']):
            raise ValidationError('implausible scanout %s poll count %d' % (step['action'], step['polls']))
        for key, value in SCANOUT_GEOMETRY.items():
            if step[key] != value:
                raise ValidationError('window %s %#x differs from the qualified firmware state' % (key, step[key]))
    first, show, held, restore, last = steps
    port, window = first['port'], first['window']
    if port > 3 or window > 3:
        raise ValidationError('scanout port %d window %d out of range' % (port, window))
    if not first['if_en'] >> 5 & 1 or first['if_en'] >> 18 & 3 != port:
        raise ValidationError('interface enable %#x does not route HDMI1 from port %d' % (first['if_en'], port))
    for step in steps:
        if step['port'] != port or step['window'] != window or step['if_en'] != first['if_en']:
            raise ValidationError('scanout routing changed during the swap')
    firmware, pattern = first['before'], show['pattern']
    if not firmware or not pattern or pattern == firmware or pattern & 0xfff or pattern >= 1 << 32:
        raise ValidationError('implausible addresses firmware %#x pattern %#x' % (firmware, pattern))
    commit = 0x8000 | 1 << port | 1 << (port + 16)
    expected = [
        dict(after=0, cfg_done=0),
        dict(before=firmware, after=pattern, firmware=firmware, pattern=pattern, cfg_done=commit),
        dict(before=pattern, after=0, firmware=firmware, pattern=pattern, cfg_done=0),
        dict(before=pattern, after=firmware, firmware=firmware, pattern=pattern, cfg_done=commit),
        dict(before=firmware, after=0, firmware=firmware, pattern=pattern, cfg_done=0),
    ]
    for step, checks in zip(steps, expected):
        for key, value in checks.items():
            if step[key] != value:
                raise ValidationError('scanout %s %s=%#x, expected %#x' % (step['action'], key, step[key], value))
    hold = re.search(r'^ROCK5_DISPLAY_SCANOUT_HOLD seconds=(\d+)$', body, re.M)
    if hold is None or not show['offset'] < hold.start() < held['offset']:
        raise ValidationError('scanout hold marker missing or out of order')
    summary = ('ROCK5_DISPLAY_SCANOUT_PASS port=%d window=%d firmware=%08x pattern=%08x hold_seconds=%s'
        ' register_writes=window_address_and_cfg_done' % (port, window, firmware, pattern, hold.group(1)))
    if body.count(summary + '\n') != 1 or body.rfind(summary) < last['offset']:
        raise ValidationError('scanout summary missing or inconsistent')
    if observation is not None:
        if observation.get('active_ports') != [port]:
            raise ValidationError('observation active ports %r, swap used port %d' % (observation.get('active_ports'), port))
        seen = observation['windows']['esmarts'][window]
        if seen['region_control'] != 1 or seen['address'] != firmware:
            raise ValidationError('observation window %d address %#x, swap saw %#x' % (window, seen['address'], firmware))
    names = ['query_before', 'show', 'query_swapped', 'restore', 'query_after']
    return dict(status='pass', port=port, window=window, firmware='%08x' % firmware, pattern='%08x' % pattern,
        hold_seconds=int(hold.group(1)), commit='%08x' % commit,
        micros={name: step['micros'] for name, step in zip(names, steps)},
        polls={name: step['polls'] for name, step in zip(names, steps)})


def _nearest_pattern_color(rgb):
    candidates = PATTERN_COLORS + [PATTERN_BORDER]
    return min(range(len(candidates)), key=lambda i: sum((a - b) ** 2 for a, b in zip(candidates[i], rgb)))


def check_pattern_frame(path, expect_pattern=True):
    """Classify a 1920x1080 capture: raise unless it shows (or, with expect_pattern False, does not show) the bars."""
    from PIL import Image
    image = Image.open(path).convert('RGB')
    if image.size != (1920, 1080):
        raise ValidationError('frame is %dx%d, not 1920x1080' % image.size)
    samples = []
    for bar in range(8):
        x = 32 + bar * PATTERN_BAR_WIDTH + PATTERN_BAR_WIDTH // 2
        for y in (200, 540, 880):
            rgb = image.getpixel((x, y))
            samples.append(dict(x=x, y=y, rgb=list(rgb), expected=bar, nearest=_nearest_pattern_color(rgb)))
    for x, y in ((16, 540), (1903, 540), (960, 16), (960, 1063)):
        rgb = image.getpixel((x, y))
        samples.append(dict(x=x, y=y, rgb=list(rgb), expected=8, nearest=_nearest_pattern_color(rgb)))
    mismatches = sum(1 for s in samples if s['nearest'] != s['expected'])
    bright = all(min(s['rgb']) >= 160 for s in samples if s['expected'] == 0)
    dark = all(max(s['rgb']) <= 90 for s in samples if s['expected'] == 7)
    visible = mismatches == 0 and bright and dark
    if expect_pattern and not visible:
        raise ValidationError('frame does not show the scanout pattern (%d of %d samples off, bright=%s, dark=%s)'
            % (mismatches, len(samples), bright, dark))
    if not expect_pattern and visible:
        raise ValidationError('frame still shows the scanout pattern')
    return dict(status='pass', pattern_visible=visible, mismatches=mismatches, samples=samples)


ACCELERANT_LINE = re.compile(
    r'^ROCK5_DISPLAY_ACCELERANT flags=(\d+) shared_area=(-?\d+) framebuffer=([0-9a-f]{8})'
    r' firmware=([0-9a-f]{8}) port=(\d) window=(\d) polls=(\d+) width=(\d+) height=(\d+)'
    r' bytes_per_row=(\d+)(?: retrace_sem=(-?\d+) retraces=(\d+))?'
    r'(?: calls=(\d+) spurious=(\d+) first_us=(\d+) last_us=(\d+) now_us=(\d+))?$', re.M)
RETRACE_LINE = re.compile(
    r'^ROCK5_DISPLAY_RETRACE waits=(\d+) timeouts=(\d+) first_us=(\d+) last_us=(\d+) period_us=(\d+)'
    r' retraces_before=(\d+) retraces_after=(\d+) elapsed_us=(\d+)(?: errors=(\d+) status=(.+?))?$', re.M)
FRAME_PERIOD_US = (15500, 18000) # 60 Hz nominal, 16667 us
ACCELERANT_SHARED = re.compile(
    r'^ROCK5_DISPLAY_ACCELERANT_SHARED version=(\d+) flags=(\d+) mode_list_area=(-?\d+) modes=(\d+)'
    r' size=(\d+)x(\d+) bytes_per_row=(\d+) pixel_khz=(\d+) h=(\d+)/(\d+)/(\d+) v=(\d+)/(\d+)/(\d+)'
    r' port_timing=([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}) edid_result=(\d+)'
    r'(?: power=(\d+))? name=(.+?) edid=([0-9a-f]{256})$', re.M)
ACCELERANT_CLONE = re.compile(
    r'^ROCK5_DISPLAY_ACCELERANT_CLONE area=(\d+) size=(\d+) samples=([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8})$', re.M)
FIRMWARE_TIMING = dict(h=(2008, 2052, 2200), v=(1084, 1089, 1125), pixel_khz=148500,
    port_timing=('0898002c', '00c00840', '04650005', '00290461'))


def validate_accelerant(body, observation=None, edid_block0=None, mode=(1920, 1080)):
    """Return the decoded accelerant state from a native --accelerant transcript or raise ValidationError.

    `observation` is a decoded observation of the same boot taken while the
    accelerant owned the frame buffer: its live window must scan the
    accelerant's buffer. `edid_block0` is the base block read by the EDID
    inventory, which the shared information must repeat. `mode` is the
    (width, height) the accelerant must report with its CEA timing: the
    firmware mode, or after a mode change the new mode at the unchanged
    1920-pixel row pitch of the frame buffer.
    """
    geometry = (mode[0], mode[1], 7680)
    expected_timing = shared_timing(*mode)
    if 'ROCK5_DISPLAY_WRITE_OPEN_ALLOWED\n' not in body:
        raise ValidationError('writable open was not admitted')
    if 'ROCK5_DISPLAY_ACCELERANT_REQUEST_CHECKS_PASS' not in body:
        raise ValidationError('accelerant request boundary checks missing')
    if 'ROCK5_DISPLAY_ACCELERANT_NOT_ACQUIRED' in body:
        raise ValidationError('no accelerant owned the frame buffer')
    line = ACCELERANT_LINE.search(body)
    if line is None:
        raise ValidationError('accelerant line missing')
    flags = int(line.group(1))
    framebuffer, firmware = int(line.group(3), 16), int(line.group(4), 16)
    port, window, polls = int(line.group(5)), int(line.group(6)), int(line.group(7))
    width, height, bytes_per_row = int(line.group(8)), int(line.group(9)), int(line.group(10))
    if flags & 1 == 0:
        raise ValidationError('frame buffer not acquired')
    if not framebuffer or framebuffer == firmware or framebuffer & 0xfff or framebuffer >= 1 << 32:
        raise ValidationError('implausible frame buffer %#x (firmware %#x)' % (framebuffer, firmware))
    if (width, height, bytes_per_row) != tuple(geometry):
        raise ValidationError('frame buffer geometry %dx%d/%d, expected %dx%d/%d'
            % ((width, height, bytes_per_row) + tuple(geometry)))
    if polls > 5000:
        raise ValidationError('implausible acquisition poll count %d' % polls)
    # Probes before the retrace stage print no semaphore or count.
    retrace_sem = int(line.group(11)) if line.group(11) is not None else -1
    retraces_before = int(line.group(12)) if line.group(12) is not None else 0
    retrace = None
    if flags & 4:
        if retrace_sem < 0:
            raise ValidationError('retrace flagged without a semaphore')
        measured = RETRACE_LINE.search(body)
        if measured is None:
            raise ValidationError('retrace measurement missing')
        waits, timeouts = int(measured.group(1)), int(measured.group(2))
        period = int(measured.group(5))
        before, after, elapsed = int(measured.group(6)), int(measured.group(7)), int(measured.group(8))
        errors = int(measured.group(9)) if measured.group(9) is not None else 0
        if timeouts or errors or waits < 8:
            raise ValidationError('retrace waits %d timeouts %d errors %d (%s)' % (waits, timeouts, errors, measured.group(10)))
        if not FRAME_PERIOD_US[0] <= period <= FRAME_PERIOD_US[1]:
            raise ValidationError('retrace period %d us is not a 60 Hz frame' % period)
        # Interrupts keep counting whether or not anyone waits: the count must
        # have grown by about the elapsed frames, and never less than the waits.
        frames = elapsed / 16667.0
        if after - before < waits or after - before > frames * 1.25 + 4:
            raise ValidationError('retrace count grew by %d over %.1f frames' % (after - before, frames))
        if before != retraces_before:
            raise ValidationError('retrace count differs between the description and the measurement')
        retrace = dict(waits=waits, period_us=period, count_delta=after - before, elapsed_us=elapsed)
        if line.group(13) is not None:
            retrace.update(calls=int(line.group(13)), spurious=int(line.group(14)),
                first_us=int(line.group(15)), last_us=int(line.group(16)), now_us=int(line.group(17)))
    elif retrace_sem >= 0 or RETRACE_LINE.search(body):
        raise ValidationError('retrace data without the retrace flag')
    signature = re.search(r'^ROCK5_DISPLAY_ACCELERANT_SIGNATURE (.*)$', body, re.M)
    if signature is None or signature.group(1) != 'rk3588_display.accelerant':
        raise ValidationError('accelerant signature missing or wrong')
    device = re.search(r'^ROCK5_DISPLAY_ACCELERANT_DEVICE (.*)$', body, re.M)
    if device is None or device.group(1) != 'graphics/rk3588_display/0':
        raise ValidationError('accelerant device name missing or wrong')
    shared = ACCELERANT_SHARED.search(body)
    if shared is None:
        raise ValidationError('shared information line missing')
    if int(shared.group(1)) != 1 or int(shared.group(2)) != (flags & 2):
        raise ValidationError('shared information version or flags differ from the driver description')
    if int(shared.group(3)) < 0 or int(shared.group(4)) < 1:
        raise ValidationError('the primary accelerant published no mode list')
    if (int(shared.group(5)), int(shared.group(6)), int(shared.group(7))) != (width, height, bytes_per_row):
        raise ValidationError('shared geometry differs from the driver description')
    timing = dict(h=tuple(int(shared.group(i)) for i in (9, 10, 11)), v=tuple(int(shared.group(i)) for i in (12, 13, 14)),
        pixel_khz=int(shared.group(8)), port_timing=tuple(shared.group(i) for i in (15, 16, 17, 18)))
    if timing != expected_timing:
        raise ValidationError('decoded timing %r differs from the %dx%d mode' % ((timing,) + tuple(mode)))
    edid_result = int(shared.group(19))
    edid = shared.group(22)
    power = int(shared.group(20)) if shared.group(20) is not None else None
    if power not in (None, 0):
        raise ValidationError('accelerant probe ran with the port powered off (%d)' % power)
    if bool(flags & 2) != (edid_result == 0):
        raise ValidationError('EDID flag and result disagree')
    if edid_block0 is not None and (not flags & 2 or edid != edid_block0):
        raise ValidationError('shared EDID block differs from the EDID inventory')
    # The clone maps the whole frame buffer, which keeps its firmware-mode
    # size (1920x1080x4) across mode changes.
    clone = ACCELERANT_CLONE.search(body)
    if clone is None or int(clone.group(2)) != 1920 * 1080 * 4:
        raise ValidationError('frame buffer clone missing or wrong size')
    if 'ROCK5_DISPLAY_ACCELERANT_ACQUIRE_BUSY\n' not in body:
        raise ValidationError('a second acquisition was not refused')
    summary = ('ROCK5_DISPLAY_ACCELERANT_PASS acquired=1 edid=%d framebuffer=%08x firmware=%08x'
        ' register_writes=owner_only' % (1 if flags & 2 else 0, framebuffer, firmware))
    if body.count(summary + '\n') != 1:
        raise ValidationError('accelerant summary missing or inconsistent')
    if observation is not None:
        if observation.get('active_ports') != [port]:
            raise ValidationError('observation active ports %r, accelerant port %d' % (observation.get('active_ports'), port))
        seen = observation['windows']['esmarts'][window]
        if seen['region_control'] != 1 or seen['address'] != framebuffer:
            raise ValidationError('observation window %d scans %#x, accelerant buffer is %#x' % (window, seen['address'], framebuffer))
    return dict(status='pass', flags=flags, framebuffer='%08x' % framebuffer, firmware='%08x' % firmware,
        port=port, window=window, polls=polls, modes=int(shared.group(4)), edid_result=edid_result,
        name=shared.group(21), samples=[clone.group(i) for i in (3, 4, 5, 6)], retrace=retrace,
        power=power)


POWER_LINE = re.compile(
    r'^ROCK5_DISPLAY_POWER mode=(off|on) result=(\d+) phase=(\d+) hold_polls=(\d+) clock_polls=(\d+)'
    r' lock_polls=(\d+) phy_status=([0-9a-f]{8}) control=([0-9a-f]{8}) previous=(\d+) micros=(\d+)$', re.M)
POWER_ACCELERANT = re.compile(
    r'^ROCK5_DISPLAY_POWER_ACCELERANT width=(\d+) height=(\d+) flags=(\d+) retraces_before=(\d+)'
    r' retraces_after=(\d+) shared_power=(\d+)$', re.M)


def validate_power(body, mode, previous=None):
    """Return the decoded power change from a native --power transcript or raise ValidationError.

    Off leaves the port in standby with no frame starts over the half second
    the probe waits; on brings the full mode set back (phase 6, PLL locked,
    frames starting again at 60 Hz).
    """
    if mode not in ('off', 'on'):
        raise ValueError(mode)
    if 'ROCK5_DISPLAY_POWER_REQUEST_CHECKS_PASS' not in body:
        raise ValidationError('power request boundary checks missing')
    line = POWER_LINE.search(body)
    if line is None:
        raise ValidationError('power line missing')
    if line.group(1) != mode:
        raise ValidationError('power line is for %s, not %s' % (line.group(1), mode))
    result, phase = int(line.group(2)), int(line.group(3))
    if result != 0:
        raise ValidationError('power change result %d at phase %d' % (result, phase))
    hold, clock, lock = int(line.group(4)), int(line.group(5)), int(line.group(6))
    phy_status, control = int(line.group(7), 16), int(line.group(8), 16)
    before = int(line.group(9))
    if previous is not None and before != previous:
        raise ValidationError('previous power mode %d, expected %d' % (before, previous))
    micros = int(line.group(10))
    accelerant = POWER_ACCELERANT.search(body)
    if accelerant is None:
        raise ValidationError('accelerant state after the power change missing')
    width, height, flags = int(accelerant.group(1)), int(accelerant.group(2)), int(accelerant.group(3))
    retraces_before, retraces_after = int(accelerant.group(4)), int(accelerant.group(5))
    shared_power = int(accelerant.group(6))
    if flags & 9 != 9:
        raise ValidationError('accelerant flags %d lack acquisition or mode control' % flags)
    if mode == 'off':
        if phase != 2 or control & 0x80000000 == 0:
            raise ValidationError('port not in standby after power-off (phase %d, control %#x)' % (phase, control))
        if hold < 1 or hold > 60 or clock or lock:
            raise ValidationError('power-off polls hold %d clock %d lock %d' % (hold, clock, lock))
        if retraces_after != retraces_before:
            raise ValidationError('frames still start while off (%d -> %d)' % (retraces_before, retraces_after))
        if shared_power != 1:
            raise ValidationError('shared power mode %d after power-off' % shared_power)
    else:
        if phase != 6 or control & 0x80000000 or control & 0xf != 0xf:
            raise ValidationError('port not running after power-on (phase %d, control %#x)' % (phase, control))
        if phy_status != 0xe:
            raise ValidationError('PHY status %#x after power-on' % phy_status)
        if hold or clock > 100 or lock > 50:
            raise ValidationError('power-on polls hold %d clock %d lock %d' % (hold, clock, lock))
        grown = retraces_after - retraces_before
        if grown < 20 or grown > 40:
            raise ValidationError('frame starts grew by %d in half a second after power-on' % grown)
        if shared_power != 0:
            raise ValidationError('shared power mode %d after power-on' % shared_power)
    if micros > 2000000:
        raise ValidationError('power change took %d us' % micros)
    if body.count('ROCK5_DISPLAY_POWER_PASS mode=%s\n' % mode) != 1:
        raise ValidationError('power summary missing or inconsistent')
    return dict(status='pass', mode=mode, phase=phase, hold_polls=hold, clock_polls=clock, lock_polls=lock,
        phy_status='%08x' % phy_status, control='%08x' % control, previous=before, micros=micros,
        width=width, height=height, flags=flags, retraces_before=retraces_before,
        retraces_after=retraces_after, shared_power=shared_power)


# The Haiku desktop as the NanoKVM captures it: the default blue workspace
# and the light Deskbar in the top-right corner.
DESKTOP_BLUE = (63, 105, 145)


def check_desktop_frame(path):
    """Raise unless a 1920x1080 capture shows the plain Haiku desktop with its Deskbar."""
    from PIL import Image
    image = Image.open(path).convert('RGB')
    if image.size != (1920, 1080):
        raise ValidationError('frame is %dx%d, not 1920x1080' % image.size)
    samples = []
    for x, y in ((400, 400), (960, 700), (1500, 300), (100, 900), (1300, 950)):
        rgb = image.getpixel((x, y))
        samples.append(dict(x=x, y=y, rgb=list(rgb), kind='workspace',
            ok=all(abs(a - b) <= 24 for a, b in zip(rgb, DESKTOP_BLUE))))
    for x, y in ((1850, 37), (1800, 10)):
        rgb = image.getpixel((x, y))
        samples.append(dict(x=x, y=y, rgb=list(rgb), kind='deskbar',
            ok=min(rgb) >= 150 and max(rgb) - min(rgb) <= 24))
    failed = [s for s in samples if not s['ok']]
    if failed:
        raise ValidationError('frame does not show the Haiku desktop: %r' % failed)
    return dict(status='pass', samples=samples)


MODE_LINE = re.compile(
    r'^ROCK5_DISPLAY_MODE width=(\d+) height=(\d+) clock=(\d+) vic=(\d+) result=(\d+) phase=(\d+)'
    r' hold_polls=(\d+) clock_polls=(\d+) lock_polls=(\d+) phy_status=([0-9a-f]{8})'
    r' timing=([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}) if_en=([0-9a-f]{8}) micros=(\d+)$', re.M)
MODE_ACCELERANT = re.compile(r'^ROCK5_DISPLAY_MODE_ACCELERANT width=(\d+) height=(\d+) flags=(\d+) retraces=(\d+)$', re.M)
# CEA-861 timings the probe requests: (clock kHz, hss, hse, htotal, vss, vse, vtotal, vic).
CEA_TIMINGS = {(1920, 1080): (148500, 2008, 2052, 2200, 1084, 1089, 1125, 16),
    (1280, 720): (74250, 1390, 1430, 1650, 725, 730, 750, 4),
    (720, 480): (27000, 736, 798, 858, 489, 495, 525, 2),
    (640, 480): (25175, 656, 752, 800, 490, 492, 525, 1)}


def shared_timing(width, height):
    """Return the shared-information timing the driver publishes for a CEA mode.

    The VOP2 port words hold the total and sync width in the high and low
    halves, then the active start and end measured from the sync; the driver
    decodes them into Accelerant.h sync positions (DecodePortTiming), and
    this is that decode run backwards from the CEA table.
    """
    timing = CEA_TIMINGS.get((width, height))
    if timing is None:
        raise ValidationError('no CEA timing for %dx%d' % (width, height))
    clock, hss, hse, htotal, vss, vse, vtotal, _ = timing
    hend, vend = htotal - (hss - width), vtotal - (vss - height)
    words = (htotal << 16 | (hse - hss), (hend - width) << 16 | hend,
        vtotal << 16 | (vse - vss), (vend - height) << 16 | vend)
    return dict(h=(hss, hse, htotal), v=(vss, vse, vtotal), pixel_khz=clock,
        port_timing=tuple('%08x' % word for word in words))


def validate_modeset(body, width, height):
    """Return the decoded mode change from a native --mode transcript or raise ValidationError."""
    if 'ROCK5_DISPLAY_MODE_REQUEST_CHECKS_PASS' not in body:
        raise ValidationError('mode request boundary checks missing')
    line = MODE_LINE.search(body)
    if line is None:
        raise ValidationError('mode line missing')
    if (int(line.group(1)), int(line.group(2))) != (width, height):
        raise ValidationError('mode line is for %sx%s, expected %dx%d' % (line.group(1), line.group(2), width, height))
    timing = CEA_TIMINGS.get((width, height))
    if timing is None:
        raise ValidationError('no CEA timing for %dx%d' % (width, height))
    clock, hss, hse, htotal, vss, vse, vtotal, vic = timing
    if int(line.group(3)) != clock or int(line.group(4)) != vic:
        raise ValidationError('mode clock/VIC %s/%s differ from CEA' % (line.group(3), line.group(4)))
    result, phase = int(line.group(5)), int(line.group(6))
    if result != 0 or phase != 6:
        raise ValidationError('mode change result %d at phase %d' % (result, phase))
    polls = dict(hold=int(line.group(7)), clock=int(line.group(8)), lock=int(line.group(9)))
    if polls['hold'] > 60 or polls['clock'] > 100 or polls['lock'] > 50:
        raise ValidationError('implausible poll counts %r' % polls)
    phy_status = int(line.group(10), 16)
    if phy_status & 0xe != 0xe:
        raise ValidationError('PHY status %#x lacks ready/lock bits' % phy_status)
    words = [int(line.group(i), 16) for i in (11, 12, 13, 14)]
    expected = [(htotal << 16) | (hse - hss), ((htotal - hss) << 16) | (htotal - hss + width),
        (vtotal << 16) | (vse - vss), ((vtotal - vss) << 16) | (vtotal - vss + height)]
    if words != expected:
        raise ValidationError('port timing %s, expected %s' % (['%08x' % w for w in words], ['%08x' % w for w in expected]))
    if int(line.group(15), 16) != 0x00080020:
        raise ValidationError('interface enable changed to %s' % line.group(15))
    micros = int(line.group(16))
    if micros <= 0 or micros > 2000000:
        raise ValidationError('implausible mode change duration %d us' % micros)
    accelerant = MODE_ACCELERANT.search(body)
    if accelerant is None or (int(accelerant.group(1)), int(accelerant.group(2))) != (width, height):
        raise ValidationError('accelerant description does not report the new mode')
    if int(accelerant.group(3)) & 0x9 != 0x9:
        raise ValidationError('accelerant flags %s lack acquired/modeset' % accelerant.group(3))
    summary = 'ROCK5_DISPLAY_MODE_PASS width=%d height=%d clock=%d vic=%d' % (width, height, clock, vic)
    if body.count(summary + '\n') != 1:
        raise ValidationError('mode summary missing')
    return dict(status='pass', width=width, height=height, clock=clock, vic=vic, polls=polls,
        phy_status='%08x' % phy_status, timing=['%08x' % w for w in words], micros=micros,
        retraces=int(accelerant.group(4)))


def check_desktop_crop(path, width, height, capture=(1920, 1080)):
    """Raise unless the frame shows the top-left width x height crop of the Haiku workspace.

    A frame of the crop's own size is checked directly. The NanoKVM scales
    every input mode to its 1920x1080 capture, so a 1280x720 mode arrives
    as that crop enlarged by 1.5: the icons sit lower and further right,
    and the Deskbar, which app_server draws beyond the crop, is absent from
    the top right. A scaled frame must show both.
    """
    from PIL import Image
    image = Image.open(path).convert('RGB')
    if image.size == (width, height):
        scale = 1.0
    elif image.size == tuple(capture) and capture[0] * height == capture[1] * width and width < capture[0]:
        scale = capture[0] / float(width)
    else:
        raise ValidationError('frame is %dx%d, not %dx%d or its scaled %dx%d capture'
            % (image.size + (width, height) + tuple(capture)))
    def blue(rgb):
        return all(abs(a - b) <= 24 for a, b in zip(rgb, DESKTOP_BLUE))
    samples = []
    for x, y in ((width // 3, height // 3), (width // 2, height * 2 // 3), (width * 3 // 4, height // 4), (width // 8, height * 7 // 8)):
        x, y = int(x * scale), int(y * scale)
        rgb = image.getpixel((x, y))
        samples.append(dict(x=x, y=y, rgb=list(rgb), kind='workspace', ok=blue(rgb)))
    # The Haiku, home and Trash icons sit at the top left of the workspace.
    box = tuple(int(v * scale) for v in (16, 16, 176, 72))
    distinct = sum(1 for rgb in image.crop(box).getdata() if not blue(rgb))
    samples.append(dict(kind='icons', box=list(box), pixels=distinct, ok=distinct >= int(600 * scale * scale)))
    if scale != 1.0:
        # Enlarged icons reach below and beyond where the unscaled desktop's
        # icons end (about 172x70), and no Deskbar is drawn at the top right.
        region = image.crop((0, 0, int(420 * scale), int(220 * scale)))
        points = [(i % region.width, i // region.width) for i, rgb in enumerate(region.getdata()) if not blue(rgb)]
        reach = (max(p[0] for p in points), max(p[1] for p in points)) if points else (0, 0)
        samples.append(dict(kind='icon_reach', x=reach[0], y=reach[1],
            ok=reach[0] >= int(176 * scale) - 40 and reach[1] >= int(72 * scale) - 24))
        deskbar = image.crop((image.width - 140, 0, image.width, 110))
        drawn = sum(1 for rgb in deskbar.getdata() if not blue(rgb))
        samples.append(dict(kind='deskbar_absent', pixels=drawn, ok=drawn <= 20))
    failed = [s for s in samples if not s['ok']]
    if failed:
        raise ValidationError('frame does not show the %dx%d Haiku workspace crop: %r' % (width, height, failed))
    return dict(status='pass', scale=scale, samples=samples)


CURSOR_STATE = re.compile(
    r'^ROCK5_DISPLAY_CURSOR_(BEFORE|AFTER) width=(\d+) height=(\d+) hot=(\d+),(\d+) x=(-?\d+) y=(-?\d+)'
    r' visible=(\d) window=(\d+) mixer=(\d+) control=([0-9a-f]{8}) start=([0-9a-f]{8}) address=([0-9a-f]{8})'
    r' mix=([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}),([0-9a-f]{8}) saved=(yes|no|kept|removed)$', re.M)
CURSOR_ACCELERANT = re.compile(r'^ROCK5_DISPLAY_CURSOR_ACCELERANT flags=(\d+) width=(\d+) height=(\d+)$', re.M)
CURSOR_BITMAP = re.compile(r'^ROCK5_DISPLAY_CURSOR_BITMAP width=(\d+) height=(\d+) hot=(\d+),(\d+) result=(\d+) polls=(\d+)$', re.M)
CURSOR_MOVE = re.compile(r'^ROCK5_DISPLAY_CURSOR_MOVE x=(-?\d+) y=(-?\d+) result=(\d+) polls=(\d+) start=([0-9a-f]{8}) address=([0-9a-f]{8})$', re.M)
CURSOR_SHOW = re.compile(r'^ROCK5_DISPLAY_CURSOR_SHOW visible=(\d) result=(\d+) polls=(\d+) control=([0-9a-f]{8})$', re.M)
CURSOR_PASS = re.compile(
    r'^ROCK5_DISPLAY_CURSOR_PASS action=(place|hide|restore) x=(-?\d+) y=(-?\d+) visible=(\d) width=(\d+) height=(\d+) polls=(\d+)$', re.M)
# The alpha mixer words the driver programs for a straight-alpha ARGB cursor
# over the opaque desktop (DisplayCursor.h).
CURSOR_MIXER_WORDS = ('00ff0125', '00ff0060', '00000024', '00000074')
CURSOR_SIZE = 64
CURSOR_ROW_BYTES = CURSOR_SIZE * 4
CURSOR_POLL_LIMIT = 5000


def cursor_placement(x, y, hot_x, hot_y, width, height, frame=(1920, 1080)):
    """Return the driver's clipped window for a pointer at x, y, or None when it is off the frame."""
    left, top = x - hot_x, y - hot_y
    right, bottom = left + width, top + height
    if width == 0 or height == 0 or right <= 0 or bottom <= 0 or left >= frame[0] or top >= frame[1]:
        return None
    crop_x, crop_y = max(0, -left), max(0, -top)
    display_x, display_y = max(0, left), max(0, top)
    return dict(crop_x=crop_x, crop_y=crop_y, display_x=display_x, display_y=display_y,
        width=min(right, frame[0]) - display_x, height=min(bottom, frame[1]) - display_y,
        start='%08x' % (display_y << 16 | display_x), offset=crop_y * CURSOR_ROW_BYTES + crop_x * 4)


def _cursor_state(match):
    return dict(width=int(match.group(2)), height=int(match.group(3)), hot=(int(match.group(4)), int(match.group(5))),
        x=int(match.group(6)), y=int(match.group(7)), visible=int(match.group(8)), window=int(match.group(9)),
        mixer=int(match.group(10)), control=match.group(11), start=match.group(12), address=match.group(13),
        mix=tuple(match.group(i) for i in (14, 15, 16, 17)), saved=match.group(18))


def validate_cursor(body, action, x=None, y=None, frame=(1920, 1080), base=None, saved=None):
    """Return the decoded cursor control from a native --cursor transcript or raise ValidationError.

    `action` is place (the probe's 64x64 quadrant bitmap shown with its hot
    spot at x, y), hide (hidden where it is) or restore (the state the first
    placement saved comes back; `saved` is that placement's BEFORE state).
    `frame` is the mode the port shows, and `base` the cursor buffer address
    an unclipped placement reported, which a clipped one must offset.
    """
    if action not in ('place', 'hide', 'restore'):
        raise ValueError(action)
    if 'ROCK5_DISPLAY_CURSOR_REQUEST_CHECKS_PASS' not in body:
        raise ValidationError('cursor request boundary checks missing')
    accelerant = CURSOR_ACCELERANT.search(body)
    if accelerant is None:
        raise ValidationError('accelerant state before the cursor control missing')
    flags = int(accelerant.group(1))
    if flags & 17 != 17:
        raise ValidationError('accelerant flags %d lack acquisition or the cursor' % flags)
    if (int(accelerant.group(2)), int(accelerant.group(3))) != tuple(frame):
        raise ValidationError('accelerant frame %sx%s, expected %dx%d' % ((accelerant.group(2), accelerant.group(3)) + tuple(frame)))
    states = {m.group(1): _cursor_state(m) for m in CURSOR_STATE.finditer(body)}
    if set(states) != {'BEFORE', 'AFTER'}:
        raise ValidationError('cursor state lines missing')
    before, after = states['BEFORE'], states['AFTER']
    for state in (before, after):
        if state['window'] != 3 or state['mixer'] != 6:
            raise ValidationError('cursor window %d mixer %d, expected ESMART3 and mixer 6' % (state['window'], state['mixer']))
    # The first placement records app_server's state; later controls find it.
    if before['saved'] not in (('yes', 'no') if action == 'place' else ('yes',)):
        raise ValidationError('saved state %s before the %s' % (before['saved'], action))
    if after['saved'] != ('removed' if action == 'restore' else 'kept'):
        raise ValidationError('saved state %s after the %s' % (after['saved'], action))
    if after['mix'] != CURSOR_MIXER_WORDS and (after['visible'] or after['control'] != '00000000'):
        raise ValidationError('mixer words %r' % (after['mix'],))
    bitmap = CURSOR_BITMAP.search(body)
    move = CURSOR_MOVE.search(body)
    show = CURSOR_SHOW.search(body)
    if move is None or show is None:
        raise ValidationError('cursor move or show line missing')
    polls = [int(move.group(4)), int(show.group(3))]
    if int(move.group(3)) != 0 or int(show.group(2)) != 0:
        raise ValidationError('cursor move result %s show result %s' % (move.group(3), show.group(2)))
    if action == 'place':
        if x is None or y is None:
            raise ValueError('place needs x and y')
        if bitmap is None or (int(bitmap.group(1)), int(bitmap.group(2))) != (CURSOR_SIZE, CURSOR_SIZE):
            raise ValidationError('quadrant bitmap missing or not %dx%d' % (CURSOR_SIZE, CURSOR_SIZE))
        if (int(bitmap.group(3)), int(bitmap.group(4))) != (0, 0) or int(bitmap.group(5)) != 0:
            raise ValidationError('quadrant bitmap hot spot %s,%s result %s' % bitmap.group(3, 4, 5))
        polls.append(int(bitmap.group(6)))
        expected = dict(width=CURSOR_SIZE, height=CURSOR_SIZE, hot=(0, 0), x=x, y=y, visible=1)
    elif action == 'hide':
        if bitmap is not None:
            raise ValidationError('hide set a bitmap')
        expected = dict(width=before['width'], height=before['height'], hot=before['hot'], x=before['x'], y=before['y'], visible=0)
    else:
        if saved is None:
            raise ValueError('restore needs the saved state')
        # The saved bitmap comes back; a saved state without one clears the window's bitmap (0x0).
        if bitmap is None or (int(bitmap.group(1)), int(bitmap.group(2))) != (saved['width'], saved['height']):
            raise ValidationError('saved bitmap missing or not %dx%d' % (saved['width'], saved['height']))
        if (int(bitmap.group(3)), int(bitmap.group(4))) != tuple(saved['hot']) or int(bitmap.group(5)) != 0:
            raise ValidationError('saved bitmap hot spot %s,%s result %s' % bitmap.group(3, 4, 5))
        polls.append(int(bitmap.group(6)))
        expected = dict(width=saved['width'], height=saved['height'], hot=tuple(saved['hot']), x=saved['x'], y=saved['y'], visible=saved['visible'])
    if (int(move.group(1)), int(move.group(2))) != (expected['x'], expected['y']) or int(show.group(1)) != expected['visible']:
        raise ValidationError('move to %s,%s show %s, expected %d,%d show %d' % (move.group(1), move.group(2), show.group(1), expected['x'], expected['y'], expected['visible']))
    for key, value in expected.items():
        if after[key] != value:
            raise ValidationError('cursor %s is %r after the %s, expected %r' % (key, after[key], action, value))
    if any(p > CURSOR_POLL_LIMIT for p in polls):
        raise ValidationError('cursor polls %r' % polls)
    placement = cursor_placement(after['x'], after['y'], after['hot'][0], after['hot'][1], after['width'], after['height'], frame) if after['visible'] else None
    if placement is None:
        if after['control'] != '00000000' or after['start'] != '00000000' or show.group(4) != '00000000':
            raise ValidationError('cursor window still enabled while %s' % ('hidden' if not after['visible'] else 'off the frame'))
    else:
        if after['control'] != '00000001' or show.group(4) != '00000001':
            raise ValidationError('cursor window control %s after the %s' % (after['control'], action))
        # A move while hidden reports the stale window words; the state after
        # the show is what counts.
        if after['start'] != placement['start']:
            raise ValidationError('cursor start %s, expected %s' % (after['start'], placement['start']))
        address = int(after['address'], 16)
        if base is not None:
            if address != base + placement['offset']:
                raise ValidationError('cursor address %#x, expected %#x + %d' % (address, base, placement['offset']))
        elif address & 0xfff != placement['offset'] & 0xfff or address == 0 or address >= 1 << 32:
            raise ValidationError('implausible cursor address %#x for offset %d' % (address, placement['offset']))
    passed = CURSOR_PASS.search(body)
    if passed is None or passed.group(1) != action or body.count('ROCK5_DISPLAY_CURSOR_PASS ') != 1:
        raise ValidationError('cursor summary missing or inconsistent')
    if (int(passed.group(2)), int(passed.group(3)), int(passed.group(4)), int(passed.group(5)), int(passed.group(6))) != (
            after['x'], after['y'], after['visible'], after['width'], after['height']):
        raise ValidationError('cursor summary differs from the state')
    if int(passed.group(7)) != sum(polls):
        raise ValidationError('cursor summary polls %s, lines %r' % (passed.group(7), polls))
    return dict(status='pass', action=action, flags=flags, before=before, after=after, placement=placement,
        polls=polls, address=int(after['address'], 16) if after['address'] != '00000000' else None,
        base=base if base is not None else (int(after['address'], 16) if placement is not None and placement['offset'] == 0 else None))


# The probe's quadrant cursor as the capture shows it over the desktop blue:
# white, black, the desktop through the transparent quadrant, and half white.
CURSOR_QUADRANTS = (((16, 16), (255, 255, 255), 'white'), ((48, 16), (0, 0, 0), 'black'),
    ((16, 48), DESKTOP_BLUE, 'transparent'),
    ((48, 48), tuple(d + (255 - d) * 128 // 255 for d in DESKTOP_BLUE), 'half'))


def check_cursor_frame(path, x, y, shown=True, frame=(1920, 1080), reference=None):
    """Raise unless the capture shows the probe's quadrant cursor with its corner at x, y (or, hidden, the background there).

    `reference` is an earlier capture of the same desktop without a cursor
    at x, y: the transparent quadrant, the points around the cursor and a
    hidden cursor must show the reference's pixels, and the half-transparent
    quadrant their blend with white, whatever windows the desktop carries.
    Without a reference the plain desktop blue is expected. Only quadrant
    centres inside the frame are judged.
    """
    from PIL import Image
    image = Image.open(path).convert('RGB')
    if image.size != tuple(frame):
        raise ValidationError('frame is %dx%d, not %dx%d' % (image.size + tuple(frame)))
    background = Image.open(reference).convert('RGB') if reference is not None else None
    if background is not None and background.size != image.size:
        raise ValidationError('reference frame is %dx%d, not %dx%d' % (background.size + image.size))
    def under(px, py):
        return background.getpixel((px, py)) if background is not None else DESKTOP_BLUE
    samples = []
    for (dx, dy), colour, kind in CURSOR_QUADRANTS:
        px, py = x + dx, y + dy
        if not (0 <= px < frame[0] and 0 <= py < frame[1]):
            continue
        base = under(px, py)
        if not shown or kind == 'transparent':
            expected = base
        elif kind == 'half':
            expected = tuple(d + (255 - d) * 128 // 255 for d in base)
        else:
            expected = colour
        rgb = image.getpixel((px, py))
        tolerance = 32 if kind == 'half' and shown else 24
        samples.append(dict(x=px, y=py, rgb=list(rgb), kind=kind if shown else 'hidden_' + kind, expected=list(expected),
            ok=all(abs(a - b) <= tolerance for a, b in zip(rgb, expected))))
    if not samples:
        raise ValidationError('no quadrant centre of a cursor at %d,%d lies on the frame' % (x, y))
    for px, py in ((x - 24, y + 32), (x + CURSOR_SIZE + 24, y + 32), (x + 32, y - 24), (x + 32, y + CURSOR_SIZE + 24)):
        if not (0 <= px < frame[0] and 0 <= py < frame[1]):
            continue
        rgb = image.getpixel((px, py))
        expected = under(px, py)
        samples.append(dict(x=px, y=py, rgb=list(rgb), kind='outside', expected=list(expected),
            ok=all(abs(a - b) <= 24 for a, b in zip(rgb, expected))))
    failed = [s for s in samples if not s['ok']]
    if failed:
        raise ValidationError('frame does not show the %s cursor at %d,%d: %r' % ('quadrant' if shown else 'hidden', x, y, failed))
    return dict(status='pass', x=x, y=y, shown=shown, reference=reference, samples=samples)


def check_pointer_frame(path, x, y, size=24, frame=(1920, 1080)):
    """Raise unless the capture shows a pointer (light and dark pixels over the desktop) in the size x size box at x, y.

    app_server's default pointer is a white arrow with a black outline; on
    the hardware window it sits where the driver placed it, and the desktop
    blue surrounds the box.
    """
    from PIL import Image
    image = Image.open(path).convert('RGB')
    if image.size != tuple(frame):
        raise ValidationError('frame is %dx%d, not %dx%d' % (image.size + tuple(frame)))
    box = (max(0, x), max(0, y), min(frame[0], x + size), min(frame[1], y + size))
    pixels = list(image.crop(box).getdata())
    light = sum(1 for rgb in pixels if min(rgb) >= 180)
    dark = sum(1 for rgb in pixels if max(rgb) <= 90)
    blue = sum(1 for rgb in pixels if all(abs(a - b) <= 24 for a, b in zip(rgb, DESKTOP_BLUE)))
    around = []
    for px, py in ((x - 16, y + size // 2), (x + size + 16, y + size // 2), (x + size // 2, y - 16), (x + size // 2, y + size + 16)):
        if 0 <= px < frame[0] and 0 <= py < frame[1]:
            rgb = image.getpixel((px, py))
            around.append(dict(x=px, y=py, rgb=list(rgb), ok=all(abs(a - b) <= 24 for a, b in zip(rgb, DESKTOP_BLUE))))
    result = dict(status='pass', x=x, y=y, box=list(box), pixels=len(pixels), light=light, dark=dark, blue=blue, around=around)
    if light < 20 or dark < 12 or blue > len(pixels) - 40 or not around or not all(a['ok'] for a in around):
        result['status'] = 'fail'
        raise ValidationError('frame does not show a pointer at %d,%d: %r' % (x, y, result))
    return result
