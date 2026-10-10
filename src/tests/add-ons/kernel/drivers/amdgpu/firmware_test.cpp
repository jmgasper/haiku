/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static void
Put16(std::vector<uint8_t>& p, size_t offset, uint16_t value)
{
	p[offset] = value;
	p[offset + 1] = value >> 8;
}

static void
Put32(std::vector<uint8_t>& p, size_t offset, uint32_t value)
{
	Put16(p, offset, value);
	Put16(p, offset + 2, value >> 16);
}

int
main(int argc, char** argv)
{
	assert(argc == 2);
	FILE* file = fopen(argv[1], "rb");
	assert(file != NULL);
	assert(fseek(file, 0, SEEK_END) == 0);
	long size = ftell(file);
	assert(size > 0 && size < 65536);
	rewind(file);
	std::vector<uint8_t> firmware(size);
	assert(fread(firmware.data(), 1, size, file) == (size_t)size);
	fclose(file);
	amdgpu::FirmwareView view;
	assert(amdgpu::ParseSdmaFirmware(firmware.data(), size, view));
	printf("SDMA firmware version %u, feature %u, %u bytes\n",
		view.version, view.featureVersion, view.codeSize);
	// Every possible truncation; lengths in the original header still refer
	// to the complete file. Parser must never read beyond the supplied size.
	for (size_t n = 0; n < firmware.size(); n++)
		assert(!amdgpu::ParseSdmaFirmware(firmware.data(), n, view));
	std::vector<uint8_t> unaligned(firmware.size() + 1);
	memcpy(unaligned.data() + 1, firmware.data(), firmware.size());
	assert(amdgpu::ParseSdmaFirmware(unaligned.data() + 1, size, view));
	for (size_t field : {size_t(0), size_t(4), size_t(20), size_t(24),
			size_t(40), size_t(44)}) {
		auto bad = firmware;
		Put32(bad, field, 0xfffffffc);
		assert(!amdgpu::ParseSdmaFirmware(bad.data(), bad.size(), view));
	}
	for (size_t field : {size_t(8), size_t(10), size_t(12), size_t(14)}) {
		auto bad = firmware;
		Put16(bad, field, 0xffff);
		assert(!amdgpu::ParseSdmaFirmware(bad.data(), bad.size(), view));
	}
	// A compact ATOM fixture with a nonzero reservation above 4 GiB checks
	// KiB units, 64-bit arithmetic, every truncation and corrupt table offsets.
	std::vector<uint8_t> rom(0x10c);
	rom[0] = 0x55; rom[1] = 0xaa;
	Put16(rom, 0x48, 0x80); Put16(rom, 0x80, 36);
	memcpy(rom.data() + 0x84, "ATOM", 4);
	Put16(rom, 0xa0, 0xc0); Put16(rom, 0xc0, 28);
	Put16(rom, 0xda, 0x100); Put16(rom, 0x100, 12);
	rom[0x102] = 1; rom[0x103] = 4;
	Put32(rom, 0x104, 0x807f0000); Put16(rom, 0x108, 128);
	Put16(rom, 0x10a, 16);
	amdgpu::AtomVramReservation reservation;
	assert(amdgpu::ParseAtomVramReservation(rom.data(), rom.size(), reservation));
	assert(reservation.start == 0x1fc000000ULL && reservation.size == 131072
		&& reservation.driverScratchSize == 16384);
	for (size_t n = 0; n < rom.size(); n++)
		assert(!amdgpu::ParseAtomVramReservation(rom.data(), n, reservation));
	for (size_t field : {size_t(0x48), size_t(0xa0), size_t(0xda),
			size_t(0x80), size_t(0xc0), size_t(0x100)}) {
		auto bad = rom;
		Put16(bad, field, 0xffff);
		assert(!amdgpu::ParseAtomVramReservation(bad.data(), bad.size(), reservation));
	}
	puts("PASS: firmware and ATOM bounds, identity, alignment and 64-bit reservation");
}
