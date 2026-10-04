/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_AUDIO_REGISTERS_H
#define RK3588_AUDIO_REGISTERS_H

#include <stdint.h>


namespace RK3588Audio {

static const uint64_t kCruBase = UINT64_C(0xfd7c0000);
static const uint64_t kPmuBase = UINT64_C(0xfd8d8000);
static const uint64_t kIocBase = UINT64_C(0xfd5f0000);
static const uint64_t kMclkBase = UINT64_C(0xfd58c000);
static const uint64_t kI2cBase = UINT64_C(0xfec90000);
static const uint64_t kI2sBase = UINT64_C(0xfe470000);

static const uint32_t kCruClockSelect24 = 0x360;
static const uint32_t kCruClockSelect25 = 0x364;
static const uint32_t kCruClockSelect26 = 0x368;
static const uint32_t kCruClockSelect28 = 0x370;
static const uint32_t kCruClockSelect38 = 0x398;
static const uint32_t kCruClockGate7 = 0x81c;
static const uint32_t kCruClockGate10 = 0x828;
static const uint32_t kCruClockGate11 = 0x82c;
static const uint32_t kCruSoftReset7 = 0xa1c;

static const uint32_t kPmuIdleRequest1 = 0x110;
static const uint32_t kPmuIdleAck = 0x118;
static const uint32_t kPmuIdleStatus = 0x120;
static const uint32_t kPmuPowerGate1 = 0x150;
static const uint32_t kPmuRepairStatus = 0x290;
static const uint32_t kAudioPower = 1u << 4;
static const uint32_t kAudioIdle = 1u << 17;
static const uint32_t kAudioRepair = 1u << 19;

static const uint32_t kI2cControl = 0x00;
static const uint32_t kI2cClockDiv = 0x04;
static const uint32_t kI2cReceiveAddress = 0x08;
static const uint32_t kI2cReceiveRegister = 0x0c;
static const uint32_t kI2cTransmitCount = 0x10;
static const uint32_t kI2cReceiveCount = 0x14;
static const uint32_t kI2cInterruptEnable = 0x18;
static const uint32_t kI2cInterruptPending = 0x1c;
static const uint32_t kI2cTransmitBuffer = 0x100;
static const uint32_t kI2cReceiveBuffer = 0x200;

static const uint32_t kI2sTransmitControl = 0x00;
static const uint32_t kI2sReceiveControl = 0x04;
static const uint32_t kI2sClockGeneration = 0x08;
static const uint32_t kI2sTransmitFifoLevel = 0x0c;
static const uint32_t kI2sDmaControl = 0x10;
static const uint32_t kI2sInterruptControl = 0x14;
static const uint32_t kI2sInterruptStatus = 0x18;
static const uint32_t kI2sTransfer = 0x1c;
static const uint32_t kI2sClear = 0x20;
static const uint32_t kI2sTransmitData = 0x24;
static const uint32_t kI2sClockDiv = 0x38;

static const uint32_t kI2sTransferTransmit = 1u << 0;
static const uint32_t kI2sTransferReceive = 1u << 1;
static const uint32_t kI2sClearTransmit = 1u << 0;
static const uint32_t kI2sClearReceive = 1u << 1;
static const uint32_t kI2sBoardTransmitControl = 0x7200000f;
static const uint32_t kI2sBoardReceiveControl = 0x01c8000f;


constexpr uint32_t
Hiword(uint32_t mask, uint32_t value)
{
	return (mask << 16) | (value & mask);
}


constexpr uint32_t
PackStereo(int16_t left, int16_t right)
{
	return (uint16_t)left | ((uint32_t)(uint16_t)right << 16);
}


constexpr uint32_t
SynchronizedTransfer()
{
	return kI2sTransferTransmit | kI2sTransferReceive;
}


constexpr uint64_t
AudioMclk(uint64_t gpll)
{
	return (gpll / 5) * 128 / 2475;
}

}

#endif
