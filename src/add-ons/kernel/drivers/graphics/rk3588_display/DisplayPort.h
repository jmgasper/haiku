/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_PORT_H
#define RK3588_DISPLAY_PORT_H


#include <stdint.h>
#include <string.h>

#include "DisplayModeSet.h"


namespace RK3588Display {

// The second connector: DisplayPort TX1 (0xfde60000) through USBDP PHY1
// (0xfed90000, DP on PHY lanes 2 and 3, USB3 on lanes 0 and 1) into the RA620
// DP-to-HDMI bridge, hot-plug on GPIO3_D5. kDpProbe, opt-in through the
// rock5-itx-edk2-v1.1-display-dp-aux profile, brings the path up to the AUX
// channel the way Linux 6.18 does (phy-rockchip-usbdp.c in its DP+USB mode,
// dw-dp.c) and reads the sink's DPCD capabilities and, on request, its EDID
// over I2C-over-AUX. It never enables the main link: no training pattern, no
// video.
static const uint32_t kDpProbe = 0x52444910; // writable handle
static const uint32_t kDpVersion = 1;
static const uint32_t kDpProbeEdid = 1; // also read EDID block 0 over I2C-over-AUX
static const uint32_t kDpProbeIgnoreHotPlug = 2; // continue without a hot-plug
static const uint32_t kDpProbeTrain = 4; // also train the main link (no video)

static const uint32_t kDpOK = 0;
static const uint32_t kDpNotReady = 1; // VO0 off or a DP/PHY bus clock gated
static const uint32_t kDpNoHotPlug = 2;
static const uint32_t kDpRefclkUnsupported = 3; // the PHY reference clock is not 24 MHz
static const uint32_t kDpLcpllTimeout = 4;
static const uint32_t kDpAuxTimeout = 5; // no reply event, or the controller's timeout bit
static const uint32_t kDpAuxNack = 6; // a reply other than ACK after the retries
static const uint32_t kDpAuxShort = 7; // fewer bytes than asked for
static const uint32_t kDpEdidInvalid = 8;
static const uint32_t kDpRopllTimeout = 9;
static const uint32_t kDpTrainingFailed = 10; // clock recovery or equalization failed at the lowest rate

static const uint32_t kDpPhaseNone = 0;
static const uint32_t kDpPhasePin = 1;
static const uint32_t kDpPhaseController = 2;
static const uint32_t kDpPhaseHotPlug = 3;
static const uint32_t kDpPhasePhy = 4;
static const uint32_t kDpPhaseLanes = 5;
static const uint32_t kDpPhaseDpcd = 6;
static const uint32_t kDpPhaseEdid = 7;
static const uint32_t kDpPhaseTrain = 8;

static const unsigned kDpPmaWordCount = 7;
static const uint32_t kDpPmaOffsets[kDpPmaWordCount] = {0x288, 0x28c, 0x2d0, 0x350, 0x354,
	0x38c, 0xb84};
static const unsigned kDpResetWordCount = 4;
static const uint32_t kDpResetOffsets[kDpResetWordCount] = {0xa08, 0xa0c, 0xae0, 0xb20};

struct DpProbeRequest {
	uint32_t version; // in
	uint32_t flags; // in
	uint32_t result;
	uint32_t phase;
	uint32_t pinMuxBefore, pinMuxAfter; // GPIO3D_IOMUX_SEL_H
	uint32_t gpioLevel; // GPIO3 external port bit 29 before the mux change
	uint32_t hpdStatusBefore, hpdStatusAfter; // DW DP HPD_STATUS
	uint32_t hpdPolls;
	uint32_t resetsBefore[kDpResetWordCount]; // CRU SOFTRST_CON2, 3, 56, 72
	uint32_t resetsAfter[kDpResetWordCount];
	uint32_t refclkSelect; // PMU CRU CLKSEL_CON14
	uint32_t pmaBefore[kDpPmaWordCount]; // read only while the PMA APB is out of reset
	uint32_t pmaAfter[kDpPmaWordCount];
	uint32_t usbdpGrfBefore, usbdpGrfAfter; // USBDP PHY1 GRF CON1
	uint32_t vo0GrfBefore, vo0GrfAfter; // VO0 GRF CON2 (PHY1 lanes, AUX, HPD select)
	uint32_t cctlBefore, cctlAfter;
	uint32_t auxClockBefore, auxClockAfter; // CRU CLKSEL_CON117 (clk_aux16m_1 divider in 15:8)
	uint32_t lcpllPolls;
	uint32_t auxTransfers, auxRetries, auxPolls;
	uint32_t auxStatus; // AUX_STATUS of the last transfer
	uint32_t dpcdCount;
	uint8_t dpcd[16]; // DPCD 0x000-0x00f
	uint32_t sinkCount; // DPCD 0x200
	uint32_t edidBytes;
	uint8_t edid[128];
	// Link training (kDpProbeTrain).
	uint32_t linkRate; // DPCD bandwidth code the link trained at (6, 0x0a, 0x14, 0x1e)
	uint32_t laneCount;
	uint32_t enhancedFraming, spreadSpectrum, trainingPattern;
	uint32_t attempts; // rates tried
	uint32_t clockRecoveryLoops, equalizationLoops;
	uint32_t ropllPolls;
	uint32_t swing[2], preEmphasis[2];
	uint8_t linkStatus[6]; // DPCD 0x202-0x207 after equalization
	uint8_t reserved[2];
	uint32_t phyifAfter, cctlTrained;
	int64_t startedMicros, finishedMicros;
};

// Control words.
static const uint32_t kIocGpio3dHigh = 0x7c; // bus IOC page offset
static const uint32_t kIocHotPlugMask = 0xf0; // GPIO3_D5 function in bits 7:4
static const uint32_t kIocHotPlugFunction = 5; // dp1_hpdin_m0
static const uint32_t kGpioExternal = 0x70;
static const uint32_t kGpioHotPlugBit = 1u << 29;
static const uint32_t kUsbdpGrfCon1 = 0x4; // low_pwrn bit 13, rx_lfps bit 14
static const uint32_t kUsbdpGrfLowPowerN = 1u << 13;
static const uint32_t kUsbdpGrfRxLfps = 1u << 14;
static const uint32_t kVo0GrfPhy1 = 0x8; // PHY1: lane select 7:0, AUX dout 8, din 9, HPD sel 10, cfg 11
static const uint32_t kVo0GrfLaneMask = 0x3ff; // lane select and both AUX selects
static const uint32_t kVo0GrfLanes = (0u << (2 * 2)) | (1u << (3 * 2)); // DP lane 0 on PHY lane 2, 1 on 3
static const uint32_t kCruPhyInitReset = 0xa08; // SOFTRST_CON2 bit 15
static const uint32_t kCruPhyInitBit = 1u << 15;
static const uint32_t kCruPhyResets = 0xa0c; // SOFTRST_CON3: cmn bit 0, lane bit 1, pcs_apb bit 2
static const uint32_t kCruPhyCmnBit = 1u << 0;
static const uint32_t kCruPhyLaneBit = 1u << 1;
static const uint32_t kCruPhyPcsApbBit = 1u << 2;
static const uint32_t kCruPhyPmaApbReset = 0xb20; // SOFTRST_CON72 bit 4
static const uint32_t kCruPhyPmaApbBit = 1u << 4;
static const uint32_t kCruRefclkSelect = 0x30000 + 0x300 + 14 * 4; // PMU CRU CLKSEL_CON14
static const uint32_t kCruRefclkMask = (3u << 7) | 0x7f; // mux 8:7 (0 = xin24m), divider 6:0
// clk_aux16m_1 = GPLL (1188 MHz) / (field + 1): Linux assigns 16 MHz, which its
// divider rounding (never above the request) makes 1188 / 75 = 15.84 MHz.
static const uint32_t kCruAuxClockSelect = 0x300 + 117 * 4; // CLKSEL_CON117
static const uint32_t kCruAuxClockMask = 0xff00;
static const uint32_t kCruAuxClockDivider = 74u << 8;

// USBDP PHY PMA (phy base + 0x8000).
static const uint32_t kPmaBase = 0x8000;
static const uint32_t kPmaMapSize = 0x3000; // Linux max_register 0x20dc
static const uint32_t kPmaLaneMux = 0x288; // bits 7:4 lane mux (1 = DP), 3:0 DP lane enable
static const uint32_t kPmaLaneMuxDp = (1u << (2 + 4)) | (1u << (3 + 4));
static const uint32_t kPmaLaneEnableDp = (1u << 2) | (1u << 3);
static const uint32_t kPmaLcpllDone = 0x350; // bit 7 lock, bit 6 AFC done
static const uint32_t kPmaLcpllReady = (1u << 7) | (1u << 6);
static const uint32_t kPmaDpReset = 0x38c; // bit 3 DP init rstn, bit 2 DP cmn rstn
static const uint32_t kPmaDpInitRstn = 1u << 3;

// DW DP TX.
static const uint32_t kDpCctl = 0x200;
static const uint32_t kDpCctlFastLinkTrain = 1u << 2;
static const uint32_t kDpAuxCommand = 0xb00;
static const uint32_t kDpAuxStatusWord = 0xb04;
static const uint32_t kDpAuxStatusTimeout = 1u << 17;
static const uint32_t kDpAuxData = 0xb08;
static const uint32_t kDpGeneralInterrupt = 0xd00;
static const uint32_t kDpAuxReplyEvent = 1u << 1;
static const uint32_t kDpGeneralInterruptEnable = 0xd04;
static const uint32_t kDpHotPlugStatus = 0xd08;
static const uint32_t kDpHotPlugStatePlug = 7; // bits 11:9
static const uint32_t kDpHotPlugInterruptEnable = 0xd0c;

static const uint32_t kAuxI2cWrite = 0x0;
static const uint32_t kAuxI2cRead = 0x1;
static const uint32_t kAuxI2cMot = 0x4;
static const uint32_t kAuxNativeRead = 0x9;
static const uint32_t kAuxNativeWrite = 0x8;
static const uint32_t kAuxEdidAddress = 0x50;

static const unsigned kDpResetPauseMicros = 10000;
static const unsigned kDpAuxReadyMicros = 10000;
static const unsigned kDpPollMicros = 200;
static const uint32_t kDpHotPlugPollLimit = 2500; // 500 ms (the board took 198 ms)
static const uint32_t kDpLcpllPollLimit = 500; // 100 ms
static const unsigned kDpAuxPollMicros = 50;
static const uint32_t kDpAuxPollLimit = 200; // 10 ms per transfer
static const uint32_t kDpAuxRetryLimit = 7;

// DW DP PHY interface and PMA rate words.
static const uint32_t kDpPhyInterface = 0xa00; // 20:17 power-down, 11:8 transmit, 7:6 lanes/2, 3:0 pattern
static const uint32_t kDpPhyPowerDownMask = 0xfu << 17;
static const uint32_t kDpPhyTransmitMask = 0xfu << 8;
static const uint32_t kDpPhyLanesMask = 3u << 6;
static const uint32_t kDpPhyPatternMask = 0xf;
static const uint32_t kDpCctlEnhancedFraming = 1u << 1;
static const uint32_t kDpCctlScrambleDisable = 1u << 0;
static const uint32_t kPmaDpLink = 0x28c; // 6:5 link bandwidth
static const uint32_t kPmaSsc = 0x2d0; // bit 1 ROPLL spread spectrum
static const uint32_t kPmaDpCmnRstn = 1u << 2; // in kPmaDpReset
static const uint32_t kPmaRopllDone = 0x354; // bit 1 lock, bit 0 AFC done
static const uint32_t kDpRopllPollLimit = 50; // 1 ms
static const unsigned kDpPhyLane[2] = {2, 3}; // PHY lanes of DP lanes 0 and 1

struct DpDrive {
	uint8_t reg0204, reg0205, reg0206, reg0207;
};

// Linux rk3588_dp_tx_drv_ctrl_rbr_hbr / _hbr2 / _hbr3 [swing][pre-emphasis].
static const DpDrive kDpDriveRbrHbr[4][4] = {
	{{0x20, 0x10, 0x42, 0xe5}, {0x26, 0x14, 0x42, 0xe5}, {0x29, 0x18, 0x42, 0xe5}, {0x2b, 0x1c, 0x43, 0xe7}},
	{{0x23, 0x10, 0x42, 0xe7}, {0x2a, 0x17, 0x43, 0xe7}, {0x2b, 0x1a, 0x43, 0xe7}},
	{{0x27, 0x10, 0x42, 0xe7}, {0x2b, 0x17, 0x43, 0xe7}},
	{{0x29, 0x10, 0x43, 0xe7}},
};
static const DpDrive kDpDriveHbr2[4][4] = {
	{{0x21, 0x10, 0x42, 0xe5}, {0x26, 0x14, 0x42, 0xe5}, {0x26, 0x16, 0x43, 0xe5}, {0x2a, 0x19, 0x43, 0xe7}},
	{{0x24, 0x10, 0x42, 0xe7}, {0x2a, 0x17, 0x43, 0xe7}, {0x2b, 0x1a, 0x43, 0xe7}},
	{{0x28, 0x10, 0x42, 0xe7}, {0x2b, 0x17, 0x43, 0xe7}},
	{{0x28, 0x10, 0x43, 0xe7}},
};
static const DpDrive kDpDriveHbr3[4][4] = {
	{{0x21, 0x10, 0x42, 0xe5}, {0x26, 0x14, 0x42, 0xe5}, {0x26, 0x16, 0x43, 0xe5}, {0x29, 0x18, 0x43, 0xe7}},
	{{0x24, 0x10, 0x42, 0xe7}, {0x2a, 0x18, 0x43, 0xe7}, {0x2b, 0x1b, 0x43, 0xe7}},
	{{0x27, 0x10, 0x42, 0xe7}, {0x2b, 0x18, 0x43, 0xe7}},
	{{0x28, 0x10, 0x43, 0xe7}},
};

// Linux rk_udphy_init_sequence and rk_udphy_24m_refclk_cfg (PMA offsets).
static const RegisterValue kUdphyInitSequence[] = {
	{0x0104, 0x44}, {0x0234, 0xe8}, {0x0248, 0x44}, {0x028c, 0x18}, {0x081c, 0xe5},
	{0x0878, 0x00}, {0x0994, 0x1c}, {0x0af0, 0x00}, {0x181c, 0xe5}, {0x1878, 0x00},
	{0x1994, 0x1c}, {0x1af0, 0x00}, {0x0428, 0x60}, {0x0d58, 0x33}, {0x1d58, 0x33},
	{0x0990, 0x74}, {0x0d64, 0x17}, {0x08c8, 0x13}, {0x1990, 0x74}, {0x1d64, 0x17},
	{0x18c8, 0x13}, {0x0d90, 0x40}, {0x0da8, 0x40}, {0x0dc0, 0x40}, {0x0dd8, 0x40},
	{0x1d90, 0x40}, {0x1da8, 0x40}, {0x1dc0, 0x40}, {0x1dd8, 0x40}, {0x03c0, 0x30},
	{0x03c4, 0x06}, {0x0e10, 0x00}, {0x1e10, 0x00}, {0x043c, 0x0f}, {0x0d2c, 0xff},
	{0x1d2c, 0xff}, {0x0d34, 0x0f}, {0x1d34, 0x0f}, {0x08fc, 0x2a}, {0x0914, 0x28},
	{0x0a30, 0x03}, {0x0e38, 0x03}, {0x0ecc, 0x27}, {0x0ed0, 0x22}, {0x0ed4, 0x26},
	{0x18fc, 0x2a}, {0x1914, 0x28}, {0x1a30, 0x03}, {0x1e38, 0x03}, {0x1ecc, 0x27},
	{0x1ed0, 0x22}, {0x1ed4, 0x26}, {0x0048, 0x0f}, {0x0060, 0x3c}, {0x0064, 0xf7},
	{0x006c, 0x20}, {0x0070, 0x7d}, {0x0074, 0x68}, {0x0af4, 0x1a}, {0x1af4, 0x1a},
	{0x0440, 0x3f}, {0x10d4, 0x08}, {0x20d4, 0x08}, {0x00d4, 0x30}, {0x0024, 0x6e},
};
static const RegisterValue kUdphyRefclk24m[] = {
	{0x0090, 0x68}, {0x0094, 0x68}, {0x0128, 0x24}, {0x012c, 0x44}, {0x0130, 0x3f},
	{0x0134, 0x44}, {0x015c, 0xa9}, {0x0160, 0x71}, {0x0164, 0x71}, {0x0168, 0xa9},
	{0x0174, 0xa9}, {0x0178, 0x71}, {0x017c, 0x71}, {0x0180, 0xa9}, {0x018c, 0x41},
	{0x0190, 0x00}, {0x0194, 0x05}, {0x01ac, 0x2a}, {0x01b0, 0x17}, {0x01b4, 0x17},
	{0x01b8, 0x2a}, {0x01c8, 0x04}, {0x01cc, 0x08}, {0x01d0, 0x08}, {0x01d4, 0x04},
	{0x01d8, 0x20}, {0x01dc, 0x01}, {0x01e0, 0x09}, {0x01e4, 0x03}, {0x01f0, 0x29},
	{0x01f4, 0x02}, {0x01f8, 0x02}, {0x01fc, 0x29}, {0x0208, 0x2a}, {0x020c, 0x17},
	{0x0210, 0x17}, {0x0214, 0x2a}, {0x0224, 0x20}, {0x03f0, 0x0a}, {0x03f4, 0x07},
	{0x03f8, 0x07}, {0x03fc, 0x0c}, {0x0404, 0x12}, {0x0408, 0x1a}, {0x040c, 0x1a},
	{0x0410, 0x3f}, {0x0ce0, 0x68}, {0x0ce8, 0xd0}, {0x0cf0, 0x87}, {0x0cf8, 0x70},
	{0x0d00, 0x70}, {0x0d08, 0xa9}, {0x1ce0, 0x68}, {0x1ce8, 0xd0}, {0x1cf0, 0x87},
	{0x1cf8, 0x70}, {0x1d00, 0x70}, {0x1d08, 0xa9}, {0x0a3c, 0xd0}, {0x0a44, 0xd0},
	{0x0a48, 0x01}, {0x0a4c, 0x0d}, {0x0a54, 0xe0}, {0x0a5c, 0xe0}, {0x0a64, 0xa8},
	{0x1a3c, 0xd0}, {0x1a44, 0xd0}, {0x1a48, 0x01}, {0x1a4c, 0x0d}, {0x1a54, 0xe0},
	{0x1a5c, 0xe0}, {0x1a64, 0xa8},
};


template<class Hardware>
void
DpPause(Hardware& hardware, unsigned micros)
{
	// The adapters' Pause spins at most a millisecond at a time.
	while (micros > 0) {
		unsigned step = micros > 1000 ? 1000 : micros;
		hardware.Pause(step);
		micros -= step;
	}
}


template<class Hardware>
void
DpReadPma(Hardware& hardware, uint32_t* words)
{
	for (unsigned i = 0; i < kDpPmaWordCount; i++)
		words[i] = hardware.ReadPma(kDpPmaOffsets[i]);
}


// One AUX transaction (dw_dp_aux_transfer): the data words for a write, the
// command, the reply event polled instead of the interrupt, the status and
// the data words for a read. Defer and NACK replies are retried.
template<class Hardware>
uint32_t
DpAuxTransfer(Hardware& hardware, DpProbeRequest& request, uint32_t type, uint32_t address,
	uint8_t* buffer, uint32_t size)
{
	for (uint32_t attempt = 0; ; attempt++) {
		request.auxTransfers++;
		if (size > 0 && (type & 1) == 0) {
			for (uint32_t i = 0; i < (size + 3) / 4; i++) {
				uint32_t value = 0;
				for (uint32_t j = 0; j < 4 && i * 4 + j < size; j++)
					value |= (uint32_t)buffer[i * 4 + j] << (j * 8);
				hardware.WriteDp(kDpAuxData + i * 4, value);
			}
		}
		hardware.WriteDp(kDpGeneralInterrupt, kDpAuxReplyEvent);
		uint32_t command = (type << 28) | ((address & 0xfffff) << 8)
			| (size > 0 ? size - 1 : (1u << 4));
		hardware.WriteDp(kDpAuxCommand, command);
		uint32_t polls = 0;
		while ((hardware.ReadDp(kDpGeneralInterrupt) & kDpAuxReplyEvent) == 0) {
			if (polls >= kDpAuxPollLimit) {
				request.auxPolls += polls;
				return kDpAuxTimeout;
			}
			polls++;
			hardware.Pause(kDpAuxPollMicros);
		}
		request.auxPolls += polls;
		hardware.WriteDp(kDpGeneralInterrupt, kDpAuxReplyEvent);
		uint32_t status = hardware.ReadDp(kDpAuxStatusWord);
		request.auxStatus = status;
		if ((status & kDpAuxStatusTimeout) != 0)
			return kDpAuxTimeout;
		uint32_t reply = (status >> 4) & 0xf;
		if (reply != 0) {
			if (attempt >= kDpAuxRetryLimit)
				return kDpAuxNack;
			request.auxRetries++;
			DpPause(hardware, 500);
			continue;
		}
		if (size > 0 && (type & 1) != 0) {
			uint32_t count = ((status >> 19) & 0x1f);
			if (count == 0 || count - 1 != size)
				return kDpAuxShort;
			for (uint32_t i = 0; i < (size + 3) / 4; i++) {
				uint32_t value = hardware.ReadDp(kDpAuxData + i * 4);
				for (uint32_t j = 0; j < 4 && i * 4 + j < size; j++)
					buffer[i * 4 + j] = (uint8_t)(value >> (j * 8));
			}
		}
		return kDpOK;
	}
}


// The probe. Preconditions (VO0 on, pclk_dp1, the AUX clock, pclk_usbdpphy1
// and pclk_gpio3 ungated) are the caller's; every register it touches is
// reported before and after.
template<class Hardware>
uint32_t
DpProbeSink(Hardware& hardware, DpProbeRequest& request)
{
	request.startedMicros = hardware.Now();
	for (unsigned i = 0; i < kDpResetWordCount; i++)
		request.resetsBefore[i] = hardware.ReadCru(kDpResetOffsets[i]);
	request.refclkSelect = hardware.ReadCru(kCruRefclkSelect);
	bool pmaReadable = (request.resetsBefore[3] & kCruPhyPmaApbBit) == 0;
	if (pmaReadable)
		DpReadPma(hardware, request.pmaBefore);
	request.usbdpGrfBefore = hardware.ReadUsbdpGrf(kUsbdpGrfCon1);
	request.vo0GrfBefore = hardware.ReadVo0Grf(kVo0GrfPhy1);
	request.cctlBefore = hardware.ReadDp(kDpCctl);
	request.hpdStatusBefore = hardware.ReadDp(kDpHotPlugStatus);
	request.gpioLevel = (hardware.ReadGpio(kGpioExternal) & kGpioHotPlugBit) != 0 ? 1 : 0;

	// The hot-plug pin to the controller (Linux pinctrl dp1m0_pins).
	request.phase = kDpPhasePin;
	request.pinMuxBefore = hardware.ReadIoc(kIocGpio3dHigh);
	if (((request.pinMuxBefore & kIocHotPlugMask) >> 4) != kIocHotPlugFunction) {
		hardware.WriteIoc(kIocGpio3dHigh,
			HiWord(kIocHotPlugMask, kIocHotPlugFunction << 4));
	}
	request.pinMuxAfter = hardware.ReadIoc(kIocGpio3dHigh);

	// The AUX clock at Linux' 16 MHz assignment, then dw_dp_init_hw: no
	// fast link training, hot-plug and AUX reply events.
	request.phase = kDpPhaseController;
	request.auxClockBefore = hardware.ReadCru(kCruAuxClockSelect);
	if ((request.auxClockBefore & kCruAuxClockMask) != kCruAuxClockDivider)
		hardware.WriteCru(kCruAuxClockSelect, HiWord(kCruAuxClockMask, kCruAuxClockDivider));
	request.auxClockAfter = hardware.ReadCru(kCruAuxClockSelect);
	hardware.WriteDp(kDpCctl, request.cctlBefore & ~kDpCctlFastLinkTrain);
	hardware.WriteDp(kDpHotPlugInterruptEnable,
		hardware.ReadDp(kDpHotPlugInterruptEnable) | 0x7);
	hardware.WriteDp(kDpGeneralInterruptEnable,
		hardware.ReadDp(kDpGeneralInterruptEnable) | 0x3);
	request.cctlAfter = hardware.ReadDp(kDpCctl);

	// The controller debounces the pin into its PLUG state.
	request.phase = kDpPhaseHotPlug;
	request.hpdPolls = 0;
	while (((hardware.ReadDp(kDpHotPlugStatus) >> 9) & 7) != kDpHotPlugStatePlug) {
		if (request.hpdPolls >= kDpHotPlugPollLimit)
			break;
		request.hpdPolls++;
		hardware.Pause(kDpPollMicros);
	}
	request.hpdStatusAfter = hardware.ReadDp(kDpHotPlugStatus);
	bool plugged = ((request.hpdStatusAfter >> 9) & 7) == kDpHotPlugStatePlug;
	if (!plugged && (request.flags & kDpProbeIgnoreHotPlug) == 0)
		return kDpNoHotPlug;

	// rk_udphy_init in the DP+USB mode the board's two DP lanes select.
	request.phase = kDpPhasePhy;
	if ((request.refclkSelect & kCruRefclkMask) != 0)
		return kDpRefclkUnsupported;
	hardware.WriteCru(kCruPhyInitReset, HiWord(kCruPhyInitBit, kCruPhyInitBit));
	hardware.WriteCru(kCruPhyResets, HiWord(kCruPhyCmnBit | kCruPhyLaneBit | kCruPhyPcsApbBit,
		kCruPhyCmnBit | kCruPhyLaneBit | kCruPhyPcsApbBit));
	hardware.WriteCru(kCruPhyPmaApbReset, HiWord(kCruPhyPmaApbBit, kCruPhyPmaApbBit));
	DpPause(hardware, kDpResetPauseMicros);
	hardware.WriteUsbdpGrf(kUsbdpGrfCon1, HiWord(kUsbdpGrfRxLfps, kUsbdpGrfRxLfps));
	hardware.WriteUsbdpGrf(kUsbdpGrfCon1, HiWord(kUsbdpGrfLowPowerN, kUsbdpGrfLowPowerN));
	hardware.WriteCru(kCruPhyPmaApbReset, HiWord(kCruPhyPmaApbBit, 0));
	hardware.WriteCru(kCruPhyResets, HiWord(kCruPhyPcsApbBit, 0));
	for (unsigned i = 0; i < sizeof(kUdphyInitSequence) / sizeof(kUdphyInitSequence[0]); i++)
		hardware.WritePma(kUdphyInitSequence[i].offset, kUdphyInitSequence[i].value);
	for (unsigned i = 0; i < sizeof(kUdphyRefclk24m) / sizeof(kUdphyRefclk24m[0]); i++)
		hardware.WritePma(kUdphyRefclk24m[i].offset, kUdphyRefclk24m[i].value);
	hardware.WritePma(kPmaLaneMux, (hardware.ReadPma(kPmaLaneMux) & ~0xffu) | kPmaLaneMuxDp);
	hardware.WriteCru(kCruPhyInitReset, HiWord(kCruPhyInitBit, 0));
	hardware.WritePma(kPmaDpReset, hardware.ReadPma(kPmaDpReset) | kPmaDpInitRstn);
	hardware.Pause(20); // the datasheet's 200 ns, rounded up to the spin granularity
	hardware.WriteCru(kCruPhyResets, HiWord(kCruPhyCmnBit | kCruPhyLaneBit, 0));
	request.lcpllPolls = 0;
	while ((hardware.ReadPma(kPmaLcpllDone) & kPmaLcpllReady) != kPmaLcpllReady) {
		if (request.lcpllPolls >= kDpLcpllPollLimit)
			return kDpLcpllTimeout;
		request.lcpllPolls++;
		hardware.Pause(kDpPollMicros);
	}

	// rk_udphy_dp_phy_power_on: DP lanes 2 and 3 enabled and routed, then
	// the time the AUX PHY needs before its first transaction.
	request.phase = kDpPhaseLanes;
	hardware.WritePma(kPmaLaneMux, (hardware.ReadPma(kPmaLaneMux) & ~0xfu) | kPmaLaneEnableDp);
	hardware.WriteVo0Grf(kVo0GrfPhy1, HiWord(kVo0GrfLaneMask, kVo0GrfLanes));
	DpPause(hardware, kDpAuxReadyMicros);
	request.usbdpGrfAfter = hardware.ReadUsbdpGrf(kUsbdpGrfCon1);
	request.vo0GrfAfter = hardware.ReadVo0Grf(kVo0GrfPhy1);
	DpReadPma(hardware, request.pmaAfter);

	// The sink's receiver capabilities and sink count.
	request.phase = kDpPhaseDpcd;
	uint32_t result = DpAuxTransfer(hardware, request, kAuxNativeRead, 0x000, request.dpcd, 16);
	if (result != kDpOK)
		return result;
	request.dpcdCount = 16;
	uint8_t sinkCount = 0;
	result = DpAuxTransfer(hardware, request, kAuxNativeRead, 0x200, &sinkCount, 1);
	if (result != kDpOK)
		return result;
	request.sinkCount = sinkCount;

	if ((request.flags & kDpProbeEdid) != 0) {
		// EDID block 0 over I2C-over-AUX: offset 0, eight 16-byte reads with
		// middle-of-transaction set, and an address-only read to stop.
		request.phase = kDpPhaseEdid;
		uint8_t offset = 0;
		result = DpAuxTransfer(hardware, request, kAuxI2cWrite | kAuxI2cMot, kAuxEdidAddress,
			&offset, 1);
		for (uint32_t block = 0; result == kDpOK && block < 8; block++) {
			result = DpAuxTransfer(hardware, request, kAuxI2cRead | kAuxI2cMot, kAuxEdidAddress,
				request.edid + block * 16, 16);
			if (result == kDpOK)
				request.edidBytes += 16;
		}
		uint32_t stop = DpAuxTransfer(hardware, request, kAuxI2cRead, kAuxEdidAddress, NULL, 0);
		if (result != kDpOK)
			return result;
		if (stop != kDpOK)
			return stop;
		static const uint8_t kHeader[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
		uint8_t sum = 0;
		for (unsigned i = 0; i < 128; i++)
			sum += request.edid[i];
		if (memcmp(request.edid, kHeader, 8) != 0 || sum != 0)
			return kDpEdidInvalid;
	}
	if ((request.flags & kDpProbeTrain) != 0)
		return DpTrainLink(hardware, request);
	return kDpOK;
}


inline uint32_t
DpBandwidthIndex(uint32_t code)
{
	return code == 0x1e ? 3 : code == 0x14 ? 2 : code == 0x0a ? 1 : 0;
}


inline uint32_t
DpLowerRate(uint32_t code)
{
	return code == 0x1e ? 0x14 : code == 0x14 ? 0x0a : code == 0x0a ? 0x06 : 0;
}


inline uint32_t
DpVoltageMax(uint32_t preEmphasis)
{
	return preEmphasis >= 3 ? 0 : 3 - preEmphasis;
}


template<class Hardware>
uint32_t
DpWriteDpcd(Hardware& hardware, DpProbeRequest& request, uint32_t address, const uint8_t* data,
	uint32_t size)
{
	uint8_t buffer[16];
	memcpy(buffer, data, size);
	return DpAuxTransfer(hardware, request, kAuxNativeWrite, address, buffer, size);
}


// rk_udphy_dp_set_voltage and the DPCD TRAINING_LANEx_SET words
// (dw_dp_link_train_update_vs_emph).
template<class Hardware>
uint32_t
DpApplyDrive(Hardware& hardware, DpProbeRequest& request, bool swingMax[2], bool preMax[2])
{
	uint32_t index = DpBandwidthIndex(request.linkRate);
	const DpDrive (*table)[4] = index == 3 ? kDpDriveHbr3 : index == 2 ? kDpDriveHbr2 : kDpDriveRbrHbr;
	uint8_t lanes[2];
	for (uint32_t lane = 0; lane < request.laneCount; lane++) {
		uint32_t offset = 0x800 * kDpPhyLane[lane];
		uint32_t clock = hardware.ReadPma(0x854 + offset);
		// TXCLK inversion follows the lane's DP mux at RBR and HBR, off above.
		clock = index <= 1 ? (clock | 2u) : (clock & ~2u);
		hardware.WritePma(0x854 + offset, clock);
		const DpDrive& drive = table[request.swing[lane]][request.preEmphasis[lane]];
		hardware.WritePma(0x810 + offset, drive.reg0204);
		hardware.WritePma(0x814 + offset, drive.reg0205);
		hardware.WritePma(0x818 + offset, drive.reg0206);
		hardware.WritePma(0x81c + offset, drive.reg0207);
		lanes[lane] = (uint8_t)(request.swing[lane] | (request.preEmphasis[lane] << 3)
			| (swingMax[lane] ? 1u << 2 : 0) | (preMax[lane] ? 1u << 5 : 0));
	}
	return DpWriteDpcd(hardware, request, 0x103, lanes, request.laneCount);
}


// dw_dp_link_get_adjustments: the sink's requested swing and pre-emphasis,
// clamped as Linux does. Returns whether anything changed.
inline bool
DpAdjust(DpProbeRequest& request, const uint8_t status[6], bool swingMax[2], bool preMax[2])
{
	bool changed = false;
	for (uint32_t lane = 0; lane < request.laneCount; lane++) {
		uint8_t request8 = status[4 + lane / 2] >> (4 * (lane & 1));
		uint32_t swing = request8 & 3, pre = (request8 >> 2) & 3;
		if (swing != request.swing[lane] || pre != request.preEmphasis[lane])
			changed = true;
		preMax[lane] = pre >= 3;
		request.preEmphasis[lane] = pre >= 3 ? 3 : pre;
		uint32_t limit = DpVoltageMax(request.preEmphasis[lane]);
		if (swing > limit)
			swing = limit;
		swingMax[lane] = swing >= 3;
		request.swing[lane] = swing >= 3 ? 3 : swing;
	}
	return changed;
}


inline bool
DpClockRecovered(const uint8_t status[6], uint32_t lanes)
{
	for (uint32_t lane = 0; lane < lanes; lane++) {
		if (((status[lane / 2] >> (4 * (lane & 1))) & 1) == 0)
			return false;
	}
	return true;
}


inline bool
DpEqualized(const uint8_t status[6], uint32_t lanes)
{
	if ((status[2] & 1) == 0) // INTERLANE_ALIGN_DONE
		return false;
	for (uint32_t lane = 0; lane < lanes; lane++) {
		if (((status[lane / 2] >> (4 * (lane & 1))) & 7) != 7)
			return false;
	}
	return true;
}


template<class Hardware>
uint32_t
DpSetPattern(Hardware& hardware, DpProbeRequest& request, uint32_t pattern)
{
	uint32_t cctl = hardware.ReadDp(kDpCctl);
	bool scrambleOff = pattern != 0 && pattern != 4;
	hardware.WriteDp(kDpCctl, scrambleOff ? (cctl | kDpCctlScrambleDisable) : (cctl & ~kDpCctlScrambleDisable));
	hardware.WriteDp(kDpPhyInterface, (hardware.ReadDp(kDpPhyInterface) & ~kDpPhyPatternMask) | pattern);
	uint8_t value = (uint8_t)((pattern == 4 ? 7 : pattern) | (scrambleOff ? 0x20 : 0));
	return DpWriteDpcd(hardware, request, 0x102, &value, 1);
}


// dw_dp_link_configure: the PHY at the rate (P3, ROPLL relock, P0), the
// lanes transmitting, framing, and the sink told the rate and lane count.
template<class Hardware>
uint32_t
DpConfigureLink(Hardware& hardware, DpProbeRequest& request)
{
	hardware.WriteDp(kDpPhyInterface,
		(hardware.ReadDp(kDpPhyInterface) & ~kDpPhyPowerDownMask) | (3u << 17));
	hardware.WritePma(kPmaDpReset, hardware.ReadPma(kPmaDpReset) & ~kPmaDpCmnRstn);
	hardware.WritePma(kPmaDpLink, (hardware.ReadPma(kPmaDpLink) & ~(3u << 5))
		| (DpBandwidthIndex(request.linkRate) << 5));
	hardware.WritePma(kPmaSsc, (hardware.ReadPma(kPmaSsc) & ~2u) | (request.spreadSpectrum ? 2u : 0));
	hardware.WritePma(kPmaDpReset, hardware.ReadPma(kPmaDpReset) | kPmaDpCmnRstn);
	request.ropllPolls = 0;
	while ((hardware.ReadPma(kPmaRopllDone) & 3) != 3) {
		if (request.ropllPolls >= kDpRopllPollLimit)
			return kDpRopllTimeout;
		request.ropllPolls++;
		hardware.Pause(20);
	}
	uint32_t phy = hardware.ReadDp(kDpPhyInterface);
	phy = (phy & ~kDpPhyLanesMask) | ((request.laneCount / 2) << 6);
	hardware.WriteDp(kDpPhyInterface, phy);
	phy &= ~kDpPhyPowerDownMask;
	hardware.WriteDp(kDpPhyInterface, phy);
	hardware.WriteDp(kDpPhyInterface, (phy & ~kDpPhyTransmitMask) | (((1u << request.laneCount) - 1) << 8));
	uint32_t cctl = hardware.ReadDp(kDpCctl);
	hardware.WriteDp(kDpCctl, request.enhancedFraming ? (cctl | kDpCctlEnhancedFraming)
		: (cctl & ~kDpCctlEnhancedFraming));
	uint8_t link[2] = {(uint8_t)request.linkRate,
		(uint8_t)(request.laneCount | (request.enhancedFraming ? 0x80 : 0))};
	uint32_t result = DpWriteDpcd(hardware, request, 0x100, link, 2);
	if (result != kDpOK)
		return result;
	uint8_t spread[2] = {(uint8_t)(request.spreadSpectrum ? 0x10 : 0),
		(uint8_t)((request.dpcd[6] & 1) != 0 ? 1 : 0)};
	return DpWriteDpcd(hardware, request, 0x107, spread, 2);
}


// dw_dp_link_train_full with the rate downgrade, from the sink's
// capabilities: at most the two lanes the board wires.
template<class Hardware>
uint32_t
DpTrainLink(Hardware& hardware, DpProbeRequest& request)
{
	request.phase = kDpPhaseTrain;
	uint8_t powerUp = 1;
	uint32_t result = DpWriteDpcd(hardware, request, 0x600, &powerUp, 1);
	if (result != kDpOK)
		return result;
	DpPause(hardware, 1000);
	uint32_t rate = request.dpcd[1];
	if (rate != 0x06 && rate != 0x0a && rate != 0x14 && rate != 0x1e)
		rate = 0x0a;
	uint32_t lanes = request.dpcd[2] & 0x1f;
	request.laneCount = lanes >= 2 ? 2 : 1;
	request.enhancedFraming = (request.dpcd[2] >> 7) & 1;
	request.spreadSpectrum = request.dpcd[3] & 1;
	uint32_t interval = request.dpcd[0xe] & 0x7f;
	unsigned crDelay = interval == 0 ? 100 : (interval > 4 ? 4 : interval) * 4000;
	unsigned eqDelay = interval == 0 ? 400 : (interval > 4 ? 4 : interval) * 4000;
	uint32_t eqPattern = (request.dpcd[3] & 0x80) != 0 ? 4 : (request.dpcd[2] & 0x40) != 0 ? 3 : 2;
	request.trainingPattern = eqPattern;
	for (; rate != 0; rate = DpLowerRate(rate)) {
		request.attempts++;
		request.linkRate = rate;
		for (uint32_t lane = 0; lane < 2; lane++)
			request.swing[lane] = request.preEmphasis[lane] = 0;
		bool swingMax[2] = {false, false}, preMax[2] = {false, false};
		result = DpConfigureLink(hardware, request);
		if (result != kDpOK)
			return result;
		result = DpSetPattern(hardware, request, 1);
		if (result != kDpOK)
			return result;
		bool recovered = false;
		uint32_t unchanged = 0;
		for (uint32_t loop = 0; loop < 32 && !recovered; loop++) {
			request.clockRecoveryLoops++;
			result = DpApplyDrive(hardware, request, swingMax, preMax);
			if (result == kDpOK) {
				DpPause(hardware, crDelay);
				result = DpAuxTransfer(hardware, request, kAuxNativeRead, 0x202, request.linkStatus, 6);
			}
			if (result != kDpOK)
				break;
			if (DpClockRecovered(request.linkStatus, request.laneCount)) {
				recovered = true;
				break;
			}
			if (DpAdjust(request, request.linkStatus, swingMax, preMax))
				unchanged = 0;
			else if (++unchanged == 5)
				break;
		}
		bool equalized = false;
		if (result == kDpOK && recovered) {
			result = DpSetPattern(hardware, request, eqPattern);
			for (uint32_t tries = 1; result == kDpOK && tries < 5; tries++) {
				request.equalizationLoops++;
				result = DpApplyDrive(hardware, request, swingMax, preMax);
				if (result == kDpOK) {
					DpPause(hardware, eqDelay);
					result = DpAuxTransfer(hardware, request, kAuxNativeRead, 0x202, request.linkStatus, 6);
				}
				if (result != kDpOK || !DpClockRecovered(request.linkStatus, request.laneCount))
					break;
				if (DpEqualized(request.linkStatus, request.laneCount)) {
					equalized = true;
					break;
				}
				DpAdjust(request, request.linkStatus, swingMax, preMax);
			}
		}
		uint32_t disabled = DpSetPattern(hardware, request, 0);
		if (result != kDpOK)
			return result;
		if (disabled != kDpOK)
			return disabled;
		if (equalized) {
			request.phyifAfter = hardware.ReadDp(kDpPhyInterface);
			request.cctlTrained = hardware.ReadDp(kDpCctl);
			return kDpOK;
		}
	}
	request.phyifAfter = hardware.ReadDp(kDpPhyInterface);
	request.cctlTrained = hardware.ReadDp(kDpCctl);
	return kDpTrainingFailed;
}


template<class Hardware>
void
DpProbeFinish(Hardware& hardware, DpProbeRequest& request)
{
	for (unsigned i = 0; i < kDpResetWordCount; i++)
		request.resetsAfter[i] = hardware.ReadCru(kDpResetOffsets[i]);
	request.finishedMicros = hardware.Now();
}

} // namespace RK3588Display

#endif // RK3588_DISPLAY_PORT_H
