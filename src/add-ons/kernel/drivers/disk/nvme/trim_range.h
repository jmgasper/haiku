/*
 * Distributed under the terms of the MIT License.
 */
#ifndef NVME_TRIM_RANGE_H
#define NVME_TRIM_RANGE_H

#include <stdint.h>


// Include only complete sectors inside both the requested and device ranges.
// A valid range containing no complete sector is a successful no-op.
inline bool
nvme_normalize_trim_range(uint64_t deviceBytes, uint32_t blockSize,
	uint64_t offset, uint64_t size, uint64_t& lba, uint32_t& blocks)
{
	lba = 0;
	blocks = 0;
	if (blockSize == 0 || deviceBytes % blockSize != 0 || offset >= deviceBytes)
		return false;

	if (size > deviceBytes - offset)
		size = deviceBytes - offset;
	uint32_t remainder = offset % blockSize;
	uint32_t skip = remainder == 0 ? 0 : blockSize - remainder;
	if (size <= skip)
		return true;

	// Subtract the partial leading sector before division, without wrapping
	// an undersized request into a large unsigned range.
	uint64_t count = (size - skip) / blockSize;
	if (count == 0)
		return true;
	if (count > UINT32_MAX)
		count = UINT32_MAX;
		// TODO: Split larger requests into additional DSM ranges.
	lba = (offset + skip) / blockSize;
	blocks = count;
	return true;
}

#endif // NVME_TRIM_RANGE_H
