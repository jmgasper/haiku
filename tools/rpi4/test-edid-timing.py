#!/usr/bin/env python3
"""Compile and exercise the production HDMI EDID decoder under sanitizers."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
#include "edid_timing.h"
#include <assert.h>

static void checksum(uint8_t* block) {
    block[127] = 0;
    for (unsigned i = 0; i < 127; i++) block[127] -= block[i];
}

int main() {
    uint8_t edid[256] = {0, 255, 255, 255, 255, 255, 255, 0};
    // 1920x1080p60 DTD read from both physical lab monitors.
    const uint8_t dtd[18] = {0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,
        0x58,0x2c,0x45,0x00,0x0f,0x28,0x21,0,0,0x1e};
    memcpy(edid + 54, dtd, 18);
    checksum(edid);
    firmware_timing t;
    assert(preferred_edid_timing(edid, 128, t));
    assert(t.clock == 148500 && t.hdisplay == 1920 && t.vdisplay == 1080);
    assert(t.hsync_start == 2008 && t.hsync_end == 2052 && t.htotal == 2200);
    assert(t.vsync_start == 1084 && t.vsync_end == 1089 && t.vtotal == 1125);
    assert(t.vrefresh == 60 && t.flags == 515); // positive sync + DVI
    assert(!preferred_edid_timing(edid, 127, t));
    edid[20] ^= 1;
    assert(!preferred_edid_timing(edid, 128, t));
    edid[20] ^= 1;
    edid[126] = 1;
    checksum(edid);
    uint8_t* cea = edid + 128;
    cea[0] = 2; cea[1] = 3; cea[2] = 8;
    cea[4] = 0x63; cea[5] = 3; cea[6] = 12; cea[7] = 0;
    checksum(cea);
    assert(preferred_edid_timing(edid, 256, t) && t.flags == 3);
    // An interlaced preferred DTD is rejected; a supported CTA DTD is used.
    edid[71] |= 128; checksum(edid);
    assert(!preferred_edid_timing(edid, 128, t));
    memcpy(cea + 8, dtd, 18); checksum(cea);
    assert(preferred_edid_timing(edid, 256, t) && t.hdisplay == 1920);
    memset(cea + 8, 0, 18); checksum(cea);
    edid[35] = 0x20; checksum(edid);
    assert(preferred_edid_timing(edid, 256, t) && t.hdisplay == 640);
    // Malformed descriptors and CTA offsets must never read outside EDID.
    uint32_t random = 7;
    for (unsigned n = 0; n < 100000; n++) {
        for (unsigned i = 8; i < sizeof(edid); i++) {
            random = random * 1664525u + 1013904223u;
            edid[i] = random >> 24;
        }
        checksum(edid); checksum(cea);
        if (preferred_edid_timing(edid, 256, t)) {
            assert(t.hdisplay >= 320 && t.hdisplay <= 4096);
            assert(t.vdisplay >= 200 && t.vdisplay <= 2160);
            assert(t.hdisplay < t.hsync_start && t.hsync_start < t.hsync_end);
            assert(t.hsync_end <= t.htotal && t.vsync_end <= t.vtotal);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix="rpi-edid-", dir=os.environ.get(
        "TMPDIR", "/mnt/HaikuWork/tmp")) as work:
    path = Path(work)
    (path / "test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(root / "src/add-ons/kernel/drivers/graphics/rpi_display"),
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("HDMI EDID timing tests passed (including 100,000 malformed EDIDs)")
