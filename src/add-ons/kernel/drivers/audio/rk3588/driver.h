/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_AUDIO_DRIVER_H
#define RK3588_AUDIO_DRIVER_H

#include <Drivers.h>
#include <KernelExport.h>
#include <device_manager.h>
#include <lock.h>
#include <hmulti_audio.h>


#define RK3588_AUDIO_DRIVER_NAME \
	"drivers/audio/hmulti/rk3588_audio/driver_v1"
#define RK3588_AUDIO_DEVICE_NAME \
	"drivers/audio/hmulti/rk3588_audio/device_v1"
#define RK3588_AUDIO_DEVICE_PATH "audio/hmulti/rk3588/0"

static const uint32 kBufferCount = 2;
static const uint32 kFramesPerBuffer = 1024;
static const uint32 kChannels = 2;
static const uint32 kSampleBytes = 2;


struct AudioStream {
	spinlock lock;
	void* buffers[kBufferCount];
	area_id area;
	sem_id readySem;
	uint32 bufferLength;
	uint32 currentBuffer;
	uint32 frameOffset;
	uint32 completedBuffer;
	uint64 framesCount;
	bigtime_t realTime;
};


struct AudioController {
	device_node* node;
	uint32 interrupt;
	area_id cruArea;
	area_id pmuArea;
	area_id iocArea;
	area_id mclkArea;
	area_id i2cArea;
	area_id i2sArea;
	volatile uint32* cru;
	volatile uint32* pmu;
	volatile uint32* ioc;
	volatile uint32* mclk;
	volatile uint32* i2c;
	volatile uint32* i2s;
	AudioStream stream;
	mutex hardwareLock;
	int32 openCount;
	bool interruptInstalled;
	bool running;
	bool codecReady;
	uint32 underruns;
};


extern device_manager_info* gDeviceManager;

status_t rk3588_audio_start(AudioController* controller);
void rk3588_audio_stop(AudioController* controller);
void rk3588_audio_release_buffers(AudioController* controller);
status_t rk3588_audio_control(AudioController* controller, uint32 operation,
	void* buffer, size_t length);

#endif
