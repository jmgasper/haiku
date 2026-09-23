/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "../../src/add-ons/kernel/drivers/audio/rk3588/audio_registers.h"

#include <assert.h>


using namespace RK3588Audio;


int
main()
{
	assert(Hiword(0x00f0, 0x0050) == 0x00f00050);
	assert(Hiword(1u << 14, 0) == 0x40000000);
	assert(AudioMclk(UINT64_C(1188000000)) == UINT64_C(12288000));
	assert(PackStereo((int16_t)0x1234, (int16_t)0x5678) == 0x56781234);
	assert(kCruClockSelect24 == 0x300 + 24 * 4);
	assert(kCruClockGate7 == 0x800 + 7 * 4);
	assert(kCruSoftReset7 == 0xa00 + 7 * 4);
	assert(kI2sBase == UINT64_C(0xfe470000));
	assert(kI2cBase == UINT64_C(0xfec90000));
	assert(kI2sBoardTransmitControl == 0x7200000f);
	assert(kI2sBoardReceiveControl == 0x01c8000f);
	assert(SynchronizedTransfer() == 3);
	return 0;
}
