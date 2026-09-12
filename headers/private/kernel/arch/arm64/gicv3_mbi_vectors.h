/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARM64_GICV3_MBI_VECTORS_H
#define ARM64_GICV3_MBI_VECTORS_H

#include <stdint.h>

namespace Gicv3Mbi {

// A naturally aligned subset of the RK3588 TRM's reserved SPIs, also inside
// the firmware MBI range. Earlier advertised MBI IDs overlap wired devices.
static const uint32_t kFirstVector = 464;
static const uint32_t kVectorCount = 16;

// The caller serializes access and checks the hardware before reuse.
class VectorPool {
public:
	int Find(uint32_t count) const
	{
		if (count == 0 || count > kVectorCount)
			return -1;
		// MSI needs a power-of-two aligned message-data base. MSI-X may ask
		// for other counts; rounding only the alignment supports both APIs.
		uint32_t alignment = 1;
		while (alignment < count)
			alignment *= 2;
		for (uint32_t offset = 0; offset + count <= kVectorCount; offset += alignment) {
			if ((fUsed & Mask(offset, count)) == 0)
				return offset;
		}
		return -1;
	}

	bool Claim(uint32_t offset, uint32_t count)
	{
		if (count == 0 || count > kVectorCount || offset > kVectorCount - count)
			return false;
		uint32_t alignment = 1;
		while (alignment < count)
			alignment *= 2;
		if (offset % alignment != 0 || (fUsed & Mask(offset, count)) != 0)
			return false;
		fUsed |= Mask(offset, count);
		fLengths[offset] = count;
		return true;
	}

	bool Contains(uint32_t offset, uint32_t count) const
	{
		return offset < kVectorCount && count != 0 && count <= kVectorCount
			&& fLengths[offset] == count;
	}

	bool Release(uint32_t offset, uint32_t count)
	{
		if (!Contains(offset, count))
			return false;
		fUsed &= ~Mask(offset, count);
		fLengths[offset] = 0;
		return true;
	}

private:
	static uint32_t Mask(uint32_t offset, uint32_t count)
	{
		return ((1u << count) - 1) << offset;
	}

	uint32_t fUsed = 0;
	uint8_t fLengths[kVectorCount]{};
};

} // namespace Gicv3Mbi
#endif
