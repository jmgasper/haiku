/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_MODESET_H
#define RK3588_DISPLAY_MODESET_H


#include "DisplayAccelerant.h"
#include "DisplayScanout.h"


namespace RK3588Display {

// Native mode setting on HDMI1 (VOP2 video port -> DW HDMI QP TX1 -> Samsung
// HDPTX PHY1). The sequence follows mainline Linux 6.18 (rockchip_drm_vop2.c,
// dw-hdmi-qp.c, phy-rockchip-samsung-hdptx.c) with the register values EDK2
// v1.1 programs on this board: stop the port, power the PHY down, program
// the ROPLL for the TMDS rate, write the port timing and window geometry,
// commit and restart the port, bring the lanes up, then refresh the AVI
// infoframe. The pixel clock is the PHY PLL's pixel output, which the
// firmware already selected as dclk_vop2; the CRU is touched only for the
// three PHY resets. Only 8-bit RGB TMDS modes at or below the frame buffer
// size are accepted.
static const uint32_t kSetDisplayMode = 0x52444909; // writable handle, frame buffer acquired
static const uint32_t kModeVersion = 1;

static const uint32_t kModeOK = 0;
static const uint32_t kModeNotAcquired = 1;
static const uint32_t kModeUnsupported = 2; // no PLL configuration or does not fit the buffer
static const uint32_t kModeHoldTimeout = 3; // the port never reported standby
static const uint32_t kModePllTimeout = 4; // PHY clock never became ready
static const uint32_t kModeLaneTimeout = 5; // PHY lanes never locked
static const uint32_t kModeVerifyFailed = 6; // timing read-back differs

static const uint32_t kModePositiveHSync = 1; // flags
static const uint32_t kModePositiveVSync = 2;

// Phases reached, for diagnostics.
static const uint32_t kPhaseStopped = 1;
static const uint32_t kPhasePhyOff = 2;
static const uint32_t kPhasePllReady = 3;
static const uint32_t kPhasePortProgrammed = 4;
static const uint32_t kPhaseLanesReady = 5;
static const uint32_t kPhaseInfoframes = 6;

struct ModeRequest {
	uint32_t version; // in
	uint32_t flags; // in: sync polarities
	uint32_t pixelClockKHz; // in
	uint32_t hDisplay, hSyncStart, hSyncEnd, hTotal; // in
	uint32_t vDisplay, vSyncStart, vSyncEnd, vTotal; // in
	uint32_t vic; // in: CEA-861 video identification code for the AVI infoframe, 0 if none
	uint32_t result;
	uint32_t phase;
	uint32_t holdPolls;
	uint32_t clockPolls;
	uint32_t lockPolls;
	uint32_t phyStatus; // HDPTX GRF status after the lanes came up
	uint32_t timing[4]; // port timing words read back
	uint32_t interfaceEnable;
	int64_t startedMicros;
	int64_t finishedMicros;
};

// Samsung HDPTX ROPLL configurations for TMDS character rates (kHz), from
// mainline Linux ropll_tmds_cfg[] (identical rows in EDK2 v1.1). Rates whose
// modes exceed the 1920x1080 frame buffer are left out.
struct PllConfig {
	uint32_t rateKHz;
	uint8_t mdiv, mdivAfc, pdiv, refdiv, sdiv;
	uint8_t sdmEnable, sdmDenominator, sdmNumeratorSign, sdmNumerator;
	uint8_t sdcN, sdcNumerator, sdcDenominator;
};
static const PllConfig kPllConfigs[] = {
	{148500, 0x7b, 0x7b, 1, 1, 3, 1, 4, 0, 3, 5, 5, 0x10},
	{108000, 135, 135, 1, 1, 5, 0, 0x9, 0, 0x05, 0, 0x14, 0x18},
	{106500, 89, 89, 1, 1, 3, 1, 89, 1, 16, 1, 0, 1},
	{85500, 214, 214, 1, 1, 11, 1, 214, 1, 16, 2, 1, 1},
	{83500, 105, 105, 1, 1, 5, 1, 42, 1, 16, 1, 0, 1},
	{74250, 124, 124, 1, 1, 7, 1, 62, 1, 16, 5, 0, 1},
	{65000, 162, 162, 1, 1, 11, 1, 54, 0, 16, 4, 1, 1},
	{40000, 100, 100, 1, 1, 11, 0, 0x9, 0, 0x05, 0, 0x14, 0x18},
	{27000, 0x5a, 0x5a, 1, 1, 0xf, 0, 0x9, 0, 0x05, 0, 0x14, 0x18},
	{25175, 84, 84, 1, 1, 0xf, 1, 168, 1, 16, 4, 1, 1},
};
static const unsigned kPllConfigCount = sizeof(kPllConfigs) / sizeof(kPllConfigs[0]);


inline const PllConfig*
FindPllConfig(uint32_t rateKHz)
{
	for (unsigned i = 0; i < kPllConfigCount; i++) {
		if (kPllConfigs[i].rateKHz == rateKHz)
			return &kPllConfigs[i];
	}
	return NULL;
}


// Register offsets. PHY register n of a block sits at n * 4.
static const uint32_t kPhyCmn = 0x0000; // CMN_REG(n) = n * 4, n <= 0xa7
static const uint32_t kPhySb = 0x0400; // SB_REG(0x100 + n)
static const uint32_t kPhyLntop = 0x0800; // LNTOP_REG(0x200 + n)
static const uint32_t kPhyLane = 0x0c00; // LANE_REG(0x300 + n) + 0x400 per lane
static const uint32_t kPhyMapSize = 0x2000;
static const uint32_t kPhyGrfControl = 0x00; // HIWORD-mask
static const uint32_t kPhyGrfStatus = 0x80;
static const uint32_t kPhyPllEnable = 1u << 7;
static const uint32_t kPhyBiasEnable = 1u << 6;
static const uint32_t kPhyBgrEnable = 1u << 5;
static const uint32_t kPhyModeSelect = 1u << 0;
static const uint32_t kPhyStatusPllLock = 1u << 3;
static const uint32_t kPhyStatusClockReady = 1u << 2;
static const uint32_t kPhyStatusPhyReady = 1u << 1;
static const uint32_t kPhyTmds14MaxKHz = 340000;

// CRU: soft resets are HIWORD-mask words at 0xa00 + 4n; PMU1CRU at +0x30000.
static const uint32_t kCruApbResetOffset = 0xa00 + 72 * 4; // SOFTRST_CON72 bit 6: p_hdptx1
static const uint32_t kCruApbResetBit = 1u << 6;
static const uint32_t kCruInitResetOffset = 0x30000 + 0xa00 + 3 * 4; // PMU1CRU_SOFTRST_CON03 bit 15
static const uint32_t kCruInitResetBit = 1u << 15;
static const uint32_t kCruCmnLaneResetOffset = 0x30000 + 0xa00 + 4 * 4; // CON04: bit 0 cmn, bit 1 lane
static const uint32_t kCruCmnResetBit = 1u << 0;
static const uint32_t kCruLaneResetBit = 1u << 1;

// VOP2 video port (base 0xc00 + 0x100 per port) and window words.
static const uint32_t kVopPortControlWord = 0x00; // DSP_CTRL: bit 31 standby, [3:0] out mode
static const uint32_t kVopPortMipiControl = 0x04;
static const uint32_t kVopPortDisplayBackground = 0x2c;
static const uint32_t kVopPortPreScanTiming = 0x30;
static const uint32_t kVopPortPostHActive = 0x34;
static const uint32_t kVopPortPostVActive = 0x38;
static const uint32_t kVopPortPostScaleFactor = 0x3c;
static const uint32_t kVopPortPostScaleControl = 0x40;
static const uint32_t kVopPortHTotalSyncEnd = 0x48;
static const uint32_t kVopPortHActiveStartEnd = 0x4c;
static const uint32_t kVopPortVTotalSyncEnd = 0x50;
static const uint32_t kVopPortVActiveStartEnd = 0x54;
static const uint32_t kVopLineFlagBase = 0x70; // + 4 per port
static const uint32_t kVopBackgroundMixBase = 0x6e0; // + 4 per port, [31:24] delay
static const uint32_t kVopPortStandby = 1u << 31;
static const uint32_t kVopOutputModeAAAA = 0xf;
static const uint32_t kVopInterruptHoldValid = 1u << 6;
static const uint32_t kVopPreScanMaxDelay = 52; // RK3588 pre_scan_max_dly[3]

// DW HDMI QP TX packet scheduler and link words.
static const uint32_t kHdmiHdcp2Config = 0x8e0; // bit 0 bypass
static const uint32_t kHdmiLinkConfig = 0x968; // bit 4 DVI
static const uint32_t kHdmiPacketConfig1 = 0xa9c; // bit 12 AVI field rate
static const uint32_t kHdmiPacketEnable = 0xaa8; // bit 13 AVI, bit 3 GCP
static const uint32_t kHdmiPacketControl = 0xaac; // write-only: bit 1 clears AVMUTE
static const uint32_t kHdmiAviContents = 0xbe0; // write-only, 5 words
static const uint32_t kHdmiAviEnable = 1u << 13;
static const uint32_t kHdmiGcpEnable = 1u << 3;
static const uint32_t kHdmiTxMapSize = 0x4000;
static const uint32_t kHdptxGrfMapSize = 0x100;

struct RegisterValue {
	uint16_t offset;
	uint8_t value;
};

// Linux rk_hdtpx_common_cmn_init_seq.
static const RegisterValue kPhyCommonInit[] = {
	{0x024, 0x0c}, {0x028, 0x83}, {0x02c, 0x06}, {0x030, 0x20}, {0x034, 0xb8}, {0x038, 0x0f},
	{0x03c, 0x0f}, {0x040, 0x04}, {0x044, 0x00}, {0x048, 0x26}, {0x04c, 0x22}, {0x050, 0x24},
	{0x054, 0x77}, {0x058, 0x08}, {0x05c, 0x00}, {0x060, 0x04}, {0x064, 0x48}, {0x068, 0x01},
	{0x06c, 0x00}, {0x070, 0x01}, {0x074, 0x64}, {0x07c, 0x00}, {0x098, 0x53}, {0x0a4, 0x01},
	{0x0c0, 0x00}, {0x0c4, 0x20}, {0x0c8, 0x30}, {0x0cc, 0x0b}, {0x0d0, 0x23}, {0x0d4, 0x00},
	{0x0e0, 0x00}, {0x0e4, 0x00}, {0x0e8, 0x00}, {0x0ec, 0x00}, {0x0f0, 0x80}, {0x0f8, 0x0c},
	{0x0fc, 0x83}, {0x100, 0x06}, {0x104, 0x20}, {0x108, 0xb8}, {0x10c, 0x00}, {0x110, 0x46},
	{0x114, 0x24}, {0x11c, 0x00}, {0x124, 0xfa}, {0x128, 0x08}, {0x12c, 0x00}, {0x130, 0x01},
	{0x134, 0x64}, {0x138, 0x14}, {0x13c, 0x00}, {0x140, 0x00}, {0x174, 0x0c}, {0x17c, 0x01},
	{0x1ac, 0x04}, {0x1cc, 0x30}, {0x1d0, 0x00}, {0x1d4, 0x20}, {0x1d8, 0x30}, {0x1dc, 0x08},
	{0x1e0, 0x0c}, {0x1e4, 0x00}, {0x1ec, 0x00}, {0x1f0, 0x00}, {0x1f4, 0x00}, {0x1f8, 0x00},
	{0x1fc, 0x00}, {0x200, 0x00}, {0x204, 0x09}, {0x208, 0x04}, {0x20c, 0x24}, {0x210, 0x20},
	{0x214, 0x03}, {0x218, 0x01}, {0x21c, 0x0c}, {0x228, 0x55}, {0x22c, 0x25}, {0x230, 0x2c},
	{0x234, 0x22}, {0x238, 0x14}, {0x23c, 0x20}, {0x240, 0x00}, {0x244, 0x00}, {0x248, 0x00},
	{0x24c, 0x00}, {0x268, 0x11}, {0x26c, 0x10},
};
// Linux rk_hdtpx_tmds_cmn_init_seq.
static const RegisterValue kPhyTmdsInit[] = {
	{0x020, 0x00}, {0x044, 0x01}, {0x05c, 0x20}, {0x078, 0x14}, {0x080, 0x00}, {0x084, 0x00},
	{0x088, 0x11}, {0x08c, 0x00}, {0x090, 0x00}, {0x094, 0x53}, {0x098, 0x00}, {0x09c, 0x00},
	{0x0a0, 0x01}, {0x0a8, 0x00}, {0x0ac, 0x00}, {0x0b0, 0x00}, {0x0b4, 0x00}, {0x0b8, 0x04},
	{0x0bc, 0x00}, {0x0c0, 0x20}, {0x0c4, 0x30}, {0x0c8, 0x0b}, {0x0cc, 0x23}, {0x0d0, 0x00},
	{0x0f4, 0x40}, {0x108, 0x78}, {0x118, 0xdd}, {0x120, 0x11}, {0x138, 0x34}, {0x170, 0x25},
	{0x178, 0x4f}, {0x1d0, 0x04}, {0x204, 0x01}, {0x21c, 0x04}, {0x224, 0x00}, {0x254, 0x00},
	{0x25c, 0x02}, {0x264, 0x04}, {0x26c, 0x00},
};
// Per-rate CMN words (CMN_REG(0051) ...). Names follow Linux.
static const uint32_t kPhyPllMdiv = 0x144; // CMN_REG(0051)
static const uint32_t kPhyPllMdivAfc = 0x154; // CMN_REG(0055)
static const uint32_t kPhyPllPdivRefdiv = 0x164; // CMN_REG(0059)
static const uint32_t kPhyPllSdiv = 0x168; // CMN_REG(005a)
static const uint32_t kPhyPllSdmControl = 0x178; // CMN_REG(005e): bit 6 SDM enable
static const uint32_t kPhyPllSdmSign = 0x190; // CMN_REG(0064): bit 3
static const uint32_t kPhyPllSdmDenominator = 0x180; // CMN_REG(0060)
static const uint32_t kPhyPllSdmNumerator = 0x194; // CMN_REG(0065)
static const uint32_t kPhyPllSdcN = 0x1a4; // CMN_REG(0069): [2:0]
static const uint32_t kPhyPllSdcNumerator = 0x1b0; // CMN_REG(006c)
static const uint32_t kPhyPllSdcDenominator = 0x1c0; // CMN_REG(0070)
static const uint32_t kPhyPllPcgControl = 0x218; // CMN_REG(0086): [7:4] postdiv, [3:1] clock select, bit 0 enable
// Linux rk_hdtpx_common_sb_init_seq, lntop sequences, lane sequences.
static const RegisterValue kPhySbInit[] = {{0x450, 0x00}, {0x454, 0x00}, {0x458, 0x00}, {0x45c, 0x00}};
static const RegisterValue kPhyLntopLowRate[] = {
	{0x804, 0x07}, {0x808, 0xc1}, {0x80c, 0xf0}, {0x810, 0x7c}, {0x814, 0x1f}};
static const RegisterValue kPhyLntopHighRate[] = {
	{0x804, 0x00}, {0x808, 0x00}, {0x80c, 0x0f}, {0x810, 0xff}, {0x814, 0xff}};
static const RegisterValue kPhyLaneInit[] = { // per lane, + 0x400 * lane
	{0x00c, 0x0c}, {0x01c, 0x20}, {0x028, 0x17}, {0x02c, 0x77}, {0x030, 0x77}, {0x034, 0x77},
	{0x038, 0x38}, {0x040, 0x03}, {0x044, 0x0f}, {0x058, 0x02}, {0x06c, 0x01}, {0x07c, 0x15},
	{0x080, 0xa0},
};
static const RegisterValue kPhyTmdsLane[] = { // per lane, then the skew word
	{0x048, 0x00}, {0x00c, 0x2f}, {0x014, 0x03}, {0x018, 0x1c},
};
static const uint8_t kPhyTmdsLaneSkew[4] = {0x02, 0x02, 0x02, 0x0a}; // LANE_REG(0x1e) per lane


inline uint32_t
HiWord(uint32_t mask, uint32_t value)
{
	return (mask << 16) | (value & mask);
}


// The 17-byte AVI infoframe EDK2 v1.1 sends: underscanned RGB, VIC, no
// aspect or colorimetry information, packed the way the QP TX wants it.
inline void
BuildAviInfoframeWords(uint32_t vic, uint32_t words[5])
{
	uint8_t frame[17] = {0x82, 0x02, 0x0d, 0x00, 0x02, 0x00, 0x00, (uint8_t)vic};
	unsigned sum = 0;
	for (unsigned i = 0; i < 17; i++)
		sum += frame[i];
	frame[3] = (uint8_t)(0x100 - (sum & 0xff));
	words[0] = frame[1] << 8 | frame[2] << 16;
	for (unsigned i = 0; i < 4; i++) {
		uint32_t value = 0;
		for (unsigned j = 0; j < 4 && i * 4 + j < 14; j++)
			value |= (uint32_t)frame[i * 4 + j + 3] << (8 * j);
		words[1 + i] = value;
	}
}


template<class Hardware>
void
WritePhySequence(Hardware& hardware, const RegisterValue* values, unsigned count, uint32_t base)
{
	for (unsigned i = 0; i < count; i++)
		hardware.WritePhy(base + values[i].offset, values[i].value);
}


// Linux rk_hdptx_phy_disable: APB reset pulse, lane and bias off, all three
// resets asserted, PLL/bias/bandgap disabled.
template<class Hardware>
void
DisablePhy(Hardware& hardware)
{
	hardware.WriteCru(kCruApbResetOffset, HiWord(kCruApbResetBit, kCruApbResetBit));
	hardware.Pause(25);
	hardware.WriteCru(kCruApbResetOffset, HiWord(kCruApbResetBit, 0));
	hardware.WritePhy(kPhyLane + 0x000, 0x82);
	hardware.WritePhy(kPhySb + 0x03c, 0xc1);
	hardware.WritePhy(kPhySb + 0x040, 0x01);
	for (unsigned lane = 0; lane < 4; lane++)
		hardware.WritePhy(kPhyLane + 0x400 * lane + 0x004, 0x80);
	hardware.WriteCru(kCruCmnLaneResetOffset, HiWord(kCruLaneResetBit, kCruLaneResetBit));
	hardware.WriteCru(kCruCmnLaneResetOffset, HiWord(kCruCmnResetBit, kCruCmnResetBit));
	hardware.WriteCru(kCruInitResetOffset, HiWord(kCruInitResetBit, kCruInitResetBit));
	hardware.WriteHdptxGrf(kPhyGrfControl, HiWord(kPhyPllEnable | kPhyBiasEnable | kPhyBgrEnable, 0));
}


// Linux rk_hdptx_ropll_tmds_cmn_config + rk_hdptx_post_enable_pll.
template<class Hardware>
uint32_t
ConfigurePhyPll(Hardware& hardware, const PllConfig& config, ModeRequest& request)
{
	// rk_hdptx_pre_power_up
	hardware.WriteCru(kCruApbResetOffset, HiWord(kCruApbResetBit, kCruApbResetBit));
	hardware.Pause(25);
	hardware.WriteCru(kCruApbResetOffset, HiWord(kCruApbResetBit, 0));
	hardware.WriteCru(kCruCmnLaneResetOffset, HiWord(kCruLaneResetBit, kCruLaneResetBit));
	hardware.WriteCru(kCruCmnLaneResetOffset, HiWord(kCruCmnResetBit, kCruCmnResetBit));
	hardware.WriteCru(kCruInitResetOffset, HiWord(kCruInitResetBit, kCruInitResetBit));
	hardware.WriteHdptxGrf(kPhyGrfControl, HiWord(kPhyPllEnable | kPhyBiasEnable | kPhyBgrEnable, 0));

	WritePhySequence(hardware, kPhyCommonInit, sizeof(kPhyCommonInit) / sizeof(RegisterValue), 0);
	WritePhySequence(hardware, kPhyTmdsInit, sizeof(kPhyTmdsInit) / sizeof(RegisterValue), 0);
	hardware.WritePhy(kPhyPllMdiv, config.mdiv);
	hardware.WritePhy(kPhyPllMdivAfc, config.mdivAfc);
	hardware.WritePhy(kPhyPllPdivRefdiv, (config.pdiv << 4) | config.refdiv);
	hardware.WritePhy(kPhyPllSdiv, config.sdiv << 4);
	uint32_t sdm = hardware.ReadPhy(kPhyPllSdmControl);
	sdm = (sdm & ~(1u << 6)) | (config.sdmEnable != 0 ? 1u << 6 : 0);
	if (config.sdmEnable == 0)
		sdm &= ~0xfu;
	hardware.WritePhy(kPhyPllSdmControl, sdm);
	uint32_t sign = hardware.ReadPhy(kPhyPllSdmSign);
	hardware.WritePhy(kPhyPllSdmSign, (sign & ~(1u << 3)) | (config.sdmNumeratorSign != 0 ? 1u << 3 : 0));
	hardware.WritePhy(kPhyPllSdmDenominator, config.sdmDenominator);
	hardware.WritePhy(kPhyPllSdmNumerator, config.sdmNumerator);
	uint32_t sdcN = hardware.ReadPhy(kPhyPllSdcN);
	hardware.WritePhy(kPhyPllSdcN, (sdcN & ~0x7u) | (config.sdcN & 0x7));
	hardware.WritePhy(kPhyPllSdcNumerator, config.sdcNumerator);
	hardware.WritePhy(kPhyPllSdcDenominator, config.sdcDenominator);
	uint32_t pcg = hardware.ReadPhy(kPhyPllPcgControl);
	pcg = (pcg & ~0xf0u) | ((uint32_t)config.sdiv << 4);
	pcg = pcg & ~0xeu; // clock select 0 for 8 bits per colour
	pcg |= 1u; // clock enable
	hardware.WritePhy(kPhyPllPcgControl, pcg);

	// rk_hdptx_post_enable_pll
	hardware.WriteHdptxGrf(kPhyGrfControl, HiWord(kPhyBiasEnable | kPhyBgrEnable, kPhyBiasEnable | kPhyBgrEnable));
	hardware.Pause(15);
	hardware.WriteCru(kCruInitResetOffset, HiWord(kCruInitResetBit, 0));
	hardware.Pause(15);
	hardware.WriteHdptxGrf(kPhyGrfControl, HiWord(kPhyPllEnable, kPhyPllEnable));
	hardware.Pause(15);
	hardware.WriteCru(kCruCmnLaneResetOffset, HiWord(kCruCmnResetBit, 0));
	request.clockPolls = 0;
	while ((hardware.ReadHdptxGrfStatus() & kPhyStatusClockReady) == 0) {
		if (request.clockPolls >= 100)
			return kModePllTimeout;
		request.clockPolls++;
		hardware.Pause(20);
	}
	return kModeOK;
}


// Linux rk_hdptx_phy_power_on (TMDS) + rk_hdptx_ropll_tmds_mode_config +
// rk_hdptx_post_enable_lane.
template<class Hardware>
uint32_t
ConfigurePhyLanes(Hardware& hardware, uint32_t rateKHz, ModeRequest& request)
{
	hardware.WriteHdptxGrf(kPhyGrfControl, HiWord(kPhyModeSelect, 0));
	WritePhySequence(hardware, kPhySbInit, sizeof(kPhySbInit) / sizeof(RegisterValue), 0);
	hardware.WritePhy(kPhyLntop + 0x000, 0x06);
	if (rateKHz > kPhyTmds14MaxKHz)
		WritePhySequence(hardware, kPhyLntopHighRate, sizeof(kPhyLntopHighRate) / sizeof(RegisterValue), 0);
	else
		WritePhySequence(hardware, kPhyLntopLowRate, sizeof(kPhyLntopLowRate) / sizeof(RegisterValue), 0);
	hardware.WritePhy(kPhyLntop + 0x018, 0x07);
	hardware.WritePhy(kPhyLntop + 0x01c, 0x0f);
	for (unsigned lane = 0; lane < 4; lane++) {
		WritePhySequence(hardware, kPhyLaneInit, sizeof(kPhyLaneInit) / sizeof(RegisterValue),
			kPhyLane + 0x400 * lane);
	}
	for (unsigned lane = 0; lane < 4; lane++)
		hardware.WritePhy(kPhyLane + 0x400 * lane + 0x048, 0x00);
	for (unsigned lane = 0; lane < 4; lane++)
		hardware.WritePhy(kPhyLane + 0x400 * lane + 0x00c, 0x2f);
	for (unsigned lane = 0; lane < 4; lane++)
		hardware.WritePhy(kPhyLane + 0x400 * lane + 0x014, 0x03);
	for (unsigned lane = 0; lane < 4; lane++)
		hardware.WritePhy(kPhyLane + 0x400 * lane + 0x018, 0x1c);
	for (unsigned lane = 0; lane < 4; lane++)
		hardware.WritePhy(kPhyLane + 0x400 * lane + 0x078, kPhyTmdsLaneSkew[lane]);
	// rk_hdptx_post_enable_lane
	hardware.WriteCru(kCruCmnLaneResetOffset, HiWord(kCruLaneResetBit, 0));
	hardware.WriteHdptxGrf(kPhyGrfControl, HiWord(kPhyBiasEnable | kPhyBgrEnable, kPhyBiasEnable | kPhyBgrEnable));
	request.lockPolls = 0;
	while (true) {
		request.phyStatus = hardware.ReadHdptxGrfStatus();
		if ((request.phyStatus & kPhyStatusPhyReady) != 0 && (request.phyStatus & kPhyStatusPllLock) != 0)
			break;
		if (request.lockPolls >= 50)
			return kModeLaneTimeout;
		request.lockPolls++;
		hardware.Pause(100);
	}
	return kModeOK;
}


// Stops the port as vop2_crtc_atomic_disable does: standby takes effect at
// the end of the frame, which the DSP_HOLD_VALID interrupt reports.
template<class Hardware>
uint32_t
StopPort(Hardware& hardware, uint32_t port, ModeRequest& request)
{
	request.holdPolls = 0;
	uint32_t control = hardware.ReadVop(kVopPortBase + port * kVopPortStride + kVopPortControlWord);
	if ((control & kVopPortStandby) != 0)
		return kModeOK; // an earlier attempt already stopped the port
	uint32_t interrupt = kVopPortInterruptBase + port * kVopPortInterruptStride;
	hardware.WriteVop(interrupt + kVopPortInterruptClear, HiWord(kVopInterruptHoldValid, kVopInterruptHoldValid));
	hardware.WriteVop(interrupt + kVopPortInterruptEnable, HiWord(kVopInterruptHoldValid, kVopInterruptHoldValid));
	hardware.ClearHoldValid();
	hardware.WriteVop(kVopPortBase + port * kVopPortStride + kVopPortControlWord, kVopPortStandby);
	uint32_t result = kModeOK;
	while (!hardware.HoldValid()) {
		if (request.holdPolls >= 60) {
			result = kModeHoldTimeout;
			break;
		}
		request.holdPolls++;
		hardware.Pause(1000);
	}
	hardware.WriteVop(interrupt + kVopPortInterruptEnable, HiWord(kVopInterruptHoldValid, 0));
	return result;
}


// Port timing, post-processing and window geometry as vop2_crtc_atomic_enable,
// vop2_post_config and the EDK2 background delay do, then commit and restart.
template<class Hardware>
void
ProgramPort(Hardware& hardware, uint32_t port, uint32_t window, const ModeRequest& request)
{
	uint32_t base = kVopPortBase + port * kVopPortStride;
	uint32_t hSyncLen = request.hSyncEnd - request.hSyncStart;
	uint32_t vSyncLen = request.vSyncEnd - request.vSyncStart;
	uint32_t hActStart = request.hTotal - request.hSyncStart;
	uint32_t hActEnd = hActStart + request.hDisplay;
	uint32_t vActStart = request.vTotal - request.vSyncStart;
	uint32_t vActEnd = vActStart + request.vDisplay;
	hardware.WriteVop(base + kVopPortHTotalSyncEnd, (request.hTotal << 16) | hSyncLen);
	hardware.WriteVop(base + kVopPortHActiveStartEnd, (hActStart << 16) | hActEnd);
	hardware.WriteVop(base + kVopPortVActiveStartEnd, (vActStart << 16) | vActEnd);
	hardware.WriteVop(kVopLineFlagBase + port * 4, (vActEnd << 16) | vActEnd);
	hardware.WriteVop(base + kVopPortVTotalSyncEnd, (request.vTotal << 16) | vSyncLen);
	hardware.WriteVop(base + kVopPortMipiControl, 0);
	// vop2_post_config with 100 % margins
	hardware.WriteVop(kVopBackgroundMixBase + port * 4, kVopPreScanMaxDelay << 24);
	hardware.WriteVop(base + kVopPortPreScanTiming,
		((kVopPreScanMaxDelay + (request.hDisplay >> 1) - 1) << 16) | hSyncLen);
	hardware.WriteVop(base + kVopPortPostHActive, (hActStart << 16) | hActEnd);
	hardware.WriteVop(base + kVopPortPostVActive, (vActStart << 16) | vActEnd);
	hardware.WriteVop(base + kVopPortPostScaleFactor, 0x10001000);
	hardware.WriteVop(base + kVopPortPostScaleControl, 0);
	hardware.WriteVop(base + kVopPortDisplayBackground, 0);
	// The window keeps its buffer and stride; only the visible size changes.
	uint32_t windowBase = kVopEsmartBase + window * kVopEsmartStride;
	uint32_t geometry = ((request.vDisplay - 1) << 16) | (request.hDisplay - 1);
	hardware.WriteVop(windowBase + kVopEsmartRegionActive, geometry);
	hardware.WriteVop(windowBase + kVopEsmartRegionDisplay, geometry);
	hardware.WriteVop(windowBase + kVopEsmartRegionStart, 0);
	hardware.WriteVop(kVopConfigDone, kVopConfigDoneEnable | (1u << port) | ((1u << port) << 16));
	hardware.WriteVop(base + kVopPortControlWord, kVopOutputModeAAAA);
}


// Refreshes the AVI infoframe for the new VIC as dw_hdmi_qp does, keeps HDCP
// bypassed and the link in HDMI mode, and clears AVMUTE as EDK2 does.
template<class Hardware>
void
ConfigureHdmiTx(Hardware& hardware, uint32_t vic)
{
	hardware.WriteHdmi(kHdmiHdcp2Config, hardware.ReadHdmi(kHdmiHdcp2Config) | 1u);
	hardware.WriteHdmi(kHdmiLinkConfig, hardware.ReadHdmi(kHdmiLinkConfig) & ~(1u << 4));
	uint32_t words[5];
	BuildAviInfoframeWords(vic, words);
	for (unsigned i = 0; i < 5; i++)
		hardware.WriteHdmi(kHdmiAviContents + i * 4, words[i]);
	hardware.WriteHdmi(kHdmiPacketConfig1, hardware.ReadHdmi(kHdmiPacketConfig1) & ~(1u << 12));
	hardware.WriteHdmi(kHdmiPacketEnable,
		hardware.ReadHdmi(kHdmiPacketEnable) | kHdmiAviEnable | kHdmiGcpEnable);
	hardware.Pause(50);
	hardware.WriteHdmi(kHdmiPacketControl, 2);
}


inline bool
ModeFitsFrameBuffer(const ModeRequest& request)
{
	return request.hDisplay >= 640 && request.vDisplay >= 480
		&& request.hDisplay <= kFrameWidth && request.vDisplay <= kFrameHeight
		&& (request.hDisplay & 3) == 0
		&& request.hSyncStart > request.hDisplay && request.hSyncEnd > request.hSyncStart
		&& request.hTotal > request.hSyncEnd && request.hTotal < 0x2000
		&& request.vSyncStart > request.vDisplay && request.vSyncEnd > request.vSyncStart
		&& request.vTotal > request.vSyncEnd && request.vTotal < 0x2000
		&& (request.flags & ~(kModePositiveHSync | kModePositiveVSync)) == 0
		&& request.vic < 256;
}


// The complete change. `port` and `window` come from the acquired frame
// buffer; the caller holds the hardware lock and keeps the retrace handler.
template<class Hardware>
uint32_t
SetDisplayMode(Hardware& hardware, uint32_t port, uint32_t window, ModeRequest& request)
{
	const PllConfig* config = FindPllConfig(request.pixelClockKHz);
	if (config == NULL || !ModeFitsFrameBuffer(request))
		return kModeUnsupported;
	uint32_t result = StopPort(hardware, port, request);
	request.phase = kPhaseStopped;
	if (result != kModeOK)
		return result;
	DisablePhy(hardware);
	request.phase = kPhasePhyOff;
	result = ConfigurePhyPll(hardware, *config, request);
	if (result != kModeOK)
		return result;
	request.phase = kPhasePllReady;
	ProgramPort(hardware, port, window, request);
	request.phase = kPhasePortProgrammed;
	result = ConfigurePhyLanes(hardware, request.pixelClockKHz, request);
	if (result != kModeOK)
		return result;
	request.phase = kPhaseLanesReady;
	ConfigureHdmiTx(hardware, request.vic);
	request.phase = kPhaseInfoframes;
	uint32_t base = kVopPortBase + port * kVopPortStride;
	request.timing[0] = hardware.ReadVop(base + kVopPortHTotalSyncEnd);
	request.timing[1] = hardware.ReadVop(base + kVopPortHActiveStartEnd);
	request.timing[2] = hardware.ReadVop(base + kVopPortVTotalSyncEnd);
	request.timing[3] = hardware.ReadVop(base + kVopPortVActiveStartEnd);
	request.interfaceEnable = hardware.ReadVop(kVopSystemOffsets[kVopSystemInterfaceEnable]);
	SharedInfo check = {};
	if (!DecodePortTiming(request.timing, request.hDisplay, request.vDisplay, check)
		|| check.hTotal != request.hTotal || check.vTotal != request.vTotal
		|| check.hSyncStart != request.hSyncStart || check.hSyncEnd != request.hSyncEnd
		|| check.vSyncStart != request.vSyncStart || check.vSyncEnd != request.vSyncEnd) {
		return kModeVerifyFailed;
	}
	return kModeOK;
}

} // namespace RK3588Display

#endif // RK3588_DISPLAY_MODESET_H
