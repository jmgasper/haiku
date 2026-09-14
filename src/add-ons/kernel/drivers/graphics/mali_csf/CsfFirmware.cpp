/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Container layout follows the MIT-licensed Linux Panthor firmware loader:
 * drivers/gpu/drm/panthor/panthor_fw.c, Linux v6.12. See GPU.md for provenance.
 */

#include "CsfFirmware.h"

#include <string.h>


namespace MaliCSF {

static const uint32_t kHeaderSize = 20;
static const uint32_t kMagic = 0xc3f13a6e;
static const uint32_t kEntryOptional = 1U << 31;
static const uint32_t kSectionFlags = 0xc000003f;


static uint32_t
read_le32(const uint8_t* data)
{
	// File data need not be aligned, even on a little-endian host.
	return uint32_t(data[0]) | (uint32_t(data[1]) << 8)
		| (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}


static FirmwareSection
read_section(const uint8_t* entry)
{
	FirmwareSection section;
	section.flags = read_le32(entry + 4);
	section.virtualAddress = read_le32(entry + 8);
	section.memorySize = read_le32(entry + 12) - section.virtualAddress;
	section.dataOffset = read_le32(entry + 16);
	section.dataSize = read_le32(entry + 20) - section.dataOffset;
	return section;
}


FirmwareImage::FirmwareImage()
	:
	fData(NULL),
	fInfo()
{
}


FirmwareStatus
FirmwareImage::Init(const void* data, size_t size)
{
	// Reinitialization must not leave an earlier valid image available on error.
	fData = NULL;
	memset(&fInfo, 0, sizeof(fInfo));
	if (data == NULL)
		return FIRMWARE_BAD_ARGUMENT;
	if (size < kHeaderSize)
		return FIRMWARE_BAD_DATA;
	if (size > kMaxFirmwareBytes)
		return FIRMWARE_LIMIT_EXCEEDED;

	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	if (read_le32(bytes) != kMagic || bytes[6] != 0 || bytes[7] != 0
		|| read_le32(bytes + 12) != 0) {
		return FIRMWARE_BAD_DATA;
	}
	if (bytes[5] != 0)
		return FIRMWARE_UNSUPPORTED;

	// The header size is the end of the entry table, NOT the full file size.
	// Section data follows the table and may be referenced by several entries.
	uint32_t tableSize = read_le32(bytes + 16);
	if (tableSize < kHeaderSize || tableSize > size || (tableSize & 3) != 0)
		return FIRMWARE_BAD_DATA;

	FirmwareInfo info = {};
	info.major = bytes[5];
	info.minor = bytes[4];
	info.versionHash = read_le32(bytes + 8);
	info.tableSize = tableSize;
	bool sharedFound = false;
	for (uint32_t offset = kHeaderSize; offset < tableSize;) {
		uint32_t header = read_le32(bytes + offset);
		uint32_t entrySize = (header >> 8) & 0xff;
		uint32_t type = header & 0xff;
		if (entrySize < 4 || (entrySize & 3) != 0
			|| entrySize > tableSize - offset) {
			return FIRMWARE_BAD_DATA;
		}

		if (type != 0) {
			// Like Panthor, config/test/trace/timeline records do not create
			// mappings. Future types require their optional bit to be set.
			if (type > 4 && (header & kEntryOptional) == 0)
				return FIRMWARE_UNSUPPORTED;
			info.ignoredEntryCount++;
			offset += entrySize;
			continue;
		}
		if (entrySize < 24)
			return FIRMWARE_BAD_DATA;

		uint32_t vaStart = read_le32(bytes + offset + 8);
		uint32_t vaEnd = read_le32(bytes + offset + 12);
		uint32_t dataStart = read_le32(bytes + offset + 16);
		uint32_t dataEnd = read_le32(bytes + offset + 20);
		if (vaEnd <= vaStart || ((vaStart | vaEnd) & 4095) != 0
			|| dataEnd < dataStart || dataEnd > size
			|| dataEnd - dataStart > vaEnd - vaStart) {
			return FIRMWARE_BAD_DATA;
		}

		FirmwareSection section = read_section(bytes + offset);
		if ((section.flags & ~kSectionFlags) != 0)
			return FIRMWARE_UNSUPPORTED;
		if ((section.flags & SECTION_PROTECTED) != 0) {
			// Protected execution needs a separate platform implementation.
			// Its sections must never enter the ordinary MCU address space.
			info.protectedSectionCount++;
			offset += entrySize;
			continue;
		}
		if (++info.sectionCount > kMaxFirmwareSections)
			return FIRMWARE_LIMIT_EXCEEDED;
		info.mappedBytes += section.memorySize;
		if (info.mappedBytes > kMaxFirmwareMappedBytes)
			return FIRMWARE_LIMIT_EXCEEDED;

		if (vaStart == kFirmwareSharedStart) {
			uint32_t required = SECTION_SHARED | SECTION_READ | SECTION_WRITE;
			if (sharedFound || (section.flags & required) != required
				|| vaEnd > kFirmwareSharedEnd) {
				return FIRMWARE_BAD_DATA;
			}
			sharedFound = true;
		}

		// Aliased file data is legitimate, but MCU virtual mappings may not
		// overlap. Earlier records have already passed all length checks.
		for (uint32_t previous = kHeaderSize; previous < offset;) {
			uint32_t oldHeader = read_le32(bytes + previous);
			if ((oldHeader & 0xff) == 0) {
				FirmwareSection old = read_section(bytes + previous);
				if ((old.flags & SECTION_PROTECTED) == 0
					&& vaStart < uint64_t(old.virtualAddress) + old.memorySize
					&& old.virtualAddress < vaEnd) {
					return FIRMWARE_BAD_DATA;
				}
			}
			previous += (oldHeader >> 8) & 0xff;
		}
		offset += entrySize;
	}
	if (!sharedFound)
		return FIRMWARE_BAD_DATA;

	fData = bytes;
	fInfo = info;
	return FIRMWARE_OK;
}


FirmwareStatus
FirmwareImage::GetSection(uint32_t index, FirmwareSection& section) const
{
	if (fData == NULL || index >= fInfo.sectionCount)
		return FIRMWARE_BAD_ARGUMENT;
	for (uint32_t offset = kHeaderSize; offset < fInfo.tableSize;) {
		uint32_t header = read_le32(fData + offset);
		if ((header & 0xff) == 0) {
			FirmwareSection current = read_section(fData + offset);
			if ((current.flags & SECTION_PROTECTED) == 0 && index-- == 0) {
				section = current;
				return FIRMWARE_OK;
			}
		}
		offset += (header >> 8) & 0xff;
	}
	return FIRMWARE_BAD_ARGUMENT;
}


FirmwareStatus
FirmwareImage::CopySection(uint32_t index, void* destination, size_t size) const
{
	FirmwareSection section;
	if (GetSection(index, section) != FIRMWARE_OK || destination == NULL
		|| size < section.memorySize) {
		return FIRMWARE_BAD_ARGUMENT;
	}
	// New backing memory is initialized in full, even without SECTION_ZERO.
	// GPU-visible padding must not contain earlier host allocations.
	memset(destination, 0, section.memorySize);
	memcpy(destination, fData + section.dataOffset, section.dataSize);
	return FIRMWARE_OK;
}


const char*
FirmwareStatusName(FirmwareStatus status)
{
	switch (status) {
		case FIRMWARE_OK: return "ok";
		case FIRMWARE_BAD_DATA: return "invalid firmware container";
		case FIRMWARE_UNSUPPORTED: return "unsupported firmware format";
		case FIRMWARE_LIMIT_EXCEEDED: return "firmware exceeds loader limits";
		case FIRMWARE_BAD_ARGUMENT: return "invalid buffer or section";
		default: return "unknown firmware error";
	}
}

} // namespace MaliCSF
