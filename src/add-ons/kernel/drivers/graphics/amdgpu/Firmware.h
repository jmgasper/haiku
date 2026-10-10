/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_FIRMWARE_H
#define AMDGPU_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

namespace amdgpu {

static inline uint16_t
ReadLE16(const uint8_t* p)
{
	return p[0] | (uint16_t)p[1] << 8;
}

static inline uint32_t
ReadLE32(const uint8_t* p)
{
	return ReadLE16(p) | (uint32_t)ReadLE16(p + 2) << 16;
}

struct FirmwareView {
	const uint8_t* code;
	uint32_t codeSize;
	uint32_t version;
	uint32_t featureVersion;
};

struct AtomVramReservation {
	uint64_t start;
	uint64_t size;
	uint32_t driverScratchSize;
};

bool ParseSdmaFirmware(const void* data, size_t size, FirmwareView& view);
bool ParseSmcFirmware(const void* data, size_t size, FirmwareView& view);
bool ParseAtomVramReservation(const void* data, size_t size,
	AtomVramReservation& reservation);

} // namespace amdgpu
#endif
