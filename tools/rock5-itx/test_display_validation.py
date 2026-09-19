"""Check the display observation decoder against synthetic probe transcripts."""
import os
import unittest

import display_validation as check


def words(values):
    return ','.join('%08x' % (v & 0xffffffff) for v in values)


def port_words(width, height, standby=0, mode=0):
    values = [0] * check.VOP_VP_COUNT
    values[0] = (standby << 31) | mode
    htotal, hsync_end = width + 280, 44
    hstart = 192
    vtotal, vsync_end = height + 45, 5
    vstart = 41
    values[10] = (htotal << 16) | hsync_end
    values[11] = (hstart << 16) | (hstart + width)
    values[12] = (vtotal << 16) | vsync_end
    values[13] = (vstart << 16) | (vstart + height)
    return values


def transcript(samples=3, vop=True, hdmi=True, vop_on=True, vo1_on=True, hdmi_gated=False,
        consistent=True, flags=None, status_jitter=0, pll=True, vo0_on=True, dp_gated=False, aux_gated=False,
        gpio_gated=False, hpd_level=0, dp=None, gpio=None, aux=None, dp_hpd=0):
    lines = ['ROCK5_DISPLAY_WRITE_OPEN_REJECTED',
        'ROCK5_DISPLAY_RESOURCES version=2 flags=1 vop=0xfdd90000/0x4200 lut=0xfdd95000/0x1000'
        ' hdmi=0xfdea0000/0x20000 hdptx=0xfed70000/0x2000 hdptx_grf=0xfd5e4000/0x100'
        ' sys_grf=0xfd58c000/0x1000 vop_grf=0xfd5a4000/0x2000 vo1_grf=0xfd5a8000/0x4000'
        ' pmu=0xfd8d8000/0x400 cru=0xfd7c0000/0x5c000 gic=0xfe600000 vop_irq=188'
        ' hdmi_irqs=205,206,207,208,393 vop_clocks=605,604,609,610,611,612,603'
        ' hdmi_clocks=531,532,533,569,595,717 vop_pd=24 hdmi_pd=26 phy_phandle=108 vop_port=1'
        ' board=radxa,rock-5-itx usbdp=0xfed90000/0x10000 usbdp_grf=0xfd5cc000/0x4000'
        ' vo0_grf=0xfd5a6000/0x2000 ioc=0xfd5f0000/0x10000 gpio3=0xfec40000/0x100 dp=0xfde60000/0x4000'
        ' dp_pd=25 usbdp_clocks=673,621,599 usbdp_resets=15,16,17,18,537 gpio3_clocks=119,120',
        'ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS']
    # The DisplayPort path follows the domain and gate words unless overridden.
    dp = (vo0_on and not dp_gated) if dp is None else dp
    aux = (dp and not aux_gated) if aux is None else aux
    gpio = (not gpio_gated) if gpio is None else gpio
    if flags is None:
        flags = (1 | (2 if vop else 8) | (4 if hdmi else 16) | (32 if dp else 64) | (128 if aux else 0)
            | (256 if gpio else 512))
    repair = (1 << 16 if vop_on else 0) | (1 << 18 if vo1_on else 0) | (1 << 17 if vo0_on else 0)
    pmu = [0x1000, 0, 0x1000, 0, 0x1000, 0, 0, 0x0fff0 & ~((vop_on << 1) | (vo0_on << 2) | (vo1_on << 3)), 0, 0x0e000000, repair]
    gate = [0, 0x8000, 0, 0x4 if hdmi_gated else 0, 0, 0, 0, 0x4 if gpio_gated else 0,
        (0x20 if dp_gated else 0) | (0x8 if aux_gated else 0)]
    status1 = (1 << 27) | (1 << 24)
    ports = [port_words(0, 0, standby=1), port_words(1920, 1080), port_words(0, 0, standby=1),
        port_words(0, 0, standby=1)]
    for index in range(samples):
        lines.append('ROCK5_DISPLAY_SNAPSHOT sample=%d version=2 flags=%#x start_us=%d end_us=%d'
            % (index, flags, 1000 + index * 10, 1050 + index * 10))
        lines.append('ROCK5_DISPLAY_PMU sample=%d %s' % (index, words(pmu)))
        lines.append('ROCK5_DISPLAY_CRU_SELECT sample=%d %s' % (index, words([0x1234, 0x0a00, 0, 0])))
        lines.append('ROCK5_DISPLAY_CRU_GATE sample=%d %s' % (index, words(gate)))
        lines.append('ROCK5_DISPLAY_SYS_GRF sample=%d %s' % (index, words([0, 0x3000, status1 | (status_jitter * index)])))
        lines.append('ROCK5_DISPLAY_VOP_GRF sample=%d %s' % (index, words([0x2])))
        lines.append('ROCK5_DISPLAY_VO1_GRF sample=%d %s' % (index, words([0x0, 0x0])))
        lines.append('ROCK5_DISPLAY_HDPTX1_GRF sample=%d %s' % (index, words([0xe0, 0xf] if pll else [0, 0])))
        lines.append('ROCK5_DISPLAY_USBDP1_GRF sample=%d %s' % (index, words([0, 0, 0, 0, 0x2, 0, 0x1100])))
        lines.append('ROCK5_DISPLAY_VO0_GRF sample=%d %s' % (index, words([0x0, 0, 0])))
        lines.append('ROCK5_DISPLAY_IOC sample=%d %s' % (index, words([0x0, 0x50])))
        if gpio:
            lines.append('ROCK5_DISPLAY_GPIO3 sample=%d %s' % (index, words([0, 0, 0, 0, (hpd_level << 29) | (index << 3), 0x0101157c])))
        if dp:
            lines.append('ROCK5_DISPLAY_DP1 sample=%d %s' % (index, words([0x14110600, 0x1, 0x4450, 0, 0, 0, 0, 0x0, 0, 0, 0x0, 0x3])))
        if aux:
            lines.append('ROCK5_DISPLAY_DP1_AUX sample=%d %s' % (index, words([0x1 * index, 0, dp_hpd << 8])))
        if vop:
            system = [0] * check.VOP_SYS_COUNT
            system[1] = 0x35880000
            system[4] = (1 << 5) | (1 << 18)  # hdmi1 enabled from video port 1
            if not consistent and index == samples - 1:
                system[4] ^= 1 << 3
            lines.append('ROCK5_DISPLAY_VOP_SYS sample=%d %s' % (index, words(system)))
            lines.append('ROCK5_DISPLAY_VOP_OVL sample=%d %s' % (index, words([0, 0x76543210, 0x1, 0, 0, 0, 0])))
            for port in range(4):
                lines.append('ROCK5_DISPLAY_VOP_VP%d sample=%d %s' % (port, index, words(ports[port])))
            for window in range(4):
                lines.append('ROCK5_DISPLAY_VOP_CLUSTER%d sample=%d %s' % (window, index, words([0] * 8)))
            for window in range(4):
                values = [0] * 7
                if window == 1:
                    values = [1, 1, 0xed3a0000, 1920, (1079 << 16) | 1919, (1079 << 16) | 1919, 0]
                lines.append('ROCK5_DISPLAY_VOP_ESMART%d sample=%d %s' % (window, index, words(values)))
            for port in range(4):
                t = check.decode_port(ports[port])
                lines.append('ROCK5_DISPLAY_VP_TIMING sample=%d port=%d standby=%d out_mode=%d'
                    ' htotal=%d hsync_end=%d hactive=%d-%d vtotal=%d vsync_end=%d vactive=%d-%d'
                    ' width=%d height=%d' % (index, port, t['standby'], t['out_mode'], t['htotal'],
                    t['hsync_end'], t['hactive_start'], t['hactive_end'], t['vtotal'], t['vsync_end'],
                    t['vactive_start'], t['vactive_end'], t['width'], t['height']))
            lines.append('ROCK5_DISPLAY_IF sample=%d dp0=0 dp1=0 edp0=0 hdmi0=0 edp1=0 hdmi1=1'
                ' mipi0=0 mipi1=0 rgb=0 dp0_mux=0 dp1_mux=0 hdmi_edp0_mux=0 hdmi_edp1_mux=1'
                ' version=35880000' % index)
        if hdmi:
            lines.append('ROCK5_DISPLAY_HDMI1 sample=%d %s' % (index, words(list(range(10)))))
        lines.append('ROCK5_DISPLAY_DP1_PATH sample=%d vo0_on=%d pclk_dp1_gated=%d aux_gated=%d hdcp_gated=0'
            ' usbdp_pclk_gated=0 immortal_gated=0 gpio3_gated=%d pin_mux=5 pin_level=%d pin_direction=0'
            ' dp_read=%d aux_read=%d version=14110600 soft_reset=00000000 phyif=00000000 pwrdown=00000003'
            ' hpd_status=%08x lane_mux=00000000' % (index, vo0_on, dp_gated, aux_gated, gpio_gated, hpd_level,
            dp, aux, dp_hpd << 8))
        lines.append('ROCK5_DISPLAY_HPD sample=%d hdmi0_level=0 hdmi0_int=0 hdmi1_level=1 hdmi1_int=1'
            ' vop_on=%d vo0_on=%d vo1_on=%d vop_gates=0x0 hdmi_gates=%#x hdptx1_status=0xf'
            % (index, vop_on, vo0_on, vo1_on, 4 if hdmi_gated else 0))
    lines.append('ROCK5_DISPLAY_OBSERVATION_PASS samples=%d register_writes=0 consistent=1'
        ' vop_read=%d hdmi_read=%d dp_read=%d gpio_read=%d' % (samples, vop, hdmi, dp, gpio))
    return '\n'.join(lines) + '\n'


def edid_blocks():
    base = bytearray(128)
    base[:8] = check.EDID_HEADER
    base[8:10] = (0x10 << 10 | 0x0e << 5 | 0x0c).to_bytes(2, 'big')  # "PNL"
    base[10:12] = (0x1234).to_bytes(2, 'little')
    base[12:16] = (0x01020304).to_bytes(4, 'little')
    base[16], base[17], base[18], base[19], base[20] = 12, 33, 1, 4, 0x80
    dtd = bytearray(18)
    clock = 14850
    dtd[0], dtd[1] = clock & 0xff, clock >> 8
    dtd[2], dtd[3], dtd[4] = 1920 & 0xff, 280 & 0xff, (1920 >> 8) << 4 | (280 >> 8)
    dtd[5], dtd[6], dtd[7] = 1080 & 0xff, 45 & 0xff, (1080 >> 8) << 4 | (45 >> 8)
    dtd[8], dtd[9] = 88, 44
    dtd[10] = 4 << 4 | 5
    dtd[11] = 0
    dtd[12], dtd[13], dtd[14] = 600 & 0xff, 340 & 0xff, (600 >> 8) << 4 | (340 >> 8)
    dtd[17] = 0x1e
    base[54:72] = dtd
    base[126] = 1
    base[127] = (0x100 - sum(base) % 256) % 256
    ext = bytearray(128)
    ext[0], ext[1] = 0x02, 0x03
    for i in range(2, 127):
        ext[i] = (i * 5) & 0xff
    ext[127] = (0x100 - sum(ext) % 256) % 256
    return bytes(base), bytes(ext)


def edid_transcript(blocks=None, info=None, summary=None):
    base, ext = edid_blocks()
    blocks = blocks if blocks is not None else [base, ext]
    lines = ['ROCK5_DISPLAY_WRITE_OPEN_REJECTED', 'ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS',
        'ROCK5_DISPLAY_EDID_REQUEST_CHECKS_PASS']
    for index, data in enumerate(blocks):
        lines.append('ROCK5_DISPLAY_EDID block=%d result=0 flags=%s bytes=128 polls=%d control=00000a00/00000a00'
            ' status=00000000/00000000 hpd=09000000 start_us=%d end_us=%d hex=%s'
            % (index, check.chex(1 if index >= 2 else 0), 300 + index, 1000 + index * 100, 1050 + index * 100, data.hex()))
    try:
        decoded = check.decode_edid_base(blocks[0])
    except check.ValidationError:
        decoded = None
    if info is None and decoded is not None:
        info = ('ROCK5_DISPLAY_EDID_INFO manufacturer=%s product=%s serial=%s week=%d year=%d version=%s'
            ' extensions=%d digital=%d preferred=%dx%d pixel_khz=%d hblank=%d vblank=%d hsync_offset=%d'
            ' hsync_width=%d vsync_offset=%d vsync_width=%d flags=%s size_mm=%dx%d checksum=ok'
            % (decoded['manufacturer'], check.chex(decoded['product']), check.chex(decoded['serial']), decoded['week'], decoded['year'],
            decoded['version'], decoded['extensions'], decoded['digital'], decoded['width'], decoded['height'],
            decoded['pixel_khz'], decoded['hblank'], decoded['vblank'], decoded['hsync_offset'],
            decoded['hsync_width'], decoded['vsync_offset'], decoded['vsync_width'], check.chex(decoded['flags']),
            decoded['width_mm'], decoded['height_mm']))
    if info:
        lines.append(info)
    for index, data in enumerate(blocks[1:], 1):
        lines.append('ROCK5_DISPLAY_EDID_EXTENSION block=%d tag=%s revision=%d checksum=ok' % (index, check.chex(data[0]), data[1]))
    lines.append(summary or 'ROCK5_DISPLAY_EDID_PASS blocks=%d extensions=%d polls=%d register_writes=i2c_master_only'
        % (len(blocks), blocks[0][126], sum(300 + i for i in range(len(blocks)))))
    return '\n'.join(lines) + '\n'


def scanout_transcript(firmware=0xed280000, pattern=0x40100000, port=2, window=2, hold=12, steps=None):
    lines = ['ROCK5_DISPLAY_WRITE_OPEN_REJECTED', 'ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS',
        'ROCK5_DISPLAY_SCANOUT_REQUEST_CHECKS_PASS']
    commit = 0x8000 | 1 << port | 1 << (port + 16)
    steps = steps or [('query', 0, firmware, 0, 0, 0, 0, 0), ('show', 1, firmware, pattern, firmware, pattern, commit, 700),
        ('query', 1, pattern, 0, firmware, pattern, 0, 0), ('restore', 0, pattern, firmware, firmware, pattern, commit, 650),
        ('query', 0, firmware, 0, firmware, pattern, 0, 0)]
    for index, (action, flags, before, after, fw, pat, cfg, polls) in enumerate(steps):
        lines.append('ROCK5_DISPLAY_SCANOUT action=%s result=0 flags=%d port=%d window=%d before=%08x after=%08x'
            ' firmware=%08x pattern=%08x region_control=00000001 virtual=1920 active=0437077f display=0437077f'
            ' start=00000000 if_en=%08x cfg_done=%08x polls=%d start_us=%d end_us=%d'
            % (action, flags, port, window, before, after, fw, pat, 0x20 | port << 18, cfg, polls, 5000 + index * 100, 5040 + index * 100))
        if action == 'show':
            lines.append('ROCK5_DISPLAY_SCANOUT_HOLD seconds=%d' % hold)
    lines.append('ROCK5_DISPLAY_SCANOUT_PASS port=%d window=%d firmware=%08x pattern=%08x hold_seconds=%d'
        ' register_writes=window_address_and_cfg_done' % (port, window, firmware, pattern, hold))
    return '\n'.join(lines) + '\n'


def pattern_image(path, quality=80, blank=False):
    from PIL import Image, ImageDraw
    image = Image.new('RGB', (1920, 1080), (66, 110, 150) if blank else check.PATTERN_BORDER)
    if not blank:
        draw = ImageDraw.Draw(image)
        for bar, color in enumerate(check.PATTERN_COLORS):
            x = 32 + bar * check.PATTERN_BAR_WIDTH
            draw.rectangle([x, 32, x + check.PATTERN_BAR_WIDTH - 1, 1047], fill=color)
    image.save(path, format='JPEG', quality=quality)


def accelerant_transcript(framebuffer=0x14c00000, firmware=0xed280000, flags=7, edid=None, timing=None, extra='',
        retrace='ROCK5_DISPLAY_RETRACE waits=24 timeouts=0 first_us=5000000 last_us=5383341 period_us=16667'
        ' retraces_before=1800 retraces_after=1826 elapsed_us=400300', retrace_sem=1310):
    base, _ = edid_blocks()
    edid = edid if edid is not None else base.hex()
    timing = timing or ' pixel_khz=148500 h=2008/2052/2200 v=1084/1089/1125 port_timing=0898002c,00c00840,04650005,00290461'
    lines = ['ROCK5_DISPLAY_WRITE_OPEN_ALLOWED', 'ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS',
        'ROCK5_DISPLAY_ACCELERANT_REQUEST_CHECKS_PASS',
        'ROCK5_DISPLAY_ACCELERANT flags=%d shared_area=1234 framebuffer=%08x firmware=%08x port=2 window=2 polls=300'
        ' width=1920 height=1080 bytes_per_row=7680 retrace_sem=%d retraces=1800' % (flags, framebuffer, firmware, retrace_sem if flags & 4 else -1),
        retrace if flags & 4 and retrace else None,
        'ROCK5_DISPLAY_ACCELERANT_SIGNATURE rk3588_display.accelerant',
        'ROCK5_DISPLAY_ACCELERANT_DEVICE graphics/rk3588_display/0',
        'ROCK5_DISPLAY_ACCELERANT_SHARED version=1 flags=%d mode_list_area=1240 modes=1 size=1920x1080 bytes_per_row=7680%s'
        ' edid_result=%d name=RK3588 VOP2 HDMI TX1 edid=%s' % (flags & 2, timing, 0 if flags & 2 else 2, edid),
        'ROCK5_DISPLAY_ACCELERANT_CLONE area=1250 size=8294400 samples=ff336699,ff336699,ff336699,ffdddddd',
        'ROCK5_DISPLAY_ACCELERANT_ACQUIRE_BUSY',
        'ROCK5_DISPLAY_ACCELERANT_PASS acquired=1 edid=%d framebuffer=%08x firmware=%08x register_writes=owner_only'
        % (1 if flags & 2 else 0, framebuffer, firmware)]
    return '\n'.join(line for line in lines if line is not None) + extra + '\n'


class DisplayValidationTest(unittest.TestCase):
    def test_write_open_outcome(self):
        self.assertEqual(check.validate(transcript())['write_open'], 'rejected')
        allowed = transcript().replace('ROCK5_DISPLAY_WRITE_OPEN_REJECTED', 'ROCK5_DISPLAY_WRITE_OPEN_ALLOWED')
        self.assertEqual(check.validate(allowed)['write_open'], 'allowed')
        with self.assertRaises(check.ValidationError):
            check.validate(transcript() + 'ROCK5_DISPLAY_WRITE_OPEN_ALLOWED\n')

    def test_accelerant_transcript(self):
        base, _ = edid_blocks()
        observation = dict(active_ports=[2], windows=dict(esmarts=[dict(region_control=0, address=0)] * 2
            + [dict(region_control=1, address=0x14c00000)] + [dict(region_control=0, address=0)]))
        decoded = check.validate_accelerant(accelerant_transcript(), observation, base.hex())
        self.assertEqual((decoded['framebuffer'], decoded['firmware'], decoded['port'], decoded['window']), ('14c00000', 'ed280000', 2, 2))
        self.assertEqual((decoded['polls'], decoded['modes'], decoded['edid_result'], decoded['flags']), (300, 1, 0, 7))
        self.assertEqual(decoded['retrace'], dict(waits=24, period_us=16667, count_delta=26, elapsed_us=400300))
        with_status = accelerant_transcript(retrace='ROCK5_DISPLAY_RETRACE waits=24 timeouts=0 first_us=5000000 last_us=5383341 period_us=16667 retraces_before=1800 retraces_after=1826 elapsed_us=400300 errors=0 status=ok')
        self.assertEqual(check.validate_accelerant(with_status)['retrace']['waits'], 24)
        self.assertIsNone(check.validate_accelerant(accelerant_transcript(flags=3))['retrace'])
        legacy = accelerant_transcript(flags=3).replace(' retrace_sem=-1 retraces=1800', '')
        self.assertEqual(check.validate_accelerant(legacy)['flags'], 3)
        with self.assertRaises(check.ValidationError):
            check.validate_accelerant(accelerant_transcript().replace(' retrace_sem=1310 retraces=1800', ''))
        self.assertEqual(decoded['name'], 'RK3588 VOP2 HDMI TX1')
        self.assertEqual(decoded['samples'][3], 'ffdddddd')
        without = check.validate_accelerant(accelerant_transcript(flags=1))
        self.assertEqual((without['flags'], without['edid_result']), (1, 2))
        # After a mode change the accelerant reports the new mode, its CEA
        # timing and port words, at the buffer's unchanged row pitch.
        self.assertEqual(check.shared_timing(1920, 1080), check.FIRMWARE_TIMING)
        self.assertEqual(check.shared_timing(1280, 720), dict(h=(1390, 1430, 1650), v=(725, 730, 750),
            pixel_khz=74250, port_timing=('06720028', '01040604', '02ee0005', '001902e9')))
        changed = accelerant_transcript(flags=15, timing=' pixel_khz=74250 h=1390/1430/1650 v=725/730/750'
            ' port_timing=06720028,01040604,02ee0005,001902e9').replace(
            ' width=1920 height=1080 bytes_per_row=7680', ' width=1280 height=720 bytes_per_row=7680').replace(
            ' size=1920x1080 ', ' size=1280x720 ')
        self.assertEqual(check.validate_accelerant(changed, mode=(1280, 720))['flags'], 15)
        for wrong in (changed.replace(' size=1280x720 ', ' size=1920x1080 '), changed.replace('h=1390/', 'h=1391/'),
                changed.replace('06720028', '06720029'), accelerant_transcript()):
            with self.assertRaises(check.ValidationError):
                check.validate_accelerant(wrong, mode=(1280, 720))
        with self.assertRaises(check.ValidationError):
            check.validate_accelerant(changed)
        retrace_cases = [
            ('no_sem', accelerant_transcript(retrace_sem=-1)),
            ('missing', accelerant_transcript(retrace='')),
            ('timeouts', accelerant_transcript(retrace='ROCK5_DISPLAY_RETRACE waits=24 timeouts=1 first_us=5000000 last_us=5383341 period_us=16667 retraces_before=1800 retraces_after=1826 elapsed_us=400300')),
            ('period', accelerant_transcript(retrace='ROCK5_DISPLAY_RETRACE waits=24 timeouts=0 first_us=5000000 last_us=5383341 period_us=33333 retraces_before=1800 retraces_after=1826 elapsed_us=400300')),
            ('count', accelerant_transcript(retrace='ROCK5_DISPLAY_RETRACE waits=24 timeouts=0 first_us=5000000 last_us=5383341 period_us=16667 retraces_before=1800 retraces_after=1810 elapsed_us=400300')),
            ('runaway', accelerant_transcript(retrace='ROCK5_DISPLAY_RETRACE waits=24 timeouts=0 first_us=5000000 last_us=5383341 period_us=16667 retraces_before=1800 retraces_after=1900 elapsed_us=400300')),
            ('mismatch', accelerant_transcript(retrace='ROCK5_DISPLAY_RETRACE waits=24 timeouts=0 first_us=5000000 last_us=5383341 period_us=16667 retraces_before=1799 retraces_after=1826 elapsed_us=400300')),
            ('unflagged', accelerant_transcript(flags=3, extra='\nROCK5_DISPLAY_RETRACE waits=24 timeouts=0 first_us=5000000 last_us=5383341 period_us=16667 retraces_before=1800 retraces_after=1826 elapsed_us=400300')),
            ('errors', accelerant_transcript(retrace='ROCK5_DISPLAY_RETRACE waits=24 timeouts=0 first_us=0 last_us=0 period_us=0 retraces_before=1800 retraces_after=1800 elapsed_us=455 errors=24 status=Operation not allowed')),
        ]
        for name, value in retrace_cases:
            with self.subTest(name=name):
                with self.assertRaises(check.ValidationError):
                    check.validate_accelerant(value)
        body = accelerant_transcript()
        cases = [
            ('open', body.replace('ROCK5_DISPLAY_WRITE_OPEN_ALLOWED', 'ROCK5_DISPLAY_WRITE_OPEN_REJECTED')),
            ('checks', body.replace('ROCK5_DISPLAY_ACCELERANT_REQUEST_CHECKS_PASS\n', '')),
            ('not_acquired', body + 'ROCK5_DISPLAY_ACCELERANT_NOT_ACQUIRED errno=1\n'),
            ('flags', body.replace('ACCELERANT flags=7', 'ACCELERANT flags=6')),
            ('same_buffer', accelerant_transcript(framebuffer=0xed280000)),
            ('unaligned', accelerant_transcript(framebuffer=0x14c00800)),
            ('geometry', body.replace('width=1920 height=1080 bytes_per_row=7680', 'width=1920 height=1080 bytes_per_row=7684')),
            ('signature', body.replace('SIGNATURE rk3588_display.accelerant', 'SIGNATURE framebuffer.accelerant')),
            ('device', body.replace('DEVICE graphics/rk3588_display/0', 'DEVICE graphics/rk3588_display/1')),
            ('modes', body.replace('mode_list_area=1240 modes=1', 'mode_list_area=-1 modes=0')),
            ('timing', accelerant_transcript(timing=' pixel_khz=148500 h=2008/2052/2200 v=1084/1089/1126 port_timing=0898002c,00c00840,04650005,00290461')),
            ('port_words', accelerant_transcript(timing=' pixel_khz=148500 h=2008/2052/2200 v=1084/1089/1125 port_timing=0898002c,00c00840,04650005,00290462')),
            ('edid_flag', body.replace('edid_result=0', 'edid_result=3')),
            ('clone', body.replace('size=8294400', 'size=8294396')),
            ('busy', body.replace('ROCK5_DISPLAY_ACCELERANT_ACQUIRE_BUSY\n', '')),
            ('summary', body.replace('register_writes=owner_only', 'register_writes=none')),
        ]
        for name, value in cases:
            with self.subTest(name=name):
                with self.assertRaises(check.ValidationError):
                    check.validate_accelerant(value)
        with self.assertRaises(check.ValidationError):
            check.validate_accelerant(body, dict(observation, active_ports=[1]))
        with self.assertRaises(check.ValidationError):
            check.validate_accelerant(accelerant_transcript(framebuffer=0x14d00000), observation)
        with self.assertRaises(check.ValidationError):
            check.validate_accelerant(body, None, 'ab' * 128)
        with self.assertRaises(check.ValidationError):
            check.validate_accelerant(accelerant_transcript(flags=1), None, base.hex())

    def test_scanout_transcript(self):
        decoded = check.validate_scanout(scanout_transcript())
        self.assertEqual(decoded['port'], 2)
        self.assertEqual(decoded['window'], 2)
        self.assertEqual((decoded['firmware'], decoded['pattern'], decoded['commit']), ('ed280000', '40100000', '00048004'))
        self.assertEqual(decoded['hold_seconds'], 12)
        self.assertEqual(decoded['micros'], dict(query_before=40, show=40, query_swapped=40, restore=40, query_after=40))
        self.assertEqual(decoded['polls'], dict(query_before=0, show=700, query_swapped=0, restore=650, query_after=0))
        observation = dict(active_ports=[2], windows=dict(esmarts=[dict(region_control=0, address=0)] * 2
            + [dict(region_control=1, address=0xed280000)] + [dict(region_control=0, address=0)]))
        self.assertEqual(check.validate_scanout(scanout_transcript(), observation)['status'], 'pass')
        self.assertEqual(check.validate_scanout(scanout_transcript(port=1, window=0))['commit'], '00028002')

    def test_scanout_rejections(self):
        body = scanout_transcript()
        observation = dict(active_ports=[2], windows=dict(esmarts=[dict(region_control=0, address=0)] * 2
            + [dict(region_control=1, address=0xed280000)] + [dict(region_control=0, address=0)]))
        cases = [
            ('checks', body.replace('ROCK5_DISPLAY_SCANOUT_REQUEST_CHECKS_PASS\n', '')),
            ('abort', body + 'ROCK5_DISPLAY_SCANOUT_ABORT step=restore\n'),
            ('order', body.replace('action=restore result=0 flags=0', 'action=restore result=0 flags=1')),
            ('result', body.replace('action=show result=0', 'action=show result=6')),
            ('same_address', scanout_transcript(pattern=0xed280000)),
            ('unaligned', scanout_transcript(pattern=0x40100800)),
            ('geometry', body.replace('virtual=1920', 'virtual=1921', 1)),
            ('routing', body.replace('if_en=00080020', 'if_en=00040020', 1)),
            ('commit', body.replace('cfg_done=00048004', 'cfg_done=00048002', 1)),
            ('restore_target', body.replace('action=restore result=0 flags=0 port=2 window=2 before=40100000 after=ed280000',
                'action=restore result=0 flags=0 port=2 window=2 before=40100000 after=ed281000')),
            ('hold', body.replace('ROCK5_DISPLAY_SCANOUT_HOLD seconds=12\n', '')),
            ('summary', body.replace('hold_seconds=12', 'hold_seconds=13')),
            ('duration', body.replace('start_us=5100 end_us=5140', 'start_us=5100 end_us=9000000')),
            ('polls', body.replace('cfg_done=00048004 polls=700', 'cfg_done=00048004 polls=5001')),
            ('query_polls', body.replace('cfg_done=00000000 polls=0 start_us=5000', 'cfg_done=00000000 polls=1 start_us=5000')),
        ]
        for name, value in cases:
            with self.subTest(name=name):
                with self.assertRaises(check.ValidationError):
                    check.validate_scanout(value)
        with self.assertRaises(check.ValidationError):
            check.validate_scanout(body, dict(observation, active_ports=[1]))
        with self.assertRaises(check.ValidationError):
            check.validate_scanout(scanout_transcript(firmware=0xed281000), observation)

    def test_desktop_frame(self):
        import tempfile
        from PIL import Image, ImageDraw
        with tempfile.TemporaryDirectory() as directory:
            desktop = directory + '/desktop.jpg'
            image = Image.new('RGB', (1920, 1080), check.DESKTOP_BLUE)
            ImageDraw.Draw(image).rectangle([1784, 0, 1919, 70], fill=(200, 200, 200))
            image.save(desktop, format='JPEG', quality=80)
            self.assertEqual(len(check.check_desktop_frame(desktop)['samples']), 7)
            real = '/mnt/HaikuWork/artifacts/interactive/20260918T055257Z-442e25/frame-026.jpg'
            if os.path.exists(real):
                self.assertEqual(check.check_desktop_frame(real)['status'], 'pass')
            pattern_image(desktop)
            with self.assertRaises(check.ValidationError):
                check.check_desktop_frame(desktop)
            Image.new('RGB', (1920, 1080), check.DESKTOP_BLUE).save(desktop, format='JPEG')
            with self.assertRaises(check.ValidationError):
                check.check_desktop_frame(desktop)

    def test_modeset_transcript(self):
        line = ('ROCK5_DISPLAY_MODE width=1280 height=720 clock=74250 vic=4 result=0 phase=6 hold_polls=2'
            ' clock_polls=3 lock_polls=7 phy_status=0000000e timing=06720028,01040604,02ee0005,001902e9'
            ' if_en=00080020 micros=41230')
        body = ('ROCK5_DISPLAY_MODE_REQUEST_CHECKS_PASS\n' + line + '\n'
            'ROCK5_DISPLAY_MODE_ACCELERANT width=1280 height=720 flags=15 retraces=3000\n'
            'ROCK5_DISPLAY_MODE_PASS width=1280 height=720 clock=74250 vic=4\n')
        decoded = check.validate_modeset(body, 1280, 720)
        self.assertEqual((decoded['clock'], decoded['vic'], decoded['polls'], decoded['retraces']),
            (74250, 4, dict(hold=2, clock=3, lock=7), 3000))
        back = body.replace('width=1280 height=720', 'width=1920 height=1080').replace('clock=74250 vic=4', 'clock=148500 vic=16').replace(
            'timing=06720028,01040604,02ee0005,001902e9', 'timing=0898002c,00c00840,04650005,00290461')
        self.assertEqual(check.validate_modeset(back, 1920, 1080)['timing'], ['0898002c', '00c00840', '04650005', '00290461'])
        cases = [
            ('checks', body.replace('ROCK5_DISPLAY_MODE_REQUEST_CHECKS_PASS\n', '')),
            ('size', (body, 1920, 1080)),
            ('result', body.replace('result=0 phase=6', 'result=4 phase=2')),
            ('status', body.replace('phy_status=0000000e', 'phy_status=00000004')),
            ('timing', body.replace('001902e9', '001902e8')),
            ('routing', body.replace('if_en=00080020', 'if_en=00040020')),
            ('accelerant', body.replace('ACCELERANT width=1280 height=720', 'ACCELERANT width=1920 height=1080')),
            ('flags', body.replace('flags=15', 'flags=7')),
            ('summary', body.replace('MODE_PASS width=1280', 'MODE_PASS width=1281')),
        ]
        for name, value in cases:
            with self.subTest(name=name):
                with self.assertRaises(check.ValidationError):
                    if isinstance(value, tuple):
                        check.validate_modeset(*value)
                    else:
                        check.validate_modeset(value, 1280, 720)

    def test_power_transcript(self):
        def transcript(mode, phase=None, control=None, before=1800, after=None, shared=None, previous=None, extra=''):
            off = mode == 'off'
            phase = 2 if off else 6 if phase is None else phase
            control = ('80000000' if off else '0000000f') if control is None else control
            after = (before if off else before + 30) if after is None else after
            shared = (1 if off else 0) if shared is None else shared
            previous = (0 if off else 1) if previous is None else previous
            return '\n'.join(['ROCK5_DISPLAY_POWER_REQUEST_CHECKS_PASS',
                'ROCK5_DISPLAY_POWER mode=%s result=0 phase=%d hold_polls=%d clock_polls=%d lock_polls=%d'
                ' phy_status=0000000e control=%s previous=%d micros=%d'
                % (mode, phase, 14 if off else 0, 0 if off else 5, 0 if off else 1, control, previous, 12000),
                'ROCK5_DISPLAY_POWER_ACCELERANT width=1920 height=1080 flags=15 retraces_before=%d'
                ' retraces_after=%d shared_power=%d' % (before, after, shared),
                'ROCK5_DISPLAY_POWER_PASS mode=%s' % mode, extra]) + '\n'
        off = check.validate_power(transcript('off'), 'off', previous=0)
        self.assertEqual((off['phase'], off['control'], off['retraces_after'], off['shared_power']), (2, '80000000', 1800, 1))
        on = check.validate_power(transcript('on'), 'on', previous=1)
        self.assertEqual((on['phase'], on['control'], on['retraces_after'] - on['retraces_before'], on['clock_polls']), (6, '0000000f', 30, 5))
        with self.assertRaises(ValueError):
            check.validate_power(transcript('on'), 'standby')
        cases = [
            ('other_mode', transcript('on'), 'off', None),
            ('previous', transcript('on'), 'on', 0),
            ('frames_while_off', transcript('off', after=1803), 'off', None),
            ('not_standby', transcript('off', control='0000000f'), 'off', None),
            ('shared_off', transcript('off', shared=0), 'off', None),
            ('frames_stalled', transcript('on', after=1805), 'on', None),
            ('still_standby', transcript('on', control='8000000f'), 'on', None),
            ('phase', transcript('on', phase=5), 'on', None),
            ('phy', transcript('on').replace('phy_status=0000000e', 'phy_status=0000000c'), 'on', None),
            ('result', transcript('on').replace('result=0 phase=6', 'result=4 phase=2'), 'on', None),
            ('checks', transcript('on').replace('ROCK5_DISPLAY_POWER_REQUEST_CHECKS_PASS\n', ''), 'on', None),
            ('summary', transcript('on').replace('ROCK5_DISPLAY_POWER_PASS mode=on', 'ROCK5_DISPLAY_POWER_PASS mode=off'), 'on', None),
            ('flags', transcript('on').replace('flags=15', 'flags=7'), 'on', None),
        ]
        for name, body, mode, previous in cases:
            with self.subTest(name):
                with self.assertRaises(check.ValidationError):
                    check.validate_power(body, mode, previous=previous)
        # The accelerant probe reports the shared power mode when the probe prints it.
        powered = accelerant_transcript().replace(' name=RK3588', ' power=0 name=RK3588')
        self.assertEqual(check.validate_accelerant(powered)['power'], 0)
        self.assertIsNone(check.validate_accelerant(accelerant_transcript())['power'])
        with self.assertRaises(check.ValidationError):
            check.validate_accelerant(accelerant_transcript().replace(' name=RK3588', ' power=1 name=RK3588'))

    def test_desktop_crop(self):
        import tempfile
        from PIL import Image, ImageDraw
        with tempfile.TemporaryDirectory() as directory:
            crop = directory + '/crop.jpg'
            image = Image.new('RGB', (1280, 720), check.DESKTOP_BLUE)
            draw = ImageDraw.Draw(image)
            for x in (20, 80, 140):
                draw.rectangle([x, 20, x + 32, 52], fill=(230, 200, 60))
            image.save(crop, format='JPEG', quality=80)
            self.assertEqual(check.check_desktop_crop(crop, 1280, 720)['status'], 'pass')
            with self.assertRaises(check.ValidationError):
                check.check_desktop_crop(crop, 1920, 1080)
            Image.new('RGB', (1280, 720), check.DESKTOP_BLUE).save(crop, format='JPEG')
            with self.assertRaises(check.ValidationError):
                check.check_desktop_crop(crop, 1280, 720)
            real = '/mnt/HaikuWork/artifacts/interactive/20260918T083449Z-28d1a5/frame-007.jpg'
            if os.path.exists(real):
                self.assertEqual(check.check_desktop_crop(real, 1920, 1080)['status'], 'pass')
            # The NanoKVM scales a 1280x720 mode to 1920x1080: larger icons, no Deskbar.
            scaled = directory + '/scaled.jpg'
            def capture(icons_scale, deskbar):
                image = Image.new('RGB', (1920, 1080), check.DESKTOP_BLUE)
                draw = ImageDraw.Draw(image)
                for x in (20, 80, 140):
                    draw.rectangle([int(x * icons_scale), int(20 * icons_scale), int((x + 32) * icons_scale),
                        int(52 * icons_scale)], fill=(230, 200, 60))
                    draw.rectangle([int(x * icons_scale), int(58 * icons_scale), int((x + 32) * icons_scale),
                        int(68 * icons_scale)], fill=(20, 20, 20))
                if deskbar:
                    draw.rectangle([1785, 0, 1919, 108], fill=(216, 216, 216))
                image.save(scaled, format='JPEG', quality=80)
            capture(1.5, False)
            result = check.check_desktop_crop(scaled, 1280, 720)
            self.assertEqual((result['status'], result['scale']), ('pass', 1.5))
            self.assertEqual([s['kind'] for s in result['samples']][-2:], ['icon_reach', 'deskbar_absent'])
            capture(1.5, True)
            with self.assertRaises(check.ValidationError):
                check.check_desktop_crop(scaled, 1280, 720)
            capture(1.0, False)  # the unscaled 1080p desktop is not the 720p crop
            with self.assertRaises(check.ValidationError):
                check.check_desktop_crop(scaled, 1280, 720)
            session = '/mnt/HaikuWork/artifacts/interactive/20260918T094058Z-81f2a1/'
            if os.path.exists(session + 'frame-010.jpg'):
                self.assertEqual(check.check_desktop_crop(session + 'frame-010.jpg', 1280, 720)['scale'], 1.5)
                with self.assertRaises(check.ValidationError):
                    check.check_desktop_crop(session + 'frame-009.jpg', 1280, 720)

    def test_cursor_transcript(self):
        def state(label, width=64, height=64, hot=(0, 0), x=200, y=200, visible=1, control='00000001', start=None,
                address='15400000', mix=check.CURSOR_MIXER_WORDS, saved='no', window=3, mixer=6):
            if start is None:
                placement = check.cursor_placement(x, y, hot[0], hot[1], width, height) if visible else None
                start = placement['start'] if placement else '00000000'
                if placement is None:
                    control = '00000000'
            return ('ROCK5_DISPLAY_CURSOR_%s width=%d height=%d hot=%d,%d x=%d y=%d visible=%d window=%d mixer=%d'
                ' control=%s start=%s address=%s mix=%s saved=%s' % (label, width, height, hot[0], hot[1], x, y, visible,
                window, mixer, control, start, address, ','.join(mix), saved))
        def transcript(action='place', x=200, y=200, before=None, bitmap=None, move=None, show=None, after=None,
                passed=None, flags=31, frame=(1920, 1080), address='15400000'):
            placement = check.cursor_placement(x, y, 0, 0, 64, 64, frame)
            lines = ['ROCK5_DISPLAY_CURSOR_REQUEST_CHECKS_PASS',
                'ROCK5_DISPLAY_CURSOR_ACCELERANT flags=%d width=%d height=%d' % ((flags,) + tuple(frame))]
            if action == 'place':
                lines.append(before if before is not None else state('BEFORE', width=22, height=22, hot=(1, 1), x=960, y=540, address='15400000', saved='no'))
                lines.append(bitmap if bitmap is not None else 'ROCK5_DISPLAY_CURSOR_BITMAP width=64 height=64 hot=0,0 result=0 polls=3')
                lines.append(move if move is not None else 'ROCK5_DISPLAY_CURSOR_MOVE x=%d y=%d result=0 polls=2 start=%s address=%08x'
                    % (x, y, placement['start'], int(address, 16) + placement['offset']))
                lines.append(show if show is not None else 'ROCK5_DISPLAY_CURSOR_SHOW visible=1 result=0 polls=1 control=00000001')
                lines.append(after if after is not None else state('AFTER', x=x, y=y, address='%08x' % (int(address, 16) + placement['offset']), saved='kept'))
                lines.append(passed if passed is not None else 'ROCK5_DISPLAY_CURSOR_PASS action=place x=%d y=%d visible=1 width=64 height=64 polls=6' % (x, y))
            elif action == 'hide':
                lines.append(before if before is not None else state('BEFORE', x=x, y=y, saved='yes'))
                lines.append(move if move is not None else 'ROCK5_DISPLAY_CURSOR_MOVE x=%d y=%d result=0 polls=2 start=00000000 address=%s' % (x, y, address))
                lines.append(show if show is not None else 'ROCK5_DISPLAY_CURSOR_SHOW visible=0 result=0 polls=1 control=00000000')
                lines.append(after if after is not None else state('AFTER', x=x, y=y, visible=0, control='00000000', start='00000000', saved='kept'))
                lines.append(passed if passed is not None else 'ROCK5_DISPLAY_CURSOR_PASS action=hide x=%d y=%d visible=0 width=64 height=64 polls=3' % (x, y))
            else:
                lines.append(before if before is not None else state('BEFORE', x=x, y=y, visible=0, control='00000000', saved='yes'))
                lines.append(bitmap if bitmap is not None else 'ROCK5_DISPLAY_CURSOR_BITMAP width=22 height=22 hot=1,1 result=0 polls=0')
                lines.append(move if move is not None else 'ROCK5_DISPLAY_CURSOR_MOVE x=960 y=540 result=0 polls=0 start=00000000 address=%s' % address)
                lines.append(show if show is not None else 'ROCK5_DISPLAY_CURSOR_SHOW visible=1 result=0 polls=4 control=00000001')
                lines.append(after if after is not None else state('AFTER', width=22, height=22, hot=(1, 1), x=960, y=540, saved='removed'))
                lines.append(passed if passed is not None else 'ROCK5_DISPLAY_CURSOR_PASS action=restore x=960 y=540 visible=1 width=22 height=22 polls=4')
            return '\n'.join(lines) + '\n'
        placed = check.validate_cursor(transcript(), 'place', 200, 200)
        self.assertEqual((placed['base'], placed['after']['start'], placed['polls'], placed['placement']['width']), (0x15400000, '00c800c8', [2, 1, 3], 64))
        saved = placed['before']
        self.assertEqual((saved['width'], saved['x'], saved['y'], saved['visible']), (22, 960, 540, 1))
        # Clipped placements offset the buffer address by the cropped rows and columns.
        left = check.validate_cursor(transcript(x=-32, y=500), 'place', -32, 500, base=0x15400000)
        self.assertEqual((left['after']['start'], left['placement']['offset'], left['placement']['width']), ('01f40000', 128, 32))
        self.assertIsNone(check.validate_cursor(transcript(x=-32, y=500), 'place', -32, 500)['base'])
        right = check.validate_cursor(transcript(x=1888, y=1048), 'place', 1888, 1048, base=0x15400000)
        self.assertEqual((right['after']['start'], right['placement']['height']), ('04180760', 32))
        hidden = check.validate_cursor(transcript('hide', x=1888, y=1048), 'hide')
        self.assertEqual((hidden['after']['visible'], hidden['placement'], hidden['address']), (0, None, 0x15400000))
        restored = check.validate_cursor(transcript('restore', x=1888, y=1048), 'restore', saved=saved)
        self.assertEqual((restored['after']['width'], restored['after']['x'], restored['after']['control']), (22, 960, '00000001'))
        # A saved state without a bitmap (app_server on its software pointer) clears the window's bitmap.
        bare = dict(saved, width=0, height=0, hot=(0, 0), visible=0)
        body = transcript('restore', x=1888, y=1048, bitmap='ROCK5_DISPLAY_CURSOR_BITMAP width=0 height=0 hot=0,0 result=0 polls=311',
            move='ROCK5_DISPLAY_CURSOR_MOVE x=960 y=540 result=0 polls=0 start=00000000 address=15400000',
            show='ROCK5_DISPLAY_CURSOR_SHOW visible=0 result=0 polls=0 control=00000000',
            after=state('AFTER', width=0, height=0, x=960, y=540, visible=0, control='00000000', saved='removed'),
            passed='ROCK5_DISPLAY_CURSOR_PASS action=restore x=960 y=540 visible=0 width=0 height=0 polls=311')
        self.assertEqual(check.validate_cursor(body, 'restore', saved=bare)['after']['width'], 0)
        with self.assertRaises(check.ValidationError):
            check.validate_cursor(body.replace('ROCK5_DISPLAY_CURSOR_BITMAP width=0 height=0 hot=0,0 result=0 polls=311\n', ''), 'restore', saved=bare)
        with self.assertRaises(ValueError):
            check.validate_cursor(transcript(), 'blink')
        with self.assertRaises(ValueError):
            check.validate_cursor(transcript(), 'place')
        with self.assertRaises(ValueError):
            check.validate_cursor(transcript('restore'), 'restore')
        cases = [
            ('checks', transcript().replace('ROCK5_DISPLAY_CURSOR_REQUEST_CHECKS_PASS\n', ''), 'place', dict(x=200, y=200)),
            ('no_cursor_flag', transcript(flags=15), 'place', dict(x=200, y=200)),
            ('frame', transcript(frame=(1280, 720)), 'place', dict(x=200, y=200)),
            ('window', transcript().replace('window=3 mixer=6 control=00000001 start=00c800c8', 'window=2 mixer=6 control=00000001 start=00c800c8'), 'place', dict(x=200, y=200)),
            ('mixer_words', transcript().replace('mix=00ff0125,00ff0060,00000024,00000074 saved=kept', 'mix=00ff0125,00ff0060,00000024,00000075 saved=kept'), 'place', dict(x=200, y=200)),
            ('saved_before', transcript(before=state('BEFORE', width=22, height=22, hot=(1, 1), x=960, y=540, saved='kept')), 'place', dict(x=200, y=200)),
            ('saved_after', transcript().replace('saved=kept', 'saved=removed'), 'place', dict(x=200, y=200)),
            ('bitmap_size', transcript(bitmap='ROCK5_DISPLAY_CURSOR_BITMAP width=32 height=32 hot=0,0 result=0 polls=3'), 'place', dict(x=200, y=200)),
            ('bitmap_result', transcript(bitmap='ROCK5_DISPLAY_CURSOR_BITMAP width=64 height=64 hot=0,0 result=4 polls=3'), 'place', dict(x=200, y=200)),
            ('move_result', transcript(move='ROCK5_DISPLAY_CURSOR_MOVE x=200 y=200 result=3 polls=5000 start=00c800c8 address=15400000'), 'place', dict(x=200, y=200)),
            ('move_target', transcript(move='ROCK5_DISPLAY_CURSOR_MOVE x=201 y=200 result=0 polls=2 start=00c800c8 address=15400000'), 'place', dict(x=200, y=200)),
            ('show_result', transcript(show='ROCK5_DISPLAY_CURSOR_SHOW visible=1 result=4 polls=1 control=00000001'), 'place', dict(x=200, y=200)),
            ('show_control', transcript(show='ROCK5_DISPLAY_CURSOR_SHOW visible=1 result=0 polls=1 control=00000000'), 'place', dict(x=200, y=200)),
            ('after_position', transcript(after=state('AFTER', x=200, y=201, saved='kept')), 'place', dict(x=200, y=200)),
            ('after_control', transcript(after=state('AFTER', control='00000000', start='00c800c8', saved='kept')), 'place', dict(x=200, y=200)),
            ('after_start', transcript(after=state('AFTER', start='00c800c9', saved='kept')), 'place', dict(x=200, y=200)),
            ('address_page', transcript(after=state('AFTER', address='15400010', saved='kept'), move='ROCK5_DISPLAY_CURSOR_MOVE x=200 y=200 result=0 polls=2 start=00c800c8 address=15400010'), 'place', dict(x=200, y=200)),
            ('address_base', transcript(x=-32, y=500), 'place', dict(x=-32, y=500, base=0x15400100)),
            ('address_mismatch', transcript(x=-32, y=500).replace('address=15400080 mix', 'address=15400084 mix'), 'place', dict(x=-32, y=500, base=0x15400000)),
            ('polls', transcript(bitmap='ROCK5_DISPLAY_CURSOR_BITMAP width=64 height=64 hot=0,0 result=0 polls=5001',
                passed='ROCK5_DISPLAY_CURSOR_PASS action=place x=200 y=200 visible=1 width=64 height=64 polls=5004'), 'place', dict(x=200, y=200)),
            ('summary_polls', transcript(passed='ROCK5_DISPLAY_CURSOR_PASS action=place x=200 y=200 visible=1 width=64 height=64 polls=7'), 'place', dict(x=200, y=200)),
            ('summary_action', transcript(passed='ROCK5_DISPLAY_CURSOR_PASS action=hide x=200 y=200 visible=1 width=64 height=64 polls=6'), 'place', dict(x=200, y=200)),
            ('summary_missing', transcript(passed=''), 'place', dict(x=200, y=200)),
            ('hide_visible', transcript('hide', x=1888, y=1048, after=state('AFTER', x=1888, y=1048, saved='kept')), 'hide', {}),
            ('hide_control', transcript('hide', x=1888, y=1048, after=state('AFTER', x=1888, y=1048, visible=0, control='00000001', start='00000000', saved='kept')), 'hide', {}),
            ('hide_bitmap', transcript('hide', x=1888, y=1048, move='ROCK5_DISPLAY_CURSOR_BITMAP width=64 height=64 hot=0,0 result=0 polls=3\nROCK5_DISPLAY_CURSOR_MOVE x=1888 y=1048 result=0 polls=2 start=00000000 address=15400000'), 'hide', {}),
            ('hide_moved', transcript('hide', x=1888, y=1048, move='ROCK5_DISPLAY_CURSOR_MOVE x=1888 y=1000 result=0 polls=2 start=00000000 address=15400000'), 'hide', {}),
            ('restore_bitmap', transcript('restore', x=1888, y=1048, bitmap='ROCK5_DISPLAY_CURSOR_BITMAP width=24 height=22 hot=1,1 result=0 polls=0'), 'restore', dict(saved=saved)),
            ('restore_hot', transcript('restore', x=1888, y=1048, bitmap='ROCK5_DISPLAY_CURSOR_BITMAP width=22 height=22 hot=2,1 result=0 polls=0'), 'restore', dict(saved=saved)),
            ('restore_position', transcript('restore', x=1888, y=1048, move='ROCK5_DISPLAY_CURSOR_MOVE x=960 y=541 result=0 polls=0 start=00000000 address=15400000'), 'restore', dict(saved=saved)),
            ('restore_saved_before', transcript('restore', x=1888, y=1048, before=state('BEFORE', x=1888, y=1048, visible=0, control='00000000', saved='no')), 'restore', dict(saved=saved)),
            ('restore_saved_after', transcript('restore', x=1888, y=1048).replace('saved=removed', 'saved=kept'), 'restore', dict(saved=saved)),
            ('restore_after', transcript('restore', x=1888, y=1048, after=state('AFTER', width=22, height=22, hot=(1, 1), x=960, y=540, visible=0, control='00000000', start='00000000', saved='removed')), 'restore', dict(saved=saved)),
        ]
        for name, body, action, keywords in cases:
            with self.subTest(name):
                with self.assertRaises(check.ValidationError):
                    check.validate_cursor(body, action, **keywords)

    def test_cursor_frame(self):
        import tempfile
        from PIL import Image, ImageDraw
        half = tuple(d + (255 - d) * 128 // 255 for d in check.DESKTOP_BLUE)
        def capture(path, x, y, shown=True, quadrants=None):
            image = Image.new('RGB', (1920, 1080), check.DESKTOP_BLUE)
            draw = ImageDraw.Draw(image)
            draw.rectangle([1784, 0, 1919, 70], fill=(200, 200, 200))
            if shown:
                colours = quadrants or ((255, 255, 255), (0, 0, 0), check.DESKTOP_BLUE, half)
                for (dx, dy), colour in zip(((0, 0), (32, 0), (0, 32), (32, 32)), colours):
                    draw.rectangle([x + dx, y + dy, x + dx + 31, y + dy + 31], fill=colour)
            image.save(path, format='JPEG', quality=80)
        with tempfile.TemporaryDirectory() as directory:
            frame = directory + '/cursor.jpg'
            capture(frame, 200, 200)
            result = check.check_cursor_frame(frame, 200, 200)
            self.assertEqual([s['kind'] for s in result['samples']], ['white', 'black', 'transparent', 'half', 'outside', 'outside', 'outside', 'outside'])
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 200, 200, shown=False)
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 300, 200)
            capture(frame, -32, 500)
            result = check.check_cursor_frame(frame, -32, 500)
            self.assertEqual([s['kind'] for s in result['samples']], ['black', 'half', 'outside', 'outside', 'outside'])
            capture(frame, 1888, 1048)
            self.assertEqual([s['kind'] for s in check.check_cursor_frame(frame, 1888, 1048)['samples']], ['white'])
            capture(frame, 1888, 1048, shown=False)
            self.assertEqual(check.check_cursor_frame(frame, 1888, 1048, shown=False)['samples'][0]['kind'], 'hidden_white')
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 1888, 1048)
            # An opaque bottom-left quadrant means the alpha was ignored; an unblended bottom-right likewise.
            capture(frame, 200, 200, quadrants=((255, 255, 255), (0, 0, 0), (255, 0, 0), half))
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 200, 200)
            capture(frame, 200, 200, quadrants=((255, 255, 255), (0, 0, 0), check.DESKTOP_BLUE, (255, 255, 255)))
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 200, 200)
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 3000, 3000)
            # With a reference capture the desktop may carry windows: the
            # transparent quadrant and the surroundings must match it, the
            # half quadrant its blend with white.
            reference = directory + '/reference.jpg'
            grey = (202, 202, 202)
            def windowed(path, x, y, shown=True, quadrants=None):
                image = Image.new('RGB', (1920, 1080), check.DESKTOP_BLUE)
                draw = ImageDraw.Draw(image)
                draw.rectangle([120, 80, 590, 320], fill=grey)
                draw.rectangle([1784, 0, 1919, 70], fill=(200, 200, 200))
                if shown:
                    half_grey = tuple(d + (255 - d) * 128 // 255 for d in grey)
                    colours = quadrants or ((255, 255, 255), (0, 0, 0), grey, half_grey)
                    for (dx, dy), colour in zip(((0, 0), (32, 0), (0, 32), (32, 32)), colours):
                        draw.rectangle([x + dx, y + dy, x + dx + 31, y + dy + 31], fill=colour)
                image.save(path, format='JPEG', quality=80)
            windowed(reference, 200, 200, shown=False)
            windowed(frame, 200, 200)
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 200, 200)
            result = check.check_cursor_frame(frame, 200, 200, reference=reference)
            self.assertEqual((result['status'], result['reference'], result['samples'][2]['expected']), ('pass', reference, list(grey)))
            windowed(frame, 200, 200, quadrants=((255, 255, 255), (0, 0, 0), check.DESKTOP_BLUE, half))
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 200, 200, reference=reference)
            windowed(frame, 200, 200, shown=False)
            self.assertEqual(check.check_cursor_frame(frame, 200, 200, shown=False, reference=reference)['status'], 'pass')
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 200, 200, shown=False)
            Image.new('RGB', (1280, 720), check.DESKTOP_BLUE).save(reference, format='JPEG')
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 200, 200, shown=False, reference=reference)
            Image.new('RGB', (1280, 720), check.DESKTOP_BLUE).save(frame, format='JPEG')
            with self.assertRaises(check.ValidationError):
                check.check_cursor_frame(frame, 200, 200)

    def test_pointer_frame(self):
        import tempfile
        from PIL import Image, ImageDraw
        with tempfile.TemporaryDirectory() as directory:
            frame = directory + '/pointer.jpg'
            def capture(pointer=True, x=958, y=538):
                image = Image.new('RGB', (1920, 1080), check.DESKTOP_BLUE)
                draw = ImageDraw.Draw(image)
                draw.rectangle([1784, 0, 1919, 70], fill=(200, 200, 200))
                if pointer:
                    draw.polygon([(x, y), (x, y + 18), (x + 5, y + 13), (x + 13, y + 13)], fill=(255, 255, 255), outline=(0, 0, 0), width=2)
                image.save(frame, format='JPEG', quality=80)
            capture()
            result = check.check_pointer_frame(frame, 958, 538)
            self.assertEqual((result['status'], len(result['around'])), ('pass', 4))
            self.assertGreaterEqual(result['light'], 20)
            with self.assertRaises(check.ValidationError):
                check.check_pointer_frame(frame, 500, 500)
            capture(pointer=False)
            with self.assertRaises(check.ValidationError):
                check.check_pointer_frame(frame, 958, 538)
            real = '/mnt/HaikuWork/artifacts/interactive/20260918T233200Z-0ced8a/frame-007.jpg'
            if os.path.exists(real):
                # The +294 software pointer at the centre of the desktop, judged the same way.
                self.assertEqual(check.check_pointer_frame(real, 958, 538)['status'], 'pass')
                with self.assertRaises(check.ValidationError):
                    check.check_pointer_frame(real, 300, 300)
            Image.new('RGB', (1280, 720), check.DESKTOP_BLUE).save(frame, format='JPEG')
            with self.assertRaises(check.ValidationError):
                check.check_pointer_frame(frame, 958, 538)

    def test_dp_path_decode(self):
        decoded = check.validate(transcript())
        self.assertEqual((decoded['dp_read'], decoded['dp_aux_read'], decoded['gpio_read']), (True, True, True))
        self.assertEqual(decoded['gates'], dict(vop=0, hdmi_pclk=0, pclk_dp1=0, dp_aux=0, dp_hdcp=0, usbdp_pclk=0, usbdp_immortal=0, gpio3=0))
        self.assertEqual(decoded['dp_pin'], dict(mux=5, lane_mux_word=0, level=0, output=0))
        self.assertEqual((decoded['dp1']['version'], decoded['dp1']['phyif_powerdown'], decoded['dp1']['hpd_level'], decoded['dp1']['hpd_state']), (0x14110600, 3, 0, 0))
        self.assertEqual(decoded['gpio3']['version'], 0x0101157c)
        plugged = check.validate(transcript(hpd_level=1, dp_hpd=1))
        self.assertEqual((plugged['dp_pin']['level'], plugged['dp1']['hpd_level']), (1, 1))
        # Gated AUX clock: the block is read, its AUX words are not.
        no_aux = check.validate(transcript(aux_gated=True))
        self.assertEqual((no_aux['dp_read'], no_aux['dp_aux_read'], 'hpd_level' in no_aux['dp1']), (True, False, False))
        # Gated APB clock or VO0 off: no DisplayPort block; gated GPIO3 clock: no pin level.
        for body in (transcript(dp_gated=True), transcript(vo0_on=False)):
            skipped = check.validate(body)
            self.assertEqual((skipped['dp_read'], skipped['dp_aux_read'], 'dp1' in skipped), (False, False, False))
        no_gpio = check.validate(transcript(gpio_gated=True))
        self.assertEqual((no_gpio['gpio_read'], 'gpio3' in no_gpio, 'level' in no_gpio['dp_pin']), (False, False, False))
        cases = [
            ('dp_flag_without_power', transcript(vo0_on=False, dp=True)),
            ('dp_flag_without_clock', transcript(dp_gated=True, dp=True)),
            ('dp_skipped_although_readable', transcript(dp=False)),
            ('aux_without_dp', transcript(dp=False, aux=True)),
            ('aux_flag_while_gated', transcript(aux_gated=True, aux=True)),
            ('gpio_flag_while_gated', transcript(gpio_gated=True, gpio=True)),
            ('summary_dp', transcript().replace('dp_read=1 gpio_read=1', 'dp_read=0 gpio_read=1')),
            ('summary_gpio', transcript().replace('dp_read=1 gpio_read=1', 'dp_read=1 gpio_read=0')),
            ('old_version', transcript().replace('version=2 flags', 'version=1 flags')),
            ('dp1_missing', '\n'.join(l for l in transcript().splitlines() if not l.startswith('ROCK5_DISPLAY_DP1 ')) + '\n'),
            ('dp1_unstable', transcript().replace('ROCK5_DISPLAY_DP1 sample=2 14110600', 'ROCK5_DISPLAY_DP1 sample=2 14110601')),
            ('resources_dp', transcript().replace('dp=0xfde60000/0x4000', 'dp=0xfdec0000/0x4000')),
            ('gate_count', transcript().replace('ROCK5_DISPLAY_CRU_GATE sample=0 00000000,00008000,00000000,00000000,00000000,00000000,00000000,00000000,00000000',
                'ROCK5_DISPLAY_CRU_GATE sample=0 00000000,00008000,00000000,00000000,00000000,00000000')),
        ]
        for name, body in cases:
            with self.subTest(name):
                with self.assertRaises(check.ValidationError):
                    check.validate(body)

    def test_dp_probe_transcript(self):
        base, _ = edid_blocks()
        dpcd = bytes([0x12, 0x0a, 0x82, 0x01, 0x00, 0x15, 0x01, 0x81, 0x02, 0x00, 0x06, 0, 0, 0, 0, 0])
        def transcript(result=0, phase=7, pin_after='00000050', hpd_after='00000f02', refclk='00000000',
                resets_after='00000000,00000000,00000000,00000000', usbdp_after='00006000', vo0_after='00000040',
                cctl_after='00000000', pma_after='000000cc,00000018,00000000,000000c0,00000003,00000008,00000000',
                dpcd_bytes=dpcd, edid=base, summary=None, count=16):
            lines = ['ROCK5_DISPLAY_DP_REQUEST_CHECKS_PASS',
                'ROCK5_DISPLAY_DP result=%d phase=%d pin=00000000,%s level=1 hpd=00000000,%s hpd_polls=17 refclk=%s'
                ' lcpll_polls=0 aux=12,0,40 aux_status=00000000 dpcd_count=%d sinks=41 edid_bytes=%d micros=41000'
                % (result, phase, pin_after, hpd_after, refclk, count, 128 if edid else 0),
                'ROCK5_DISPLAY_DP_WORDS resets=00000000,00000000,00000000,00000000/%s usbdp_grf=00006000,%s'
                ' vo0_grf=000000e4,%s cctl=00000004,%s pma_before=00000000,00000000,00000000,000000c0,00000000,00000000,00000000'
                ' pma_after=%s' % (resets_after, usbdp_after, vo0_after, cctl_after, pma_after),
                'ROCK5_DISPLAY_DP_DPCD ' + dpcd_bytes.hex()]
            if edid:
                lines.append('ROCK5_DISPLAY_DP_EDID ' + edid.hex())
            lines.append(summary if summary is not None else 'ROCK5_DISPLAY_DP_PASS edid=%d' % (1 if edid else 0))
            return '\n'.join(lines) + '\n'
        result = check.validate_dp_probe(transcript(), edid=True)
        self.assertEqual(result['dpcd_decoded'], dict(revision='1.2', max_link_rate_gbps=2.7, max_link_rate_code=0x0a,
            max_lanes=2, enhanced_framing=1, tps3=0, max_downspread=1, downstream_port_present=1, downstream_type=2,
            downstream_ports=1, training_interval=0))
        self.assertEqual((result['sink_count'], result['aux_transfers'], result['edid_base']['manufacturer']), (0x41, 12, check.decode_edid_base(base)['manufacturer']))
        plain = check.validate_dp_probe(transcript(phase=6, edid=None))
        self.assertIsNone(plain['edid'])
        broken = bytearray(base)
        broken[20] ^= 1
        cases = [
            ('result', transcript(result=5, phase=6), True),
            ('phase', transcript(phase=6), True),
            ('pin', transcript(pin_after='00000000'), True),
            ('hpd', transcript(hpd_after='00000000'), True),
            ('refclk', transcript(refclk='00000080'), True),
            ('reset', transcript(resets_after='00008000,00000000,00000000,00000000'), True),
            ('pma_reset', transcript(resets_after='00000000,00000000,00000000,00000010'), True),
            ('grf', transcript(usbdp_after='00004000'), True),
            ('lanes', transcript(vo0_after='000000e4'), True),
            ('cctl', transcript(cctl_after='00000004'), True),
            ('pma', transcript(pma_after='000000c0,00000018,00000000,000000c0,00000003,00000008,00000000'), True),
            ('lcpll', transcript(pma_after='000000cc,00000018,00000000,00000000,00000003,00000008,00000000'), True),
            ('dpcd', transcript(dpcd_bytes=bytes(16)), True),
            ('dpcd_count', transcript(count=0), True),
            ('edid_missing', transcript(edid=None), True),
            ('edid_checksum', transcript(edid=bytes(broken)), True),
            ('summary', transcript(summary='ROCK5_DISPLAY_DP_PASS edid=0'), True),
            ('checks', transcript().replace('ROCK5_DISPLAY_DP_REQUEST_CHECKS_PASS\n', ''), True),
        ]
        for name, body, with_edid in cases:
            with self.subTest(name):
                with self.assertRaises(check.ValidationError):
                    check.validate_dp_probe(body, edid=with_edid)

    def test_pattern_frame(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            shown = directory + '/pattern.jpg'
            desktop = directory + '/desktop.jpg'
            pattern_image(shown)
            pattern_image(desktop, blank=True)
            result = check.check_pattern_frame(shown)
            self.assertTrue(result['pattern_visible'])
            self.assertEqual(result['mismatches'], 0)
            self.assertEqual(len(result['samples']), 28)
            self.assertFalse(check.check_pattern_frame(desktop, expect_pattern=False)['pattern_visible'])
            with self.assertRaises(check.ValidationError):
                check.check_pattern_frame(desktop)
            with self.assertRaises(check.ValidationError):
                check.check_pattern_frame(shown, expect_pattern=False)
            pattern_image(shown, quality=30)
            self.assertTrue(check.check_pattern_frame(shown)['pattern_visible'])

    def test_edid_decode(self):
        result = check.validate_edid(edid_transcript())
        self.assertEqual(result['base']['manufacturer'], 'PNL')
        self.assertEqual(result['base']['product'], 0x1234)
        self.assertEqual((result['base']['width'], result['base']['height']), (1920, 1080))
        self.assertEqual(result['base']['pixel_khz'], 148500)
        self.assertAlmostEqual(result['preferred_refresh_hz'], 60.0, places=1)
        self.assertEqual(sorted(result['blocks']), ['0', '1'])

    def test_native_edid_transcript(self):
        # Actual transcript from the first native +264 EDID read (2026-09-18).
        from pathlib import Path
        path = Path('/mnt/HaikuWork/artifacts/interactive/20260918T045402Z-11d04a/shell-20260918T045748Z-d3fca7.txt')
        if not path.exists():
            self.skipTest('native transcript not present on this host')
        body = path.read_text()
        edid_body = body.split('ROCK5_DISPLAY_INVENTORY_EXIT=0\n', 1)[1].split('ROCK5_DISPLAY_EDID_EXIT=0\n', 1)[0]
        result = check.validate_edid(edid_body)
        self.assertEqual(result['base']['manufacturer'], 'VCS')
        self.assertEqual((result['base']['width'], result['base']['height'], result['base']['pixel_khz']), (1920, 1080, 148500))
        self.assertEqual(sorted(result['blocks']), ['0', '1'])

    def test_edid_rejections(self):
        base, ext = edid_blocks()
        broken = bytearray(base); broken[127] ^= 1
        bad_header = bytearray(base); bad_header[0] = 1
        cases = [
            ('checksum', edid_transcript(blocks=[bytes(broken), ext])),
            ('header', edid_transcript(blocks=[bytes(bad_header), ext], info='ROCK5_DISPLAY_EDID_INFO manufacturer=PNL')),
            ('missing_extension', edid_transcript(blocks=[base])),
            ('info', edid_transcript().replace('preferred=1920x1080', 'preferred=1280x720')),
            ('busy', edid_transcript().replace('control=00000a00/00000a00', 'control=00000a00/00000a04')),
            ('hpd', edid_transcript().replace('hpd=09000000', 'hpd=08000000')),
            ('summary', edid_transcript(summary='ROCK5_DISPLAY_EDID_PASS blocks=2 extensions=1 polls=1 register_writes=i2c_master_only')),
            ('checks', edid_transcript().replace('ROCK5_DISPLAY_EDID_REQUEST_CHECKS_PASS\n', '')),
            ('flags', edid_transcript().replace('block=1 result=0 flags=0 ', 'block=1 result=0 flags=0x2 ')),
        ]
        for name, body in cases:
            with self.subTest(name=name):
                with self.assertRaises(check.ValidationError):
                    check.validate_edid(body)

    def test_decodes_live_port(self):
        result = check.validate(transcript())
        self.assertEqual(result['active_ports'], [1])
        self.assertEqual(result['ports'][1]['width'], 1920)
        self.assertEqual(result['ports'][1]['height'], 1080)
        self.assertEqual(result['interfaces']['hdmi1'], 1)
        self.assertEqual(result['interface_muxes']['hdmi_edp1'], 1)
        self.assertEqual(result['hpd']['hdmi1_level'], 1)
        self.assertEqual(result['hdptx1']['pll_lock'], 1)
        self.assertEqual(result['windows']['esmarts'][1]['address'], 0xed3a0000)
        self.assertTrue(result['vop_read'] and result['hdmi_read'])

    def test_volatile_status_bits_are_tolerated(self):
        result = check.validate(transcript(status_jitter=0x10))
        self.assertEqual(result['hpd']['hdmi1_level'], 1)
        with self.assertRaises(check.ValidationError):
            check.validate(transcript(status_jitter=0x10000))

    def test_skipped_blocks_match_power_words(self):
        result = check.validate(transcript(vop=False, vop_on=False))
        self.assertFalse(result['vop_read'])
        self.assertNotIn('ports', result)
        result = check.validate(transcript(hdmi=False, hdmi_gated=True))
        # DPMS off: the PHY PLL is off, so the driver skips the HDMI block.
        powered_off = check.validate(transcript(hdmi=False, pll=False))
        self.assertFalse(powered_off['hdmi_read'])
        self.assertEqual((powered_off['hdptx_grf'][0], powered_off['hdptx1']['pll_lock']), (0, 0))
        self.assertFalse(result['hdmi_read'])
        self.assertNotIn('hdmi1', result)

    def test_rejections(self):
        cases = [
            ('open', transcript().replace('ROCK5_DISPLAY_WRITE_OPEN_REJECTED\n', '')),
            ('description', transcript().replace('ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS\n', '')),
            ('board', transcript().replace('board=radxa,rock-5-itx', 'board=radxa,rock-5b')),
            ('summary', transcript().replace('register_writes=0', 'register_writes=1')),
            ('samples', transcript(samples=2)),
            ('inconsistent', transcript(consistent=False)),
            ('flags', transcript(flags=1 | 2 | 8 | 4)),
            ('gating', transcript(vop=True, vop_on=False)),
            ('hdmi_gating', transcript(hdmi=True, hdmi_gated=True)),
            ('hdmi_phy_off', transcript(hdmi=True, pll=False)),
            ('summary_flags', transcript().replace('vop_read=1 hdmi_read=1', 'vop_read=0 hdmi_read=1')),
            ('timing', transcript().replace('width=1920 height=1080', 'width=1280 height=720')),
            ('duplicate', transcript() + 'ROCK5_DISPLAY_PMU sample=0 ' + words([0] * 11) + '\n'),
            ('missing_block', transcript().replace('ROCK5_DISPLAY_HDMI1 sample=2 ', 'ROCK5_DISPLAY_HDMIX sample=2 ')),
        ]
        for name, body in cases:
            with self.subTest(name=name):
                with self.assertRaises(check.ValidationError):
                    check.validate(body)


if __name__ == '__main__':
    unittest.main()
