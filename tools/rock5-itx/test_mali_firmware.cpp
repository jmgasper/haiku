/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfFirmware.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace MaliCSF;
using std::vector;

static void put(vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		bytes.at(offset + i) = uint8_t(value >> (8 * i));
}

static void section(vector<uint8_t>& bytes, size_t offset, uint32_t flags,
	uint32_t start, uint32_t end, uint32_t first, uint32_t last)
{
	put(bytes, offset, 24 << 8);
	put(bytes, offset + 4, flags);
	put(bytes, offset + 8, start);
	put(bytes, offset + 12, end);
	put(bytes, offset + 16, first);
	put(bytes, offset + 20, last);
}

static vector<uint8_t> fixture()
{
	vector<uint8_t> bytes(112, 0);
	put(bytes, 0, 0xc3f13a6e);
	bytes[4] = 3;
	put(bytes, 8, 0x01050000);
	put(bytes, 16, 96);
	section(bytes, 20, 13, 0x800000, 0x801000, 96, 103);
	section(bytes, 44, 43, 0x3000000, 0x3001000, 0, 0);
	section(bytes, 68, 0xc000001b, 0x4000000, 0x4001000, 104, 112);
	put(bytes, 92, 0x800004ee); // Unknown optional metadata, with no payload.
	for (size_t i = 96; i < bytes.size(); i++)
		bytes[i] = uint8_t(i);
	return bytes;
}

static void expect_bad(vector<uint8_t> bytes, size_t offset, uint32_t value)
{
	put(bytes, offset, value);
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) != FIRMWARE_OK);
	assert(image.Info().sectionCount == 0);
}

static void valid_and_copy()
{
	auto bytes = fixture();
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	assert(image.Info().minor == 3 && image.Info().versionHash == 0x01050000);
	assert(image.Info().tableSize == 96 && image.Info().sectionCount == 2);
	assert(image.Info().protectedSectionCount == 1);
	assert(image.Info().ignoredEntryCount == 1 && image.Info().mappedBytes == 8192);
	FirmwareSection s;
	assert(image.GetSection(0, s) == FIRMWARE_OK);
	assert(s.virtualAddress == 0x800000 && s.flags == 13 && s.dataSize == 7);
	assert(image.GetSection(1, s) == FIRMWARE_OK);
	assert(s.virtualAddress == 0x4000000 && s.flags == 0xc000001b);
	assert(image.GetSection(2, s) == FIRMWARE_BAD_ARGUMENT);
	assert(image.GetSection(UINT32_MAX, s) == FIRMWARE_BAD_ARGUMENT);
	for (uint32_t i = 0; i < 2; i++) {
		vector<uint8_t> copy(4096 + 2 * 16, 0xa5);
		assert(image.CopySection(i, copy.data() + 16, 4095) == FIRMWARE_BAD_ARGUMENT);
		for (auto byte : copy) assert(byte == 0xa5);
		assert(image.CopySection(i, copy.data() + 16, 4096) == FIRMWARE_OK);
		for (size_t n = 0; n < 16; n++) {
			assert(copy[n] == 0xa5 && copy[4096 + 16 + n] == 0xa5);
		}
		size_t dataStart = i == 0 ? 96 : 104, dataSize = i == 0 ? 7 : 8;
		for (size_t n = 0; n < 4096; n++)
			assert(copy[16 + n] == (n < dataSize ? uint8_t(dataStart + n) : 0));
	}
	assert(image.CopySection(0, NULL, 4096) == FIRMWARE_BAD_ARGUMENT);
	assert(image.Init(NULL, 0) == FIRMWARE_BAD_ARGUMENT);
	assert(image.Info().sectionCount == 0 && image.Info().mappedBytes == 0);
	assert(image.GetSection(0, s) == FIRMWARE_BAD_ARGUMENT);
	assert(image.Init(bytes.data(), kMaxFirmwareBytes + 1) == FIRMWARE_LIMIT_EXCEEDED);
	puts("valid layout, protected exclusion, guarded copy and stale-view checks pass");
}

static void invalid_layouts()
{
	auto bytes = fixture();
	for (size_t size = 0; size < bytes.size(); size++) {
		FirmwareImage image;
		assert(image.Init(bytes.data(), size) != FIRMWARE_OK);
	}
	expect_bad(bytes, 0, 0); // Magic.
	expect_bad(bytes, 4, 0x103); // Unsupported major version.
	expect_bad(bytes, 4, 0x10003); // Reserved header field.
	expect_bad(bytes, 12, 1);
	for (uint32_t size : {0U, 16U, 19U, 20U, 93U, 97U, 116U, UINT32_MAX})
		expect_bad(bytes, 16, size);
	for (uint32_t size : {0U, 2U, 4U, 20U, 23U, 100U, 252U})
		expect_bad(bytes, 20, size << 8);
	expect_bad(bytes, 92, 0x000004ee); // Unknown required entry.
	expect_bad(bytes, 24, 0x100d); // Unknown section flags.
	expect_bad(bytes, 28, 0x800001); // Unaligned start.
	expect_bad(bytes, 32, 0x801001); // Unaligned end.
	expect_bad(bytes, 32, 0x800000); // Empty mapping.
	expect_bad(bytes, 32, 0x7ff000); // Reversed virtual range.
	expect_bad(bytes, 36, 104); // Reversed data range.
	expect_bad(bytes, 40, UINT32_MAX); // Out-of-file data.
	expect_bad(bytes, 76, 0x800000); // Overlap and missing shared region.
	expect_bad(bytes, 72, 0x8000001b); // Shared marker missing.
	expect_bad(bytes, 72, 0xc0000019); // Shared interface not writable.
	expect_bad(bytes, 80, kFirmwareSharedEnd + 4096); // Shared limit.
	bytes.resize(5000);
	expect_bad(bytes, 40, 4900); // Data larger than backing memory.
	bytes = fixture();
	put(bytes, 48, 11); // Ordinary mapping instead of protected entry.
	put(bytes, 52, 0x800000);
	put(bytes, 56, 0x801000);
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_BAD_DATA);
	// File-data aliases in distinct virtual ranges are allowed.
	put(bytes, 52, 0x900000); put(bytes, 56, 0x901000);
	put(bytes, 60, 96); put(bytes, 64, 103);
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	assert(image.Info().sectionCount == 3);
	puts("truncation, table/entry/range validation and alias checks pass");
}

static void bounds_and_limits()
{
	auto bytes = fixture();
	FirmwareImage image;
	expect_bad(bytes, 32, 0x800000 + 64 * 1024 * 1024);
	// More sections than the loader admits, all distinct and individually valid.
	vector<uint8_t> many(20 + 65 * 24, 0);
	put(many, 0, 0xc3f13a6e); put(many, 16, many.size());
	for (uint32_t i = 0; i < 65; i++)
		section(many, 20 + i * 24, 11, i * 4096, (i + 1) * 4096, 0, 0);
	assert(image.Init(many.data(), many.size()) == FIRMWARE_LIMIT_EXCEEDED);
	// The source ends directly against an inaccessible page, and each possible
	// short input is tried. This catches reads hidden by spare vector capacity.
	size_t pageSize = sysconf(_SC_PAGESIZE);
	uint8_t* pages = static_cast<uint8_t*>(mmap(NULL, pageSize * 2,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	assert(pages != MAP_FAILED);
	assert(mprotect(pages + pageSize, pageSize, PROT_NONE) == 0);
	for (size_t size = 0; size <= bytes.size(); size++) {
		uint8_t* data = pages + pageSize - size;
		memcpy(data, bytes.data(), size);
		assert((image.Init(data, size) == FIRMWARE_OK) == (size == bytes.size()));
	}
	// A full valid view starting at an unaligned host address.
	memcpy(pages + 1, bytes.data(), bytes.size());
	assert(image.Init(pages + 1, bytes.size()) == FIRMWARE_OK);
	assert(munmap(pages, pageSize * 2) == 0);
	puts("allocation limits, guarded reads and unaligned input checks pass");
}

int main()
{
	valid_and_copy();
	invalid_layouts();
	bounds_and_limits();
	puts("MALI_CSF_FIRMWARE_TEST_PASS");
}
