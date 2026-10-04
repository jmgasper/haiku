/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_DISPLAY_EDID_H
#define RK3588_DISPLAY_EDID_H


#include "DisplayObservation.h"


namespace RK3588Display {

// Opt-in EDID read over the DW HDMI QP TX1 I2C master (DDC). This is the
// first display write path: it touches only the controller's I2C master and
// interrupt-status registers, never video, PHY, clock or power state.
static const uint32_t kReadEdid = 0x52444902;
static const uint32_t kEdidVersion = 1;
static const uint32_t kEdidBlockBytes = 128;
static const uint32_t kEdidMaxBlocks = 4;

static const uint32_t kEdidOK = 0;
static const uint32_t kEdidNotReady = 1; // VO1 off or HDMI APB clock gated
static const uint32_t kEdidNoHotPlug = 2; // HDMI TX1 hot-plug level low
static const uint32_t kEdidTimeout = 3;
static const uint32_t kEdidNack = 4;
static const uint32_t kEdidInvalidBlock = 5;
static const uint32_t kEdidPoweredOff = 6; // DPMS off: the HDMI TX registers are unreachable

static const uint32_t kEdidSegmentUsed = 1;
static const uint32_t kEdidMasterReset = 2; // the master was reset after an error

// DW HDMI QP register offsets (Linux drivers/gpu/drm/bridge/synopsys/dw-hdmi-qp.h).
static const uint32_t kHdmiEdidMapSize = 0x4000;
static const uint32_t kI2cmControl0 = 0x0ec;
static const uint32_t kI2cmStatus0 = 0x0f0;
static const uint32_t kI2cmInterfaceControl0 = 0x0f4;
static const uint32_t kI2cmInterfaceControl1 = 0x0f8;
static const uint32_t kI2cmReadData = 0x10c;
static const uint32_t kMainUnit1InterruptStatus = 0x3020;
static const uint32_t kMainUnit1InterruptMask = 0x3024;
static const uint32_t kMainUnit1InterruptClear = 0x3028;
static const uint32_t kI2cmAddressMask = 0xff000; // register address, shift 12
static const uint32_t kI2cmSlaveMask = 0xfe0; // slave address, shift 5
static const uint32_t kI2cmWriteMask = 0x1e;
static const uint32_t kI2cmExtendedRead = 1u << 4;
static const uint32_t kI2cmFastRead = 1u << 2;
static const uint32_t kI2cmSegmentPointerMask = 0x7f80; // shift 7
static const uint32_t kI2cmSegmentAddressMask = 0x7f;
static const uint32_t kI2cmOperationDone = 1u << 0;
static const uint32_t kI2cmNack = 1u << 2;
static const uint32_t kDdcAddress = 0x50;
static const uint32_t kDdcSegmentAddress = 0x30;
static const uint32_t kHpdLevel1 = 1u << 24; // SYS_GRF SOC_STATUS1 HDMI1 level
static const unsigned kEdidPollMicros = 20;
static const unsigned kEdidPollLimit = 5000; // 100 ms per byte, as Linux

struct EdidRequest {
	uint32_t version; // in
	uint32_t block; // in: 0..3
	uint32_t result; // out
	uint32_t flags; // out
	int64_t startedMicros;
	int64_t finishedMicros;
	uint32_t bytesRead;
	uint32_t polls;
	uint32_t controlBefore; // I2CM_INTERFACE_CONTROL0 before the transfer
	uint32_t controlAfter;
	uint32_t statusBefore; // MAINUNIT_1_INT_STATUS before the transfer
	uint32_t statusAfter;
	uint32_t hotPlug; // SYS_GRF SOC_STATUS1 at the time of the read
	uint32_t reserved;
	uint8_t data[kEdidBlockBytes];
};


template<class Hardware>
static uint32_t
ModifyHdmi(Hardware& hardware, uint32_t offset, uint32_t mask, uint32_t value)
{
	uint32_t updated = (hardware.ReadHdmi(offset) & ~mask) | (value & mask);
	hardware.WriteHdmi(offset, updated);
	return updated;
}


// Reads one 128-byte EDID block, one byte per transfer as both Linux and the
// firmware do, polling the interrupt status with a bounded deadline. Every
// error path resets the I2C master and clears the transfer request bits so
// the controller is left idle; nothing else is changed.
template<class Hardware>
uint32_t
ReadEdidBlock(Hardware& hardware, uint32_t block, EdidRequest& request)
{
	request.result = kEdidInvalidBlock;
	request.flags = 0;
	request.bytesRead = 0;
	request.polls = 0;
	if (block >= kEdidMaxBlocks)
		return request.result;
	request.startedMicros = hardware.Now();
	request.controlBefore = hardware.ReadHdmi(kI2cmInterfaceControl0);
	request.statusBefore = hardware.ReadHdmi(kMainUnit1InterruptStatus);
	// Unmask done/error status while transferring, as Linux and EDK2 do.
	ModifyHdmi(hardware, kMainUnit1InterruptMask, kI2cmOperationDone | kI2cmNack,
		kI2cmOperationDone | kI2cmNack);
	hardware.WriteHdmi(kMainUnit1InterruptClear, kI2cmOperationDone | kI2cmNack);
	ModifyHdmi(hardware, kI2cmInterfaceControl0, kI2cmSlaveMask, kDdcAddress << 5);
	uint32_t command = kI2cmFastRead;
	if (block >= 2) {
		ModifyHdmi(hardware, kI2cmInterfaceControl1, kI2cmSegmentAddressMask, kDdcSegmentAddress);
		ModifyHdmi(hardware, kI2cmInterfaceControl1, kI2cmSegmentPointerMask, (block / 2) << 7);
		command = kI2cmExtendedRead;
		request.flags |= kEdidSegmentUsed;
	}
	uint32_t start = (block & 1) * kEdidBlockBytes;
	request.result = kEdidOK;
	for (uint32_t index = 0; index < kEdidBlockBytes; index++) {
		ModifyHdmi(hardware, kI2cmInterfaceControl0, kI2cmAddressMask, (start + index) << 12);
		ModifyHdmi(hardware, kI2cmInterfaceControl0, kI2cmWriteMask, command);
		uint32_t status = 0;
		unsigned poll;
		for (poll = 0; poll < kEdidPollLimit; poll++) {
			status = hardware.ReadHdmi(kMainUnit1InterruptStatus)
				& (kI2cmOperationDone | kI2cmNack);
			if (status != 0)
				break;
			hardware.Pause(kEdidPollMicros);
		}
		request.polls += poll;
		if (status == 0 || (status & kI2cmNack) != 0) {
			request.result = status == 0 ? kEdidTimeout : kEdidNack;
			hardware.WriteHdmi(kI2cmControl0, 0x01);
			request.flags |= kEdidMasterReset;
			if (status != 0)
				hardware.WriteHdmi(kMainUnit1InterruptClear, status);
			ModifyHdmi(hardware, kI2cmInterfaceControl0, kI2cmWriteMask, 0);
			break;
		}
		request.data[index] = (uint8_t)(hardware.ReadHdmi(kI2cmReadData) & 0xff);
		request.bytesRead = index + 1;
		ModifyHdmi(hardware, kI2cmInterfaceControl0, kI2cmWriteMask, 0);
		hardware.WriteHdmi(kMainUnit1InterruptClear, status);
	}
	ModifyHdmi(hardware, kMainUnit1InterruptMask, kI2cmOperationDone | kI2cmNack, 0);
	request.controlAfter = hardware.ReadHdmi(kI2cmInterfaceControl0);
	request.statusAfter = hardware.ReadHdmi(kMainUnit1InterruptStatus);
	request.finishedMicros = hardware.Now();
	return request.result;
}

} // namespace RK3588Display

#endif // RK3588_DISPLAY_EDID_H
