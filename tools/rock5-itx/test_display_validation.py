"""Check the display observation decoder against synthetic probe transcripts."""
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
        consistent=True, flags=None):
    lines = ['ROCK5_DISPLAY_WRITE_OPEN_REJECTED',
        'ROCK5_DISPLAY_RESOURCES version=1 flags=1 vop=0xfdd90000/0x4200 lut=0xfdd95000/0x1000'
        ' hdmi=0xfdea0000/0x20000 hdptx=0xfed70000/0x2000 hdptx_grf=0xfd5e4000/0x100'
        ' sys_grf=0xfd58c000/0x1000 vop_grf=0xfd5a4000/0x2000 vo1_grf=0xfd5a8000/0x4000'
        ' pmu=0xfd8d8000/0x400 cru=0xfd7c0000/0x5c000 gic=0xfe600000 vop_irq=188'
        ' hdmi_irqs=205,206,207,208,393 vop_clocks=605,604,609,610,611,612,603'
        ' hdmi_clocks=531,532,533,569,595,717 vop_pd=24 hdmi_pd=26 phy_phandle=108 vop_port=1'
        ' board=radxa,rock-5-itx',
        'ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS']
    if flags is None:
        flags = 1 | (2 if vop else 8) | (4 if hdmi else 16)
    repair = (1 << 16 if vop_on else 0) | (1 << 18 if vo1_on else 0) | (1 << 17)
    pmu = [0x1000, 0, 0x1000, 0, 0x1000, 0, 0, 0x0fff0 & ~((vop_on << 1) | (vo1_on << 3)), 0, 0x0e000000, repair]
    gate = [0, 0x8000, 0, 0x4 if hdmi_gated else 0, 0, 0]
    status1 = (1 << 27) | (1 << 24)
    ports = [port_words(0, 0, standby=1), port_words(1920, 1080), port_words(0, 0, standby=1),
        port_words(0, 0, standby=1)]
    for index in range(samples):
        lines.append('ROCK5_DISPLAY_SNAPSHOT sample=%d version=1 flags=%#x start_us=%d end_us=%d'
            % (index, flags, 1000 + index * 10, 1050 + index * 10))
        lines.append('ROCK5_DISPLAY_PMU sample=%d %s' % (index, words(pmu)))
        lines.append('ROCK5_DISPLAY_CRU_SELECT sample=%d %s' % (index, words([0x1234, 0x0a00, 0, 0])))
        lines.append('ROCK5_DISPLAY_CRU_GATE sample=%d %s' % (index, words(gate)))
        lines.append('ROCK5_DISPLAY_SYS_GRF sample=%d %s' % (index, words([0, 0x3000, status1])))
        lines.append('ROCK5_DISPLAY_VOP_GRF sample=%d %s' % (index, words([0x2])))
        lines.append('ROCK5_DISPLAY_VO1_GRF sample=%d %s' % (index, words([0x0, 0x0])))
        lines.append('ROCK5_DISPLAY_HDPTX1_GRF sample=%d %s' % (index, words([0xe0, 0xf])))
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
            lines.append('ROCK5_DISPLAY_HDMI1 sample=%d %s' % (index, words(list(range(14)))))
        lines.append('ROCK5_DISPLAY_HPD sample=%d hdmi0_level=0 hdmi0_int=0 hdmi1_level=1 hdmi1_int=1'
            ' vop_on=%d vo0_on=1 vo1_on=%d vop_gates=0x0 hdmi_gates=%#x hdptx1_status=0xf'
            % (index, vop_on, vo1_on, 4 if hdmi_gated else 0))
    lines.append('ROCK5_DISPLAY_OBSERVATION_PASS samples=%d register_writes=0 consistent=1'
        ' vop_read=%d hdmi_read=%d' % (samples, vop, hdmi))
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
        lines.append('ROCK5_DISPLAY_EDID block=%d result=0 flags=%#x bytes=128 polls=%d control=00000a00/00000a00'
            ' status=00000000/00000000 hpd=09000000 start_us=%d end_us=%d hex=%s'
            % (index, 1 if index >= 2 else 0, 300 + index, 1000 + index * 100, 1050 + index * 100, data.hex()))
    try:
        decoded = check.decode_edid_base(blocks[0])
    except check.ValidationError:
        decoded = None
    if info is None and decoded is not None:
        info = ('ROCK5_DISPLAY_EDID_INFO manufacturer=%s product=%#x serial=%#x week=%d year=%d version=%s'
            ' extensions=%d digital=%d preferred=%dx%d pixel_khz=%d hblank=%d vblank=%d hsync_offset=%d'
            ' hsync_width=%d vsync_offset=%d vsync_width=%d flags=%#x size_mm=%dx%d checksum=ok'
            % (decoded['manufacturer'], decoded['product'], decoded['serial'], decoded['week'], decoded['year'],
            decoded['version'], decoded['extensions'], decoded['digital'], decoded['width'], decoded['height'],
            decoded['pixel_khz'], decoded['hblank'], decoded['vblank'], decoded['hsync_offset'],
            decoded['hsync_width'], decoded['vsync_offset'], decoded['vsync_width'], decoded['flags'],
            decoded['width_mm'], decoded['height_mm']))
    if info:
        lines.append(info)
    for index, data in enumerate(blocks[1:], 1):
        lines.append('ROCK5_DISPLAY_EDID_EXTENSION block=%d tag=%#x revision=%d checksum=ok' % (index, data[0], data[1]))
    lines.append(summary or 'ROCK5_DISPLAY_EDID_PASS blocks=%d extensions=%d polls=%d register_writes=i2c_master_only'
        % (len(blocks), blocks[0][126], sum(300 + i for i in range(len(blocks)))))
    return '\n'.join(lines) + '\n'


class DisplayValidationTest(unittest.TestCase):
    def test_edid_decode(self):
        result = check.validate_edid(edid_transcript())
        self.assertEqual(result['base']['manufacturer'], 'PNL')
        self.assertEqual(result['base']['product'], 0x1234)
        self.assertEqual((result['base']['width'], result['base']['height']), (1920, 1080))
        self.assertEqual(result['base']['pixel_khz'], 148500)
        self.assertAlmostEqual(result['preferred_refresh_hz'], 60.0, places=1)
        self.assertEqual(sorted(result['blocks']), [0, 1])

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
            ('flags', edid_transcript().replace('block=1 result=0 flags=0x0', 'block=1 result=0 flags=0x2')),
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

    def test_skipped_blocks_match_power_words(self):
        result = check.validate(transcript(vop=False, vop_on=False))
        self.assertFalse(result['vop_read'])
        self.assertNotIn('ports', result)
        result = check.validate(transcript(hdmi=False, hdmi_gated=True))
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
