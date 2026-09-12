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
	uint64_t offset, uint64_t size, uint64_t& lba, uint64_t& blocks)
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
	lba = (offset + skip) / blockSize;
	blocks = count;
	return true;
}


// Zero Identify limits mean no restriction beyond the command format. Keep
// the full input interval in 64 bits and split it before writing descriptors.
class NVMeTrimBatch {
public:
	NVMeTrimBatch(uint16_t maxRanges, uint32_t maxRangeBlocks,
		uint64_t maxCommandBlocks)
		:
		fMaxRanges(maxRanges == 0 || maxRanges > 256 ? 256 : maxRanges),
		fMaxRangeBlocks(maxRangeBlocks == 0 ? UINT32_MAX : maxRangeBlocks),
		fMaxCommandBlocks(maxCommandBlocks == 0 ? UINT64_MAX : maxCommandBlocks),
		fCount(0),
		fBlocks(0)
	{
	}

	// Returns zero when the caller must submit and reset the current batch.
	uint32_t Add(uint64_t remaining)
	{
		if (fCount == fMaxRanges || fBlocks == fMaxCommandBlocks
			|| remaining == 0) {
			return 0;
		}
		if (remaining > fMaxRangeBlocks)
			remaining = fMaxRangeBlocks;
		if (remaining > fMaxCommandBlocks - fBlocks)
			remaining = fMaxCommandBlocks - fBlocks;
		fCount++;
		fBlocks += remaining;
		return remaining;
	}

	void Reset() { fCount = 0; fBlocks = 0; }
	uint16_t Count() const { return fCount; }
	uint16_t MaxRanges() const { return fMaxRanges; }
	uint64_t Blocks() const { return fBlocks; }

private:
	uint16_t fMaxRanges;
	uint32_t fMaxRangeBlocks;
	uint64_t fMaxCommandBlocks;
	uint16_t fCount;
	uint64_t fBlocks;
};

#endif // NVME_TRIM_RANGE_H
