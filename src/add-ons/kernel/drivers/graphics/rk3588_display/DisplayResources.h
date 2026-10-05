/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_RESOURCES_H
#define RK3588_DISPLAY_RESOURCES_H


#include <stddef.h>
#include <stdint.h>
#include <string.h>


namespace RK3588Display {

// 'RDI' + operation. Both requests are read-only diagnostics.
static const uint32_t kGetResources = 0x52444900;
static const uint32_t kGetSnapshot = 0x52444901;
static const uint32_t kResourceVersion = 2; // 2: the DisplayPort TX1 path blocks
static const uint32_t kDescriptionValidated = 1;

// Pointer-free description of the display pipeline resources admitted from
// the firmware device tree: VOP2, the native HDMI TX1 controller, its Samsung
// HDPTX PHY and the always-on control blocks needed to observe them safely.
// These values describe firmware resources; they do not report scanout,
// hot-plug, EDID or any register access.
struct ResourceInfo {
	uint32_t version;
	uint32_t flags;
	uint64_t vopBase;
	uint64_t vopSize;
	uint64_t vopLutBase;
	uint64_t vopLutSize;
	uint64_t hdmiBase;
	uint64_t hdmiSize;
	uint64_t hdptxBase;
	uint64_t hdptxSize;
	uint64_t hdptxGrfBase;
	uint64_t hdptxGrfSize;
	uint64_t sysGrfBase;
	uint64_t sysGrfSize;
	uint64_t vopGrfBase;
	uint64_t vopGrfSize;
	uint64_t vo1GrfBase;
	uint64_t vo1GrfSize;
	uint64_t pmuBase;
	uint64_t pmuSize;
	uint64_t clockBase;
	uint64_t clockSize;
	uint64_t interruptBase;
	uint32_t vopInterrupt;
	uint32_t hdmiInterrupts[5]; // avp, cec, earc, main, hpd
	uint32_t vopClockIds[7]; // aclk, hclk, dclk_vp0..3, pclk_vop
	uint32_t hdmiClockIds[6]; // pclk, earc, ref, aud, hdp, hclk_vo1
	uint32_t vopPowerDomain;
	uint32_t hdmiPowerDomain;
	uint32_t hdmiPhyPhandle;
	uint32_t vopPortIndex; // video port feeding HDMI TX1 in the device tree
	char boardCompatible[32];
	// The second connector's path (version 2): USBDP PHY1 and its GRFs, the
	// bus IOC and GPIO3 for the hot-plug pin, all from the firmware device
	// tree; DisplayPort TX1 itself has no node there, so its block comes
	// from Linux' rk3588-extra.dtsi (dp@fde60000, PD_VO0) as a constant.
	uint64_t usbdpPhyBase;
	uint64_t usbdpPhySize;
	uint64_t usbdpGrfBase;
	uint64_t usbdpGrfSize;
	uint64_t vo0GrfBase;
	uint64_t vo0GrfSize;
	uint64_t iocBase;
	uint64_t iocSize;
	uint64_t gpio3Base;
	uint64_t gpio3Size;
	uint64_t dpBase;
	uint64_t dpSize;
	uint32_t usbdpPhyClockIds[3]; // refclk, immortal, pclk
	uint32_t usbdpPhyResets[5]; // init, cmn, lane, pcs_apb, pma_apb
	uint32_t gpio3ClockIds[2];
	uint32_t dpPowerDomain;
	uint32_t reserved;
};

static const uint64_t kDpBase = 0xfde60000; // DisplayPort TX1, Linux rk3588-extra.dtsi
static const uint64_t kDpSize = 0x4000;
static const uint32_t kDpPowerDomain = 25; // RK3588_PD_VO0


inline bool
ReadCells(const void* data, int length, uint32_t* cells, size_t count)
{
	if (data == NULL || cells == NULL || length < 0
		|| count > SIZE_MAX / 4 || (size_t)length != count * 4) {
		return false;
	}
	const uint8_t* bytes = (const uint8_t*)data;
	for (size_t i = 0; i < count; i++) {
		cells[i] = (uint32_t)bytes[i * 4] << 24
			| (uint32_t)bytes[i * 4 + 1] << 16
			| (uint32_t)bytes[i * 4 + 2] << 8
			| bytes[i * 4 + 3];
	}
	return true;
}


inline int
StringIndex(const void* data, int length, const char* wanted)
{
	if (data == NULL || wanted == NULL || length <= 0)
		return -1;
	const char* cursor = (const char*)data;
	int result = -1;
	for (int index = 0; length > 0; index++) {
		const char* end = (const char*)memchr(cursor, 0, length);
		if (end == NULL || end == cursor)
			return -1;
		if (strcmp(cursor, wanted) == 0) {
			if (result >= 0)
				return -1;
			result = index;
		}
		length -= end - cursor + 1;
		cursor = end + 1;
	}
	return result;
}


// The exact ROCK 5 ITX / EDK2 v1.1 mainline device-tree description this
// driver was written against. Anything else is refused before any mapping.
inline bool
ResourcesMatch(const ResourceInfo& info)
{
	static const uint32_t kVopClocks[7] = {0x25d, 0x25c, 0x261, 0x262, 0x263, 0x264, 0x25b};
	static const uint32_t kHdmiClocks[6] = {0x213, 0x214, 0x215, 0x239, 0x253, 0x2cd};
	static const uint32_t kHdmiInterrupts[5] = {205, 206, 207, 208, 393};
	return info.version == kResourceVersion && info.flags == kDescriptionValidated
		&& info.vopBase == 0xfdd90000 && info.vopSize == 0x4200
		&& info.vopLutBase == 0xfdd95000 && info.vopLutSize == 0x1000
		&& info.hdmiBase == 0xfdea0000 && info.hdmiSize == 0x20000
		&& info.hdptxBase == 0xfed70000 && info.hdptxSize == 0x2000
		&& info.hdptxGrfBase == 0xfd5e4000 && info.hdptxGrfSize == 0x100
		&& info.sysGrfBase == 0xfd58c000 && info.sysGrfSize == 0x1000
		&& info.vopGrfBase == 0xfd5a4000 && info.vopGrfSize == 0x2000
		&& info.vo1GrfBase == 0xfd5a8000 && info.vo1GrfSize == 0x4000
		&& info.pmuBase == 0xfd8d8000 && info.pmuSize == 0x400
		&& info.clockBase == 0xfd7c0000 && info.clockSize == 0x5c000
		&& info.interruptBase == 0xfe600000
		&& info.vopInterrupt == 188
		&& memcmp(info.hdmiInterrupts, kHdmiInterrupts, sizeof(kHdmiInterrupts)) == 0
		&& memcmp(info.vopClockIds, kVopClocks, sizeof(kVopClocks)) == 0
		&& memcmp(info.hdmiClockIds, kHdmiClocks, sizeof(kHdmiClocks)) == 0
		&& info.vopPowerDomain == 24 && info.hdmiPowerDomain == 26
		&& info.hdmiPhyPhandle != 0 && info.vopPortIndex == 1
		&& strcmp(info.boardCompatible, "radxa,rock-5-itx") == 0
		&& info.usbdpPhyBase == 0xfed90000 && info.usbdpPhySize == 0x10000
		&& info.usbdpGrfBase == 0xfd5cc000 && info.usbdpGrfSize == 0x4000
		&& info.vo0GrfBase == 0xfd5a6000 && info.vo0GrfSize == 0x2000
		&& info.iocBase == 0xfd5f0000 && info.iocSize == 0x10000
		&& info.gpio3Base == 0xfec40000 && info.gpio3Size == 0x100
		&& info.dpBase == kDpBase && info.dpSize == kDpSize
		&& info.dpPowerDomain == kDpPowerDomain;
}

} // namespace RK3588Display

#endif // RK3588_DISPLAY_RESOURCES_H
