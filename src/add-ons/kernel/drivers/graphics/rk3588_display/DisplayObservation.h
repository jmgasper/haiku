/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_OBSERVATION_H
#define RK3588_DISPLAY_OBSERVATION_H


#include "DisplayResources.h"


namespace RK3588Display {

static const uint32_t kSnapshotVersion = 1;

// Snapshot flags. A snapshot is a sequential read-only observation, never an
// atomic capture, and never a permission to program the display pipeline.
static const uint32_t kSnapshotReadOnly = 1;
static const uint32_t kSnapshotVopRead = 2;
static const uint32_t kSnapshotHdmiRead = 4;
static const uint32_t kSnapshotVopSkipped = 8;
static const uint32_t kSnapshotHdmiSkipped = 16;

// Register offsets. Sources: RK3588 TRM v1.0 Part 1 (CRU, PMU), Linux 6.18.52
// drivers/pmdomain/rockchip/pm-domains.c and drivers/clk/rockchip/clk-rk3588.c,
// Linux/EDK2 VOP2 and DW HDMI QP register definitions. PMU offsets are
// relative to the device-tree window at fd8d8000 (size 0x400).

// PMU: BUS_IDLE_REQ0/1, BUS_IDLE_ACK0/1, BUS_IDLE_ST0/1, PWR_GATE_SFTCON1/2,
// PWR_STATUS, MEM_STATUS (chain) and BISR repair status (power-on indication).
static const uint32_t kPmuOffsets[] = {0x10c, 0x110, 0x118, 0x11c, 0x120, 0x124,
	0x14c, 0x150, 0x180, 0x1f8, 0x290};
static const unsigned kPmuCount = 11;
static const unsigned kPmuPowerGate2 = 7; // 0x150: bit1 VOP, bit2 VO0, bit3 VO1 (1 = off)
static const unsigned kPmuRepairStatus = 10; // 0x290: bit16 VOP, bit17 VO0, bit18 VO1 (1 = on)
static const uint32_t kPmuVopOn = 1u << 16;
static const uint32_t kPmuVo1On = 1u << 18;

// CRU: CLKSEL_CON 111/112/113 (dclk_vop0..3 sources/dividers), 136 (hdmitx1
// earc); CLKGATE_CON 52/53 (VOP), 59 (VO1 bus), 61 (HDMI TX), 72/73 (PHYs, HPD).
static const uint32_t kClockSelectOffsets[] = {0x4bc, 0x4c0, 0x4c4, 0x520};
static const unsigned kClockSelectCount = 4;
static const uint32_t kClockGateOffsets[] = {0x8d0, 0x8d4, 0x8ec, 0x8f4, 0x920, 0x924};
static const unsigned kClockGateCount = 6;
static const unsigned kClockGateVop = 0; // CLKGATE_CON(52): bit8 hclk_vop, bit9 aclk_vop
static const uint32_t kClockGateVopMask = (1u << 8) | (1u << 9);
static const unsigned kClockGateHdmi = 3; // CLKGATE_CON(61): bit2 pclk_hdmitx1
static const uint32_t kClockGateHdmiMask = 1u << 2;

// SYS GRF: SOC_CON2 (HPD interrupt masks), SOC_CON7 (HPD path), SOC_STATUS1
// (HDMI0/1 HPD levels: bits 16-19 and 24-27).
static const uint32_t kSysGrfOffsets[] = {0x308, 0x31c, 0x384};
static const unsigned kSysGrfCount = 3;
static const uint32_t kVopGrfOffset = 0x8; // VOP_CON2: HDMI TX enables
static const uint32_t kVo1GrfOffsets[] = {0x0, 0xc}; // VO1_CON0 sync polarity, CON3 format
static const unsigned kVo1GrfCount = 2;
static const uint32_t kHdptxGrfOffsets[] = {0x0, 0x80}; // CON0 enables, STATUS ready/lock
static const unsigned kHdptxGrfCount = 2;

// VOP2 (fdd90000): only the first 8 KiB are mapped and read.
static const uint32_t kVopMapSize = 0x2000;
static const uint32_t kVopSystemOffsets[] = {0x000, 0x004, 0x008, 0x024, 0x028,
	0x02c, 0x030, 0x034, 0x050, 0x058, 0x060, 0x070, 0x074, 0x078, 0x07c, 0x080,
	0x088, 0x090, 0x098, 0x0a0, 0x0a8, 0x0b0, 0x0b8, 0x0c0, 0x0c8, 0x0d0, 0x0d8};
static const unsigned kVopSystemCount = 27;
static const unsigned kVopSystemVersion = 1;
static const unsigned kVopSystemInterfaceEnable = 4;
static const uint32_t kVopOverlayOffsets[] = {0x600, 0x604, 0x608, 0x6e0, 0x6e4,
	0x6e8, 0x6ec};
static const unsigned kVopOverlayCount = 7;
static const uint32_t kVopPortBase = 0xc00;
static const uint32_t kVopPortStride = 0x100;
static const unsigned kVopPortCount = 4;
static const uint32_t kVopPortOffsets[] = {0x00, 0x04, 0x08, 0x0c, 0x2c, 0x30,
	0x34, 0x38, 0x3c, 0x40, 0x48, 0x4c, 0x50, 0x54};
static const unsigned kVopPortRegisterCount = 14;
static const unsigned kVopPortDisplayControl = 0;
static const unsigned kVopPortHTotal = 10;
static const unsigned kVopPortHActive = 11;
static const unsigned kVopPortVTotal = 12;
static const unsigned kVopPortVActive = 13;
static const uint32_t kVopClusterBase = 0x1000;
static const uint32_t kVopClusterStride = 0x200;
static const unsigned kVopClusterCount = 4;
static const uint32_t kVopClusterOffsets[] = {0x00, 0x04, 0x10, 0x18, 0x20, 0x24,
	0x28, 0x100};
static const unsigned kVopClusterRegisterCount = 8;
static const uint32_t kVopEsmartBase = 0x1800;
static const uint32_t kVopEsmartStride = 0x200;
static const unsigned kVopEsmartCount = 4;
static const uint32_t kVopEsmartOffsets[] = {0x00, 0x10, 0x14, 0x1c, 0x20, 0x24,
	0x28};
static const unsigned kVopEsmartRegisterCount = 7;

// DW HDMI QP TX1 (fdea0000): 16 KiB are mapped and only registers that Linux or
// the firmware read (read-modify-write or read) are touched. Reading other
// offsets can raise a synchronous external abort: the +259 native trial
// panicked on the write-only I2CM_CONTROL0 (0xec). Order: GLOBAL_SWDISABLE,
// I2CM_INTERFACE_CONTROL0/1, AUDIO_INTERFACE_CONFIG0, HDCP2LOGIC_CONFIG0,
// LINK_CONFIG0, PKTSCHED_PKT_CONFIG1, PKTSCHED_PKT_EN, MAINUNIT_1_INT_STATUS,
// MAINUNIT_1_INT_MASK_N.
static const uint32_t kHdmiMapSize = 0x4000;
static const uint32_t kHdmiOffsets[] = {0x044, 0x0f4, 0x0f8, 0x820, 0x8e0, 0x968,
	0xa9c, 0xaa8, 0x3020, 0x3024};
static const unsigned kHdmiCount = 10;


struct DisplaySnapshot {
	uint32_t version;
	uint32_t flags;
	int64_t startedMicros;
	int64_t finishedMicros;
	uint32_t pmu[kPmuCount];
	uint32_t clockSelect[kClockSelectCount];
	uint32_t clockGate[kClockGateCount];
	uint32_t sysGrf[kSysGrfCount];
	uint32_t vopGrf;
	uint32_t vo1Grf[kVo1GrfCount];
	uint32_t hdptxGrf[kHdptxGrfCount];
	uint32_t vopSystem[kVopSystemCount];
	uint32_t vopOverlay[kVopOverlayCount];
	uint32_t vopPort[kVopPortCount][kVopPortRegisterCount];
	uint32_t vopCluster[kVopClusterCount][kVopClusterRegisterCount];
	uint32_t vopEsmart[kVopEsmartCount][kVopEsmartRegisterCount];
	uint32_t hdmi[kHdmiCount];
};


inline bool
VopReadable(const DisplaySnapshot& snapshot)
{
	return (snapshot.pmu[kPmuRepairStatus] & kPmuVopOn) != 0
		&& (snapshot.clockGate[kClockGateVop] & kClockGateVopMask) == 0;
}


inline bool
HdmiReadable(const DisplaySnapshot& snapshot)
{
	return (snapshot.pmu[kPmuRepairStatus] & kPmuVo1On) != 0
		&& (snapshot.clockGate[kClockGateHdmi] & kClockGateHdmiMask) == 0;
}


// Reads the always-on control blocks first, then maps VOP2 and HDMI TX1 only
// while their power domains are on and their bus clocks are ungated. Every
// path releases its mappings; nothing is written. The hardware adapter is a
// template parameter so host fixtures can model the kernel services.
template<class Hardware>
status_t
ObserveDisplay(Hardware& hardware, const ResourceInfo& resources, DisplaySnapshot& output)
{
	if (!ResourcesMatch(resources))
		return B_NOT_SUPPORTED;
	DisplaySnapshot snapshot = {};
	snapshot.version = kSnapshotVersion;
	snapshot.flags = kSnapshotReadOnly;
	status_t status = hardware.MapControl(resources);
	if (status != B_OK)
		return status;
	snapshot.startedMicros = hardware.Now();
	for (unsigned i = 0; i < kPmuCount; i++)
		snapshot.pmu[i] = hardware.ReadPmu(kPmuOffsets[i]);
	for (unsigned i = 0; i < kClockSelectCount; i++)
		snapshot.clockSelect[i] = hardware.ReadClock(kClockSelectOffsets[i]);
	for (unsigned i = 0; i < kClockGateCount; i++)
		snapshot.clockGate[i] = hardware.ReadClock(kClockGateOffsets[i]);
	for (unsigned i = 0; i < kSysGrfCount; i++)
		snapshot.sysGrf[i] = hardware.ReadSysGrf(kSysGrfOffsets[i]);
	snapshot.vopGrf = hardware.ReadVopGrf(kVopGrfOffset);
	for (unsigned i = 0; i < kVo1GrfCount; i++)
		snapshot.vo1Grf[i] = hardware.ReadVo1Grf(kVo1GrfOffsets[i]);
	for (unsigned i = 0; i < kHdptxGrfCount; i++)
		snapshot.hdptxGrf[i] = hardware.ReadHdptxGrf(kHdptxGrfOffsets[i]);

	if (VopReadable(snapshot)) {
		status = hardware.MapVop(resources, kVopMapSize);
		if (status != B_OK) {
			hardware.Unmap();
			return status;
		}
		for (unsigned i = 0; i < kVopSystemCount; i++)
			snapshot.vopSystem[i] = hardware.ReadVop(kVopSystemOffsets[i]);
		for (unsigned i = 0; i < kVopOverlayCount; i++)
			snapshot.vopOverlay[i] = hardware.ReadVop(kVopOverlayOffsets[i]);
		for (unsigned port = 0; port < kVopPortCount; port++) {
			for (unsigned i = 0; i < kVopPortRegisterCount; i++) {
				snapshot.vopPort[port][i] = hardware.ReadVop(kVopPortBase
					+ port * kVopPortStride + kVopPortOffsets[i]);
			}
		}
		for (unsigned window = 0; window < kVopClusterCount; window++) {
			for (unsigned i = 0; i < kVopClusterRegisterCount; i++) {
				snapshot.vopCluster[window][i] = hardware.ReadVop(kVopClusterBase
					+ window * kVopClusterStride + kVopClusterOffsets[i]);
			}
		}
		for (unsigned window = 0; window < kVopEsmartCount; window++) {
			for (unsigned i = 0; i < kVopEsmartRegisterCount; i++) {
				snapshot.vopEsmart[window][i] = hardware.ReadVop(kVopEsmartBase
					+ window * kVopEsmartStride + kVopEsmartOffsets[i]);
			}
		}
		hardware.UnmapVop();
		snapshot.flags |= kSnapshotVopRead;
	} else
		snapshot.flags |= kSnapshotVopSkipped;

	if (HdmiReadable(snapshot)) {
		status = hardware.MapHdmi(resources, kHdmiMapSize);
		if (status != B_OK) {
			hardware.Unmap();
			return status;
		}
		for (unsigned i = 0; i < kHdmiCount; i++)
			snapshot.hdmi[i] = hardware.ReadHdmi(kHdmiOffsets[i]);
		hardware.UnmapHdmi();
		snapshot.flags |= kSnapshotHdmiRead;
	} else
		snapshot.flags |= kSnapshotHdmiSkipped;

	snapshot.finishedMicros = hardware.Now();
	hardware.Unmap();
	output = snapshot;
	return B_OK;
}

} // namespace RK3588Display

#endif // RK3588_DISPLAY_OBSERVATION_H
