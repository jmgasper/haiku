/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_MEMORY_H
#define MALI_CSF_MEMORY_H

#include "CsfFirmware.h"
#include <string.h>

namespace MaliCSF {

// Driver-owned, non-executable MCU workspace, separate from file sections.
struct FirmwareWorkspace {
	uint32_t address;
	uint32_t bytes;
	bool cached;
};
static const unsigned kMaxFirmwareWorkspaces = 3;

// RK3588's MCU allocates in the low 4 GiB of a 48-bit input address space.
// Its MMU still starts at level 0. Physical outputs are limited to 40 bits.
// This builder owns no allocation and performs no cache or hardware operation.
// The immutable image and caller's page-aligned private RAM must outlive it.
class FirmwareMemory {
public:
	FirmwareMemory() { Clear(); }
	bool Plan(const FirmwareImage& image, const FirmwareWorkspace* workspace = NULL,
		unsigned workspaceCount = 0)
	{
		Clear();
		if (image.Info().sectionCount == 0
			|| image.Info().sectionCount > kMaxFirmwareSections
			|| workspaceCount > kMaxFirmwareWorkspaces
			|| (workspaceCount != 0 && workspace == NULL))
			return false;
		fRegionCount = image.Info().sectionCount + workspaceCount;
		for (uint32_t i = 0; i < fRegionCount; i++) {
			MappedRegion section = {};
			if (i < image.Info().sectionCount) {
				FirmwareSection file;
				if (image.GetSection(i, file) != FIRMWARE_OK) {
					Clear();
					return false;
				}
				section = {file.virtualAddress, file.memorySize, file.flags};
			} else {
				const FirmwareWorkspace& region = workspace[i - image.Info().sectionCount];
				if (region.address < 0x04000000
					|| uint64_t(region.address) + region.bytes > 0x08000000) {
					Clear();
					return false;
				}
				section = {region.address, region.bytes,
					SECTION_READ | SECTION_WRITE | (region.cached ? 8u : 24u)};
			}
			if (section.memorySize == 0 || (section.memorySize & 4095) != 0
				|| (section.virtualAddress & 4095) != 0
				|| uint64_t(section.virtualAddress) + section.memorySize > (UINT64_C(1) << 32)
				|| (section.flags & SECTION_READ) == 0
				|| (section.flags & SECTION_PROTECTED) != 0
				|| (section.flags & (SECTION_WRITE | SECTION_EXECUTE))
					== (SECTION_WRITE | SECTION_EXECUTE)) {
				Clear();
				return false;
			}
			for (uint32_t j = 0; j < i; j++) {
				if (uint64_t(section.virtualAddress) < uint64_t(fSections[j].virtualAddress)
						+ fSections[j].memorySize
					&& uint64_t(fSections[j].virtualAddress) < uint64_t(section.virtualAddress)
						+ section.memorySize) {
					Clear();
					return false;
				}
			}
			fSections[i] = section;
			fOffsets[i] = fPayloadBytes;
			fPayloadBytes += section.memorySize;
			if (fPayloadBytes > kMaxFirmwareMappedBytes) {
				Clear();
				return false;
			}
			if ((section.flags & SECTION_SHARED) != 0)
				fShared = i;
			uint64_t end = uint64_t(section.virtualAddress) + section.memorySize;
			for (uint64_t chunk = section.virtualAddress >> 21; chunk <= (end - 1) >> 21; chunk++)
				fChunks[chunk / 8] |= 1u << (chunk % 8);
		}
		// Enumerating occupied 2 MiB windows needs only 256 bytes of scratch
		// and does not depend on section ordering or their file-data aliases.
		fTablePages = 2; // level 0 and level 1
		for (unsigned gigabyte = 0; gigabyte < 4; gigabyte++) {
			bool occupied = false;
			for (unsigned slot = 0; slot < 512; slot++) {
				if (HasChunk(gigabyte * 512 + slot)) {
					fTablePages++;
					occupied = true;
				}
			}
			if (occupied)
				fTablePages++; // level 2 for this 1 GiB window
		}
		if (fShared == kMaxFirmwareSections) {
			Clear();
			return false;
		}
		fImage = &image;
		return true;
	}

	size_t RequiredBytes() const { return fImage == NULL ? 0 : fTablePages * 4096 + fPayloadBytes; }
	uint32_t TablePages() const { return fTablePages; }
	uint64_t RootPhysical() const { return fPhysical; }
	uint32_t SharedAddress() const { return fImage == NULL ? 0 : fSections[fShared].virtualAddress; }
	size_t SharedBytes() const { return fImage == NULL ? 0 : fSections[fShared].memorySize; }
	void* SharedData() const { return fData == NULL ? NULL : fData + fTablePages * 4096 + fOffsets[fShared]; }
	void* WorkspaceData(unsigned index) const
	{
		return fData == NULL || index >= fRegionCount - fImage->Info().sectionCount
			? NULL : fData + fTablePages * 4096 + fOffsets[fImage->Info().sectionCount + index];
	}
	void InvalidateMapping() { fData = NULL; fPhysical = 0; }

	bool Build(void* data, size_t bytes, uint64_t physical)
	{
		InvalidateMapping();
		const size_t required = RequiredBytes();
		if (required == 0 || data == NULL || (uintptr_t(data) & 4095) != 0
			|| bytes < required || (physical & 4095) != 0
			|| physical >= (UINT64_C(1) << 40)
			|| required > (UINT64_C(1) << 40) - physical)
			return false;
		uint8_t* arena = (uint8_t*)data;
		memset(arena, 0, required);
		uint32_t next = 2;
		uint64_t* root = (uint64_t*)arena;
		uint64_t* level1 = (uint64_t*)(arena + 4096);
		root[0] = (physical + 4096) | 3;
		for (unsigned gigabyte = 0; gigabyte < 4; gigabyte++) {
			uint64_t* level2 = NULL;
			for (unsigned slot = 0; slot < 512; slot++) {
				unsigned chunk = gigabyte * 512 + slot;
				if (!HasChunk(chunk))
					continue;
				if (level2 == NULL) {
					level1[gigabyte] = (physical + next * 4096) | 3;
					level2 = (uint64_t*)(arena + next++ * 4096);
				}
				level2[slot] = (physical + next * 4096) | 3;
				uint64_t* leaves = (uint64_t*)(arena + next++ * 4096);
				for (unsigned page = 0; page < 512; page++) {
					uint64_t address = (uint64_t(chunk) << 21) + page * 4096;
					for (uint32_t i = 0; i < fRegionCount; i++) {
						const MappedRegion& section = fSections[i];
						if (address < section.virtualAddress
							|| address - section.virtualAddress >= section.memorySize)
							continue;
						uint64_t output = physical + fTablePages * 4096 + fOffsets[i]
							+ address - section.virtualAddress;
						// ARM64 stage-1 page: valid, page, AF, unprivileged access.
						uint64_t attributes = 3 | (1u << 10) | (1u << 6);
						if ((section.flags & SECTION_WRITE) == 0)
							attributes |= 1u << 7;
						if ((section.flags & SECTION_EXECUTE) == 0)
							attributes |= UINT64_C(3) << 53; // PXN and UXN
						bool cached = (section.flags & SECTION_CACHE_MASK) == (1u << 3);
						attributes |= cached ? (1u << 2) | (3u << 8) : (2u << 8);
						leaves[page] = output | attributes;
						break;
					}
				}
			}
		}
		if (next != fTablePages)
			return false;
		for (uint32_t i = 0; i < fImage->Info().sectionCount; i++) {
			if (fImage->CopySection(i, arena + fTablePages * 4096 + fOffsets[i],
					fSections[i].memorySize) != FIRMWARE_OK)
				return false;
		}
		fPhysical = physical;
		fData = arena;
		return true;
	}

	// These are Mali AS_MEMATTR bytes, not the CPU's MAIR encoding.
	static uint64_t MemoryAttributes() { return UINT64_C(0xc0c0c0c0c0c08f4c); }
	static uint64_t TranslationConfig() { return UINT64_C(0x420001c6); }

private:
	struct MappedRegion { uint32_t virtualAddress, memorySize, flags; };
	void Clear()
	{
		fImage = NULL;
		fData = NULL;
		fPhysical = 0;
		fPayloadBytes = 0;
		fTablePages = 0;
		fRegionCount = 0;
		fShared = kMaxFirmwareSections;
		memset(fChunks, 0, sizeof(fChunks));
	}
	bool HasChunk(unsigned chunk) const { return (fChunks[chunk / 8] & (1u << (chunk % 8))) != 0; }
	const FirmwareImage* fImage;
	uint8_t* fData;
	uint64_t fPhysical;
	size_t fPayloadBytes;
	uint32_t fTablePages;
	uint32_t fShared;
	uint32_t fRegionCount;
	uint8_t fChunks[256];
	MappedRegion fSections[kMaxFirmwareSections + kMaxFirmwareWorkspaces];
	size_t fOffsets[kMaxFirmwareSections + kMaxFirmwareWorkspaces];
};

} // namespace MaliCSF
#endif
