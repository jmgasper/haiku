/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RPI_EDID_TIMING_H
#define RPI_EDID_TIMING_H

#include <stdint.h>
#include <string.h>

// Raspberry Pi firmware SET_TIMING (the Linux vc4 firmware KMS ABI).
struct firmware_timing {
	uint8_t display, padding;
	uint16_t video_id_code;
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint16_t vrefresh, padding2;
	uint32_t flags;
};
static_assert(sizeof(firmware_timing) == 36, "firmware timing ABI");

static inline bool
valid_edid_block(const uint8_t* data, bool base)
{
	static const uint8_t header[] = {0, 255, 255, 255, 255, 255, 255, 0};
	if (base && memcmp(data, header, sizeof(header)) != 0)
		return false;
	uint8_t sum = 0;
	for (unsigned i = 0; i < 128; i++)
		sum += data[i];
	return sum == 0;
}

static inline bool
decode_detailed_timing(const uint8_t* d, firmware_timing& timing)
{
	firmware_timing t = {};
	t.clock = (d[0] | (d[1] << 8)) * 10;
	// Progressive digital separate sync, within the Pi 4's normal TMDS limit.
	if (t.clock == 0 || t.clock > 300000 || (d[17] & 0x98) != 0x18)
		return false;
	t.hdisplay = d[2] | ((d[4] & 0xf0) << 4);
	t.htotal = t.hdisplay + (d[3] | ((d[4] & 15) << 8));
	t.vdisplay = d[5] | ((d[7] & 0xf0) << 4);
	t.vtotal = t.vdisplay + (d[6] | ((d[7] & 15) << 8));
	t.hsync_start = t.hdisplay + (d[8] | ((d[11] & 0xc0) << 2));
	t.hsync_end = t.hsync_start + (d[9] | ((d[11] & 0x30) << 4));
	t.vsync_start = t.vdisplay + ((d[10] >> 4) | ((d[11] & 0x0c) << 2));
	t.vsync_end = t.vsync_start + ((d[10] & 15) | ((d[11] & 3) << 4));
	if (t.hdisplay < 320 || t.vdisplay < 200 || t.hdisplay > 4096
		|| t.vdisplay > 2160 || t.hsync_start <= t.hdisplay
		|| t.hsync_end <= t.hsync_start || t.hsync_end > t.htotal
		|| t.vsync_start <= t.vdisplay || t.vsync_end <= t.vsync_start
		|| t.vsync_end > t.vtotal)
		return false;
	t.flags = ((d[17] & 2) ? 1 : 0) | ((d[17] & 4) ? 2 : 0);
	t.vrefresh = (t.clock * 1000u + t.htotal * t.vtotal / 2)
		/ (t.htotal * t.vtotal);
	if (t.vrefresh < 23 || t.vrefresh > 240)
		return false;
	timing = t;
	return true;
}

static inline bool
preferred_edid_timing(const uint8_t* edid, unsigned length,
	firmware_timing& timing)
{
	if (length < 128 || !valid_edid_block(edid, true))
		return false;
	bool found = false;
	for (unsigned offset = 54; offset + 18 <= 126; offset += 18) {
		if (decode_detailed_timing(edid + offset, timing)) {
			found = true;
			break;
		}
	}
	bool hdmi = false;
	if (length >= 256 && edid[126] != 0
		&& valid_edid_block(edid + 128, false) && edid[128] == 2) {
		const uint8_t* cea = edid + 128;
		unsigned end = cea[2];
		if (end >= 4 && end <= 127) {
			for (unsigned i = 4; i < end;) {
				unsigned size = cea[i] & 31;
				if (i + 1 + size > end)
					break;
				// HDMI vendor-specific data block, IEEE OUI 00-0c-03.
				if ((cea[i] >> 5) == 3 && size >= 3 && cea[i + 1] == 3
					&& cea[i + 2] == 12 && cea[i + 3] == 0)
					hdmi = true;
				i += size + 1;
			}
			for (unsigned i = end; !found && i + 18 <= 127; i += 18)
				found = decode_detailed_timing(cea + i, timing);
		}
	}
	if (!found && (edid[35] & 0x20) != 0) {
		// EDID explicitly advertises VGA 640x480 at 60 Hz.
		timing = {0, 0, 0, 25175, 640, 656, 752, 800, 0, 480, 490,
			492, 525, 0, 60, 0, 0};
		found = true;
	}
	if (found && !hdmi)
		timing.flags |= 1u << 9;
	return found;
}

#endif
