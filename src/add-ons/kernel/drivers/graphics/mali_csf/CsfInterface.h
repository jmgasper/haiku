/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_INTERFACE_H
#define MALI_CSF_INTERFACE_H

#include <stdint.h>
#include <stddef.h>

namespace MaliCSF {

struct InterfaceInfo {
	uint32_t version;
	uint32_t features;
	uint32_t instrumentation;
	uint32_t groupCount;
	uint32_t groupStride;
	uint32_t streamCount;
	uint32_t streamStride;
	uint32_t streamFeatures;
	uint32_t workRegisters;
	uint32_t scoreboards;
	uint32_t inputOffset;
	uint32_t outputOffset;
};

// A snapshot of the CSF 1.5 interface layout used by the admitted firmware.
// All offsets/sizes are ABI facts from the MIT option of Linux 6.18.52's
// panthor_fw.h. Do not dereference a firmware-supplied pointer on the CPU.
// This checks every control/input/output interval, including mutual overlap,
// before the caller may write the global input. No GPU job is configured.
inline bool
InspectInterface(const void* data, size_t bytes, uint32_t base, InterfaceInfo& info)
{
	info = {};
	if (data == NULL || (uintptr_t(data) & 7) != 0 || bytes < 32
		|| uint64_t(base) + bytes > (UINT64_C(1) << 32))
		return false;
	const volatile uint32_t* words = (const volatile uint32_t*)data;
	struct Interval { uint32_t begin, end; } intervals[219];
	unsigned count = 0;
	auto claim = [&](uint64_t offset, uint32_t size, unsigned alignment) {
		if (offset > bytes || size > bytes - offset || (offset & (alignment - 1)) != 0
			|| count >= sizeof(intervals) / sizeof(intervals[0]))
			return false;
		for (unsigned i = 0; i < count; i++) {
			if (offset < intervals[i].end && offset + size > intervals[i].begin)
				return false;
		}
		intervals[count++] = {uint32_t(offset), uint32_t(offset + size)};
		return true;
	};
	auto address = [&](uint32_t value, uint32_t size) {
		return value >= base && claim(uint64_t(value) - base, size, 8);
	};
	if (!claim(0, 32, 4))
		return false;
	InterfaceInfo result = {};
	result.version = words[0];
	result.features = words[1];
	result.groupCount = words[4];
	result.groupStride = words[5];
	result.instrumentation = words[7];
	uint32_t input = words[2], output = words[3];
	if (result.version != 0x01050000 || result.groupCount != 8
		|| result.groupStride < 0x40 + 8 * 12 || (result.groupStride & 3) != 0
		|| !address(input, 136) || !address(output, 28))
		return false;
	result.inputOffset = input - base;
	result.outputOffset = output - base;
	uint32_t groupFeatures = 0, suspendBytes = 0, protectedBytes = 0;
	for (unsigned group = 0; group < 8; group++) {
		uint64_t offset = 0x1000 + uint64_t(group) * result.groupStride;
		if (!claim(offset, 28, 4))
			return false;
		const volatile uint32_t* control = words + offset / 4;
		uint32_t features = control[0], suspend = control[3], protectedSize = control[4];
		uint32_t streams = control[5], stride = control[6];
		if (streams != 8 || stride < 12 || (stride & 3) != 0
			|| uint64_t(0x40) + uint64_t(streams - 1) * stride + 12 > result.groupStride
			|| !address(control[1], 88) || !address(control[2], 32))
			return false;
		if (group == 0) {
			groupFeatures = features;
			suspendBytes = suspend;
			protectedBytes = protectedSize;
			result.streamCount = streams;
			result.streamStride = stride;
		} else if (features != groupFeatures || suspend != suspendBytes
			|| protectedSize != protectedBytes || stride != result.streamStride)
			return false;
		for (unsigned stream = 0; stream < 8; stream++) {
			uint64_t streamOffset = offset + 0x40 + uint64_t(stream) * stride;
			if (!claim(streamOffset, 12, 4))
				return false;
			const volatile uint32_t* slot = words + streamOffset / 4;
			uint32_t streamFeatures = slot[0];
			if (!address(slot[1], 88) || !address(slot[2], 216))
				return false;
			if (group == 0 && stream == 0) {
				result.streamFeatures = streamFeatures;
				result.workRegisters = (streamFeatures & 255) + 1;
				result.scoreboards = (streamFeatures >> 8) & 255;
				if (result.workRegisters != 96 || result.scoreboards != 8)
					return false;
			} else if (streamFeatures != result.streamFeatures)
				return false;
		}
	}
	info = result;
	return true;
}

} // namespace MaliCSF
#endif
