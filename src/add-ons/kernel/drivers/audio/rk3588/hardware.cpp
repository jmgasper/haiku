/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "driver.h"

#include "audio_registers.h"

#include <util/AutoLock.h>
#include <string.h>


using namespace RK3588Audio;

namespace {

static const uint32 kI2cEnable = 1u << 0;
static const uint32 kI2cRegisterTransmit = 1u << 1;
static const uint32 kI2cStart = 1u << 3;
static const uint32 kI2cStop = 1u << 4;
static const uint32 kI2cLastAck = 1u << 5;
static const uint32 kI2cActAck = 1u << 6;
static const uint32 kI2cInterruptTransmitFinished = 1u << 2;
static const uint32 kI2cInterruptReceiveFinished = 1u << 3;
static const uint32 kI2cInterruptStart = 1u << 4;
static const uint32 kI2cInterruptStop = 1u << 5;
static const uint32 kI2cInterruptNak = 1u << 6;
static const uint32 kI2cInterruptAll = 0x7f;
static const uint32 kI2cValidByte0 = 1u << 24;
static const uint32 kI2cTuning = 0x5200;
static const uint32 kI2cDivider = 0x003e003e;
static const uint8 kCodecAddress = 0x11;


inline uint32
Read(const volatile uint32* registers, uint32 offset)
{
	memory_read_barrier();
	return registers[offset / 4];
}


inline void
Write(volatile uint32* registers, uint32 offset, uint32 value)
{
	registers[offset / 4] = value;
	memory_write_barrier();
}


bool
WaitBits(const volatile uint32* registers, uint32 offset, uint32 mask,
	uint32 value, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	do {
		if ((Read(registers, offset) & mask) == value)
			return true;
		snooze(5);
	} while (system_time() < deadline);
	return false;
}


status_t
WaitI2cInterrupt(AudioController* controller, uint32 wanted)
{
	bigtime_t deadline = system_time() + 10000;
	do {
		uint32 pending = Read(controller->i2c, kI2cInterruptPending);
		if ((pending & kI2cInterruptNak) != 0)
			return B_DEV_NOT_READY;
		if ((pending & wanted) != 0)
			return B_OK;
		snooze(5);
	} while (system_time() < deadline);
	return B_TIMED_OUT;
}


void
ResetI2c(AudioController* controller)
{
	Write(controller->i2c, kI2cInterruptEnable, 0);
	Write(controller->i2c, kI2cInterruptPending, kI2cInterruptAll);
	Write(controller->i2c, kI2cControl, kI2cTuning);
}


status_t
EnsureAudioPower(AudioController* controller)
{
	if ((Read(controller->pmu, kPmuRepairStatus) & kAudioRepair) == 0) {
		Write(controller->pmu, kPmuPowerGate1, kAudioPower << 16);
		if (!WaitBits(controller->pmu, kPmuRepairStatus, kAudioRepair,
				kAudioRepair, 10000)) {
			return B_TIMED_OUT;
		}
	}
	Write(controller->pmu, kPmuIdleRequest1, 1u << (1 + 16));
	if (!WaitBits(controller->pmu, kPmuIdleAck, kAudioIdle, 0, 10000)
		|| !WaitBits(controller->pmu, kPmuIdleStatus, kAudioIdle, 0, 10000)) {
		return B_TIMED_OUT;
	}
	return B_OK;
}


void
ConfigurePins(AudioController* controller)
{
	// GPIO1_C2/C3 are MCLK/SCLK, C5/C7 are LRCK/SDO0 and D4 is SDI0.
	Write(controller->ioc, 0x8030, Hiword(0xff00, 0x1100));
	Write(controller->ioc, 0x8034, Hiword(0xf0f0, 0x1010));
	Write(controller->ioc, 0x803c, Hiword(0x000f, 0x0002));
	// GPIO1_D0/D1 carry I2C7 in mux function 9.
	Write(controller->ioc, 0x8038, Hiword(0x00ff, 0x0099));
	// The RK3588 clk-out gate uses zero to enable I2S0 MCLK on the pad.
	Write(controller->mclk, 0x318, Hiword(1u, 0));
}


void
ConfigureClocks(AudioController* controller)
{
	// HCLK/PCLK audio roots, then GPLL / 5 as the fractional input.
	Write(controller->cru, kCruClockSelect24,
		Hiword(0x03ff, 4u << 4));
	// 1.188 GHz / 5 * 128 / 2475 = 12.288 MHz exactly.
	Write(controller->cru, kCruClockSelect25, (128u << 16) | 2475u);
	Write(controller->cru, kCruClockSelect26, Hiword(0x0003, 0x0001));
	Write(controller->cru, kCruClockSelect28, Hiword(0x000c, 0));
	uint32 audioGates = (1u << 0) | (1u << 1) | (1u << 4)
		| (1u << 5) | (1u << 6) | (1u << 7);
	Write(controller->cru, kCruClockGate7, Hiword(audioGates, 0));

	// I2C7 gets a 100 MHz functional clock and an ungated APB clock.
	Write(controller->cru, kCruClockSelect38,
		Hiword(1u << 12, 1u << 12));
	Write(controller->cru, kCruClockGate10, Hiword(1u << 14, 0));
	Write(controller->cru, kCruClockGate11, Hiword(1u << 6, 0));
}


status_t
I2cStart(AudioController* controller, uint32 mode)
{
	Write(controller->i2c, kI2cInterruptPending, kI2cInterruptAll);
	Write(controller->i2c, kI2cClockDiv, kI2cDivider);
	Write(controller->i2c, kI2cInterruptEnable, kI2cInterruptStart);
	Write(controller->i2c, kI2cControl,
		kI2cTuning | kI2cEnable | mode | kI2cStart | kI2cActAck);
	status_t status = WaitI2cInterrupt(controller, kI2cInterruptStart);
	if (status != B_OK) {
		ResetI2c(controller);
		return status;
	}
	Write(controller->i2c, kI2cInterruptPending, kI2cInterruptStart);
	uint32 control = Read(controller->i2c, kI2cControl) & ~kI2cStart;
	Write(controller->i2c, kI2cControl, control);
	return B_OK;
}


status_t
I2cStop(AudioController* controller)
{
	Write(controller->i2c, kI2cInterruptEnable, kI2cInterruptStop);
	uint32 control = Read(controller->i2c, kI2cControl) | kI2cStop;
	Write(controller->i2c, kI2cControl, control);
	if (!WaitBits(controller->i2c, kI2cInterruptPending,
			kI2cInterruptStop, kI2cInterruptStop, 10000)) {
		ResetI2c(controller);
		return B_TIMED_OUT;
	}
	Write(controller->i2c, kI2cInterruptPending, kI2cInterruptStop);
	Write(controller->i2c, kI2cInterruptEnable, 0);
	Write(controller->i2c, kI2cControl, kI2cTuning);
	return B_OK;
}


status_t
CodecWrite(AudioController* controller, uint8 reg, uint8 value)
{
	status_t status = I2cStart(controller, 0);
	if (status != B_OK)
		return status;
	Write(controller->i2c, kI2cTransmitBuffer,
		((uint32)value << 16) | ((uint32)reg << 8) | (kCodecAddress << 1));
	Write(controller->i2c, kI2cInterruptEnable,
		kI2cInterruptTransmitFinished | kI2cInterruptNak);
	Write(controller->i2c, kI2cTransmitCount, 3);
	status = WaitI2cInterrupt(controller, kI2cInterruptTransmitFinished);
	if (status != B_OK) {
		ResetI2c(controller);
		return status;
	}
	Write(controller->i2c, kI2cInterruptPending,
		kI2cInterruptTransmitFinished);
	return I2cStop(controller);
}


status_t
CodecRead(AudioController* controller, uint8 reg, uint8& value)
{
	Write(controller->i2c, kI2cReceiveAddress,
		kI2cValidByte0 | (kCodecAddress << 1));
	Write(controller->i2c, kI2cReceiveRegister, kI2cValidByte0 | reg);
	status_t status = I2cStart(controller, kI2cRegisterTransmit);
	if (status != B_OK)
		return status;
	Write(controller->i2c, kI2cControl,
		Read(controller->i2c, kI2cControl) | kI2cLastAck);
	Write(controller->i2c, kI2cInterruptEnable,
		kI2cInterruptReceiveFinished | kI2cInterruptNak);
	Write(controller->i2c, kI2cReceiveCount, 1);
	status = WaitI2cInterrupt(controller, kI2cInterruptReceiveFinished);
	if (status != B_OK) {
		ResetI2c(controller);
		return status;
	}
	value = Read(controller->i2c, kI2cReceiveBuffer) & 0xff;
	Write(controller->i2c, kI2cInterruptPending,
		kI2cInterruptReceiveFinished | kI2cInterruptStart);
	return I2cStop(controller);
}


status_t
ConfigureCodec(AudioController* controller)
{
	status_t status = CodecWrite(controller, 0x00, 0x3f);
	if (status != B_OK)
		return status;
	snooze(5000);
	status = CodecWrite(controller, 0x00, 0x80);
	if (status != B_OK)
		return status;
	snooze(30000);

	struct RegisterValue { uint8 reg; uint8 value; };
	static const RegisterValue registers[] = {
		{0x0c, 0xff}, // VMID and state machine reference
		{0x03, 0x32}, // vendor ADC oversampling value
		{0x01, 0x77}, // board playback clock state, MCLK and BCLK inputs on
		{0x04, 0x11}, {0x05, 0x00},
		{0x06, 0x11}, {0x07, 0x00}, // MCLK/LRCK = 256
		{0x09, 0x01}, // codec slave, MCLK/BCLK = 4 (encoded value 1)
		{0x0a, 0x4c}, // ADC off, 16-bit I2S
		{0x0b, 0x0c}, // 16-bit I2S DAC
		{0x0d, 0x26}, // playback bias and analog references on
		{0x14, 0x88}, // route left/right DACs to headphone mixers
		{0x15, 0x00}, {0x16, 0x00},
		{0x17, 0x66}, // charge pumps and headphone drivers
		{0x18, 0x00}, {0x19, 0x02}, {0x1a, 0x02},
		{0x2f, 0x00}, // both DACs on
		{0x30, 0x30}, // Linux board state plus mute until the FIFO is running
		{0x31, 0x00}, {0x32, 0x00},
		{0x33, 0x00}, {0x34, 0x00}
	};
	for (size_t index = 0; index < sizeof(registers) / sizeof(registers[0]);
		index++) {
		status = CodecWrite(controller, registers[index].reg,
			registers[index].value);
		if (status != B_OK)
			return status;
	}
	uint8 reset = 0;
	status = CodecRead(controller, 0x00, reset);
	if (status != B_OK || reset != 0x80)
		return status == B_OK ? B_BAD_DATA : status;
	controller->codecReady = true;
	return B_OK;
}


void
ConfigureI2s(AudioController* controller)
{
	// Reset the TX master clock domain after its clocks are running.
	Write(controller->cru, kCruSoftReset7, Hiword(1u << 7, 1u << 7));
	snooze(10);
	Write(controller->cru, kCruSoftReset7, Hiword(1u << 7, 0));
	snooze(10);

	Write(controller->i2s, kI2sTransfer, 0);
	Write(controller->i2s, kI2sInterruptControl, 0);
	Write(controller->i2s, kI2sDmaControl, 16);
	// Two channels, 16 valid bits, normal I2S alignment.  Preserve the RK3588
	// path selectors used by I2S0; zeroing these routes the samples away from
	// SDO0 even though the clock pins continue to toggle.
	Write(controller->i2s, kI2sTransmitControl, kI2sBoardTransmitControl);
	Write(controller->i2s, kI2sReceiveControl, kI2sBoardReceiveControl);
	// TX is the shared clock source; 64 BCLKs per frame.
	Write(controller->i2s, kI2sClockGeneration,
		(1u << 28) | (63u << 8) | 63u);
	// 12.288 MHz / 4 = 3.072 MHz BCLK.
	Write(controller->i2s, kI2sClockDiv, (3u << 8) | 3u);
	Write(controller->i2s, kI2sClear,
		kI2sClearTransmit | kI2sClearReceive);
	WaitBits(controller->i2s, kI2sClear,
		kI2sClearTransmit | kI2sClearReceive, 0, 1000);
}


bool
AdvanceFrame(AudioController* controller)
{
	AudioStream& stream = controller->stream;
	const uint32* samples = (const uint32*)stream.buffers[stream.currentBuffer];
	Write(controller->i2s, kI2sTransmitData, samples[stream.frameOffset]);
	stream.frameOffset++;
	if (stream.frameOffset < stream.bufferLength)
		return false;
	stream.completedBuffer = stream.currentBuffer;
	stream.currentBuffer = (stream.currentBuffer + 1) % kBufferCount;
	stream.frameOffset = 0;
	stream.framesCount += stream.bufferLength;
	stream.realTime = system_time();
	return true;
}


int32
AudioInterrupt(void* cookie)
{
	AudioController* controller = (AudioController*)cookie;
	uint32 status = Read(controller->i2s, kI2sInterruptStatus);
	if (!controller->running || (status & 3) == 0)
		return B_UNHANDLED_INTERRUPT;
	bool completed = false;
	acquire_spinlock(&controller->stream.lock);
	uint32 level = Read(controller->i2s, kI2sTransmitFifoLevel) & 0x3f;
	for (uint32 count = level; count < 32; count++)
		completed |= AdvanceFrame(controller);
	if ((status & 2) != 0) {
		controller->underruns++;
		uint32 control = Read(controller->i2s, kI2sInterruptControl);
		Write(controller->i2s, kI2sInterruptControl, control | (1u << 2));
	}
	release_spinlock(&controller->stream.lock);
	if (completed) {
		release_sem_etc(controller->stream.readySem, 1, B_DO_NOT_RESCHEDULE);
		return B_INVOKE_SCHEDULER;
	}
	return B_HANDLED_INTERRUPT;
}

}


status_t
rk3588_audio_start(AudioController* controller)
{
	MutexLocker locker(controller->hardwareLock);
	if (controller->running)
		return B_OK;
	if (controller->stream.area < B_OK || controller->stream.readySem < B_OK)
		return B_NO_INIT;
	status_t status = EnsureAudioPower(controller);
	if (status != B_OK)
		return status;
	ConfigurePins(controller);
	ConfigureClocks(controller);
	controller->codecReady = false;
	status = ConfigureCodec(controller);
	if (status != B_OK) {
		dprintf("rk3588_audio: ES8316 setup failed: %s\n", strerror(status));
		return status;
	}
	ConfigureI2s(controller);
	status = install_io_interrupt_handler(controller->interrupt, AudioInterrupt,
		controller, 0);
	if (status != B_OK)
		return status;
	controller->interruptInstalled = true;
	controller->stream.currentBuffer = 0;
	controller->stream.frameOffset = 0;
	controller->stream.completedBuffer = kBufferCount - 1;
	controller->stream.framesCount = 0;
	controller->stream.realTime = system_time();
	controller->underruns = 0;
	controller->running = true;
	cpu_status interrupts = disable_interrupts();
	acquire_spinlock(&controller->stream.lock);
	for (uint32 count = 0; count < 32; count++)
		AdvanceFrame(controller);
	release_spinlock(&controller->stream.lock);
	restore_interrupts(interrupts);
	// FIFO threshold 8, TX empty and TX underrun interrupts.
	Write(controller->i2s, kI2sInterruptControl, (7u << 4) | 3u);
	// clk-trcm=TX requires the receive and transmit clock domains to start in
	// lockstep even for playback-only streams.
	Write(controller->i2s, kI2sTransfer, SynchronizedTransfer());
	status = CodecWrite(controller, 0x30, 0x10);
	if (status != B_OK) {
		controller->running = false;
		Write(controller->i2s, kI2sInterruptControl, 0);
		Write(controller->i2s, kI2sTransfer, 0);
		remove_io_interrupt_handler(controller->interrupt, AudioInterrupt,
			controller);
		controller->interruptInstalled = false;
		return status;
	}
	dprintf("rk3588_audio: playback started at 48000 Hz, stereo S16\n");
	return B_OK;
}


void
rk3588_audio_stop(AudioController* controller)
{
	MutexLocker locker(controller->hardwareLock);
	if (!controller->running && !controller->interruptInstalled)
		return;
	controller->running = false;
	Write(controller->i2s, kI2sInterruptControl, 0);
	Write(controller->i2s, kI2sTransfer, 0);
	snooze(150);
	Write(controller->i2s, kI2sClear,
		kI2sClearTransmit | kI2sClearReceive);
	WaitBits(controller->i2s, kI2sClear,
		kI2sClearTransmit | kI2sClearReceive, 0, 1000);
	if (controller->interruptInstalled) {
		remove_io_interrupt_handler(controller->interrupt, AudioInterrupt,
			controller);
		controller->interruptInstalled = false;
	}
	if (controller->codecReady)
		CodecWrite(controller, 0x30, 0x30);
	if (controller->stream.readySem >= B_OK)
		release_sem(controller->stream.readySem);
	dprintf("rk3588_audio: playback stopped, underruns=%" B_PRIu32 "\n",
		controller->underruns);
}
