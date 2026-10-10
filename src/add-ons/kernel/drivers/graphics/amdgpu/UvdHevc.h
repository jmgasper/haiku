/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_UVD_HEVC_H
#define AMDGPU_UVD_HEVC_H
#include <amdgpu_hevc.h>
#include "UvdH264.h"

namespace amdgpu {
struct UvdHevcLayout {
	uint32_t bytes, pitch, outputHeight, outputBytes, dpbBytes, contextBytes;
	uint32_t message, feedback, scaling, session, bitstream, dpb, context, target;
	uint32_t guards[4];
};
const size_t kUvdHevcScalingBytes = 992;
// Maximum remaining CPU aperture after the driver's first-64-MiB reservation.
// The allocator must still enforce actual availability and ROM reservations.
const uint32_t kUvdHevcMaxSessionBytes = 192 * 1024 * 1024;
bool UvdHevcSize(const amdgpu_hevc_config& config, UvdHevcLayout& layout);
bool UvdHevcValidate(const amdgpu_hevc_config& config,
	const amdgpu_hevc_picture& picture, uint32_t bitstreamBytes);
bool UvdHevcBitstream(const uint8_t* data, uint32_t bytes, uint8_t nalType);
bool UvdHevcMessage(uint8_t* message, uint32_t type, uint32_t handle,
	const amdgpu_hevc_config& config, const amdgpu_hevc_picture* picture,
	uint32_t bitstreamBytes, uint32_t frame);
void UvdHevcScaling(uint8_t* scaling, const amdgpu_hevc_picture& picture);
}
#endif
