/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_PROPERTY_CAPTURE_H
#define MALI_CSF_PROPERTY_CAPTURE_H

#include "CsfInterface.h"
#include "CsfQueue.h"

namespace MaliCSF {

// Only the runtime hardware worker may call this, while the GPU is powered.
// Register offsets: Linux 6.18.52 panthor_regs.h/panthor_hw.c (MIT option).
// All pairs below are immutable feature registers, not changing counters.
template<typename IO>
GpuProperties
CaptureGpuProperties(IO& io, const InterfaceInfo& interface,
	const QueueInterfaceInfo& queue, uint32_t timerHz)
{
	GpuProperties result = {};
	result.gpuId = io.ReadGpu(0x000);
	result.gpuRevision = io.ReadGpu(0x280);
	result.csfId = io.ReadGpu(0x01c);
	result.coreFeatures = io.ReadGpu(0x008);
	result.l2Features = io.ReadGpu(0x004);
	result.tilerFeatures = io.ReadGpu(0x00c);
	result.memFeatures = io.ReadGpu(0x010);
	result.mmuFeatures = io.ReadGpu(0x014);
	result.threadFeatures = io.ReadGpu(0x0ac);
	result.maxThreads = io.ReadGpu(0x0a0);
	result.maxWorkgroupSize = io.ReadGpu(0x0a4);
	result.maxBarrierSize = io.ReadGpu(0x0a8);
	result.coherencyFeatures = io.ReadGpu(0x300);
	for (unsigned i = 0; i < 4; i++) result.textureFeatures[i] = io.ReadGpu(0x0b0 + i * 4);
	result.addressSpaces = io.ReadGpu(0x018);
	auto read64 = [&](uint32_t offset) {
		uint64_t low = io.ReadGpu(offset);
		return low | (uint64_t(io.ReadGpu(offset + 4)) << 32);
	};
	result.shaderPresent = read64(0x100);
	result.tilerPresent = read64(0x110);
	result.l2Present = read64(0x120);
	result.gpuFeatures = read64(0x060);
	result.firmwareVersion = interface.version;
	result.firmwareFeatures = interface.features;
	result.groupFeatures = queue.features;
	result.streamFeatures = interface.streamFeatures;
	result.groupCount = interface.groupCount;
	result.streamCount = interface.streamCount;
	result.workRegisters = interface.workRegisters;
	result.scoreboards = interface.scoreboards;
	result.reservedRegisters = kQueueReservedRegisters;
	result.firmwareTimerHz = timerHz;
	result.maxQueues = kMaxRuntimeQueues;
	result.maxQueueJobs = kMaxQueueJobs;
	result.userVaLimit = kVmUserLimit;
	return result;
}

} // namespace MaliCSF
#endif
