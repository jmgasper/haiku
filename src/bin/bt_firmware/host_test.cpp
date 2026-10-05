/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Build-host check of the Intel and Realtek Bluetooth firmware logic
 * against a decompressed linux-firmware tree (intel/ and rtl_bt/):
 *
 *   g++ -O1 -Wall -o bt_firmware_host_test host_test.cpp \
 *       IntelBluetoothFirmware.cpp RealtekBluetoothFirmware.cpp
 *   ./bt_firmware_host_test /path/to/firmware
 */


#include "IntelBluetoothFirmware.h"
#include "RealtekBluetoothFirmware.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>


using namespace IntelBluetooth;


static int sFailures = 0;


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			sFailures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)


static bool
ReadFile(const std::string& path, std::vector<uint8_t>& data)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == NULL)
		return false;
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	data.resize(size > 0 ? size : 0);
	bool ok = size >= 0
		&& fread(data.data(), 1, data.size(), file) == data.size();
	fclose(file);
	return ok;
}


static bool
Exists(const std::string& directory, const char* name)
{
	struct stat info;
	return stat((directory + "/" + name).c_str(), &info) == 0;
}


static void
TestNaming(const std::string& directory)
{
	char name[64];

	// AX210 on the ROCK 5 ITX: Linux loaded ibt-0041-0041.sfi.
	TlvVersion ax210;
	memset(&ax210, 0, sizeof(ax210));
	ax210.cnviTop = 0x00000410;
	ax210.cnvrTop = 0x00000410;
	ax210.cnviBt = 0x00173700;
	ax210.imageType = kImageBootloader;
	TlvFirmwareName(ax210, "sfi", name, sizeof(name));
	CHECK(strcmp(name, "ibt-0041-0041.sfi") == 0, "AX210 name %s", name);
	CHECK(TlvGeneration(HardwareVariant(ax210.cnviBt)) == kGenerationTlv,
		"AX210 generation");
	CHECK(HardwarePlatform(ax210.cnviBt) == 0x37, "AX210 platform");

	// Blazar: intermediate loader first, then the transport specific image.
	TlvVersion blazar;
	memset(&blazar, 0, sizeof(blazar));
	blazar.cnviTop = 0x01000900;
	blazar.cnvrTop = 0x00000410;
	blazar.cnviBt = 0x001e3700;
	blazar.imageType = kImageBootloader;
	TlvFirmwareName(blazar, "sfi", name, sizeof(name));
	CHECK(strcmp(name, "ibt-0190-0041-iml.sfi") == 0, "Blazar IML %s", name);
	CHECK(Exists(directory, name), "missing %s", name);
	blazar.imageType = kImageIntermediate;
	strcpy(blazar.firmwareId, "usb");
	TlvFirmwareName(blazar, "sfi", name, sizeof(name));
	CHECK(strcmp(name, "ibt-0190-0041-usb.sfi") == 0, "Blazar op %s", name);
	CHECK(Exists(directory, name), "missing %s", name);
	TlvFirmwareName(blazar, "ddc", name, sizeof(name));
	CHECK(strcmp(name, "ibt-0190-0041-usb.ddc") == 0, "Blazar ddc %s", name);
	// Without a firmware id Linux falls back to the plain name.
	blazar.firmwareId[0] = '\0';
	TlvFirmwareName(blazar, "sfi", name, sizeof(name));
	CHECK(strcmp(name, "ibt-0190-0041.sfi") == 0, "Blazar plain %s", name);

	// A TyP-era controller never uses the firmware id.
	TlvVersion gap = ax210;
	gap.cnviBt = 0x001c3700;
	strcpy(gap.firmwareId, "usb");
	gap.imageType = kImageBootloader;
	TlvFirmwareName(gap, "sfi", name, sizeof(name));
	CHECK(strcmp(name, "ibt-0041-0041.sfi") == 0, "GaP name %s", name);

	// Legacy bootloader parts.
	LegacyVersion legacy;
	memset(&legacy, 0, sizeof(legacy));
	legacy.hwPlatform = 0x37;
	legacy.hwVariant = 0x0b;
	CHECK(LegacyFirmwareName(legacy, 5, "sfi", name, sizeof(name))
		&& strcmp(name, "ibt-11-5.sfi") == 0, "8260 name %s", name);
	CHECK(Exists(directory, name), "missing %s", name);
	legacy.hwVariant = 0x0c;
	CHECK(LegacyFirmwareName(legacy, 16, "ddc", name, sizeof(name))
		&& strcmp(name, "ibt-12-16.ddc") == 0, "8265 name %s", name);
	CHECK(Exists(directory, name), "missing %s", name);
	legacy.hwVariant = 0x12;
	legacy.hwRevision = 16;
	legacy.fwRevision = 1;
	CHECK(LegacyFirmwareName(legacy, 0, "sfi", name, sizeof(name))
		&& strcmp(name, "ibt-18-16-1.sfi") == 0, "9260 name %s", name);
	CHECK(Exists(directory, name), "missing %s", name);
	legacy.hwVariant = 0x14;
	legacy.hwRevision = 0;
	legacy.fwRevision = 3;
	CHECK(LegacyFirmwareName(legacy, 0, "sfi", name, sizeof(name))
		&& strcmp(name, "ibt-20-0-3.sfi") == 0, "AX200 name %s", name);
	CHECK(Exists(directory, name), "missing %s", name);
	legacy.hwVariant = 0x08;
	CHECK(!LegacyFirmwareName(legacy, 0, "sfi", name, sizeof(name)),
		"ROM part must not have an SFI name");
	CHECK(LegacyGeneration(0x08) == kGenerationLegacyRom, "StP generation");
	CHECK(LegacyGeneration(0x14) == kGenerationLegacyBootloader,
		"CcP generation");
	CHECK(TlvGeneration(0x14) == kGenerationLegacyBootloader,
		"CcP TLV generation");
	CHECK(TlvGeneration(0x20) == kGenerationUnknown, "unknown variant");

	// ROM patch naming.
	LegacyVersion rom;
	memset(&rom, 0, sizeof(rom));
	rom.hwPlatform = 0x37;
	rom.hwVariant = 0x08;
	rom.hwRevision = 0x10;
	rom.fwVariant = 0x22;
	rom.fwRevision = 0x50;
	rom.fwBuildNumber = 0x19;
	rom.fwBuildWeek = 0x14;
	rom.fwBuildYear = 0x0f;
	char fallback[64];
	RomPatchNames(rom, name, sizeof(name), fallback, sizeof(fallback));
	CHECK(strcmp(name, "ibt-hw-37.8.10-fw-22.50.19.14.f.bseq") == 0,
		"ROM patch name %s", name);
	CHECK(Exists(directory, name), "missing %s", name);
	CHECK(strcmp(fallback, "ibt-hw-37.8.bseq") == 0, "ROM fallback %s",
		fallback);

	// Version replies as the AX210 returned them (image type and engine).
	const uint8_t tlvReply[] = {
		0x10, 4, 0x10, 0x04, 0x00, 0x00,
		0x11, 4, 0x10, 0x04, 0x00, 0x00,
		0x12, 4, 0x00, 0x37, 0x17, 0x00,
		0x1c, 1, 0x01,
		0x2f, 1, 0x01,
		0x50, 5, 'u', 's', 'b', 0, 0,
	};
	TlvVersion parsed;
	CHECK(ParseTlvVersion(tlvReply, sizeof(tlvReply), parsed),
		"TLV parse");
	CHECK(parsed.imageType == 1 && parsed.secureBootEngine == 1
		&& HardwareVariant(parsed.cnviBt) == 0x17
		&& strcmp(parsed.firmwareId, "usb") == 0, "TLV fields");
	CHECK(!IsLegacyVersionReply(tlvReply, sizeof(tlvReply)), "TLV legacy");
	const uint8_t truncated[] = { 0x10, 4, 0x10 };
	CHECK(!ParseTlvVersion(truncated, sizeof(truncated), parsed),
		"truncated TLV accepted");
	const uint8_t legacyReply[] = { 0x37, 0x0b, 0x10, 0x06, 0x00, 0, 0, 0,
		0 };
	CHECK(IsLegacyVersionReply(legacyReply, sizeof(legacyReply)),
		"legacy reply not recognized");

	CHECK(FindUsbDevice(0x8087, 0x0032) != NULL, "AX210 USB id");
	CHECK(FindUsbDevice(0x8087, 0x07da) == NULL, "CSR part is not Intel");
	CHECK(FindUsbDevice(0x0a12, 0x0032) == NULL, "vendor mismatch");
}


static bool
EndsWith(const std::string& text, const char* suffix)
{
	size_t length = strlen(suffix);
	return text.size() >= length
		&& text.compare(text.size() - length, length, suffix) == 0;
}


static void
TestFiles(const std::string& directory)
{
	DIR* dir = opendir(directory.c_str());
	CHECK(dir != NULL, "cannot open %s", directory.c_str());
	if (dir == NULL)
		return;

	int sfi = 0, ddc = 0, bseq = 0;
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		std::string name = entry->d_name;
		if (name.compare(0, 4, "ibt-") != 0)
			continue;
		std::vector<uint8_t> data;
		CHECK(ReadFile(directory + "/" + name, data), "read %s",
			name.c_str());

		if (EndsWith(name, ".sfi")) {
			sfi++;
			// ibt-<decimal>-... files are legacy bootloader images
			// (RSA only); ibt-<4 hex>-<4 hex>... are TLV images.
			bool tlv = name.size() > 8 && name[8] == '-'
				&& name.find('-', 4) == 8;
			uint8_t variant = tlv ? 0x17 : 0x12;
			for (int engine = 0; engine < (tlv ? 2 : 1); engine++) {
				SfiLayout layout;
				const char* error;
				bool ok = InspectSfi(data.data(), data.size(), tlv, variant,
					engine, layout, &error);
				CHECK(ok, "%s engine %d: %s", name.c_str(), engine,
					error != NULL ? error : "");
				if (ok && engine == 0) {
					printf("  %-24s %-4s payload@%zu boot=%08x build %u-%u.%u"
						" fragments=%zu\n", name.c_str(),
						tlv ? "TLV" : "RSA", layout.payloadOffset,
						layout.bootAddress, layout.buildNumber,
						layout.buildWeek, layout.buildYear,
						layout.fragments);
				}
			}
		} else if (EndsWith(name, ".ddc")) {
			ddc++;
			size_t offset = 0, length = 0;
			int records = 0;
			while (NextDdcRecord(data.data(), data.size(), offset, length)) {
				offset += length;
				records++;
			}
			CHECK(offset == data.size() && records > 0,
				"%s: DDC parse stopped at %zu of %zu", name.c_str(), offset,
				data.size());
		} else if (EndsWith(name, ".bseq")) {
			bseq++;
			size_t offset = 0;
			int steps = 0, patches = 0;
			PatchStep step;
			while (NextPatchStep(data.data(), data.size(), offset, step)) {
				steps++;
				if (step.opcode == kOpcodePatch)
					patches++;
			}
			CHECK(offset == data.size() && steps > 0,
				"%s: patch parse stopped at %zu of %zu", name.c_str(),
				offset, data.size());
			printf("  %-40s %d commands, %d patch writes\n", name.c_str(),
				steps, patches);
		}
	}
	closedir(dir);
	printf("checked %d sfi, %d ddc, %d bseq files\n", sfi, ddc, bseq);
	CHECK(sfi > 0 && ddc > 0 && bseq > 0, "firmware directory incomplete");
}


static void
TestRealtek(const std::string& directory)
{
	using namespace RealtekBluetooth;

	// Fragment indices: 0..0x7f, then 1..0x7f again; the last has bit 7.
	uint8_t index;
	size_t size;
	FragmentAt(1000, 0, index, size);
	CHECK(index == 0 && size == 252, "first fragment %u %zu", index, size);
	FragmentAt(1000, 3, index, size);
	CHECK(index == 0x83 && size == 1000 % 252, "last fragment %x %zu", index,
		size);
	CHECK(CountFragments(1000) == 4 && CountFragments(504) == 3,
		"fragment count");
	FragmentAt(100000, 0x7f, index, size);
	CHECK(index == 0x7f, "index 0x7f -> %x", index);
	FragmentAt(100000, 0x80, index, size);
	CHECK(index == 1, "index after wrap %x", index);
	FragmentAt(100000, 0x80 + 0x7e, index, size);
	CHECK(index == 0x7f, "second wrap end %x", index);
	FragmentAt(100000, 0x80 + 0x7f, index, size);
	CHECK(index == 1, "second wrap %x", index);

	CHECK(MatchIc(0x8761, 0xb, 0xa) != NULL
		&& strcmp(MatchIc(0x8761, 0xb, 0xa)->firmware, "rtl8761bu_fw") == 0,
		"RTL8761BU match");
	CHECK(MatchIc(0x8822, 0xc, 0xa) != NULL, "RTL8822CU match");
	CHECK(MatchIc(0x8822, 0xc, 0x8) == NULL, "UART-only entry matched");
	CHECK(IsListedUsbDevice(0x2357, 0x0604), "TP-Link UB500 listed");
	CHECK(!IsListedUsbDevice(0x8087, 0x0032), "Intel listed as Realtek");

	int built = 0;
	for (size_t i = 0; i < CountIcs(); i++) {
		const IcInfo* ic = IcAt(i);
		std::vector<uint8_t> firmware, config;
		std::string base = directory + "/rtl_bt/";
		if (!ReadFile(base + ic->firmware + ".bin", firmware)) {
			printf("  %-10s no %s.bin\n", ic->name, ic->firmware);
			continue;
		}
		if (ic->config != NULL)
			ReadFile(base + ic->config + ".bin", config);
		std::vector<int> romVersions;
		std::vector<uint8_t> patch;
		int projectId = -1;
		const char* error = NULL;
		for (int rom = 0; rom < 16; rom++) {
			if (BuildPatch(*ic, rom, 0, firmware.data(), firmware.size(),
					config.data(), config.size(), patch, projectId, &error)) {
				romVersions.push_back(rom);
			}
		}
		CHECK(!romVersions.empty(), "%s: no ROM version builds (%s)",
			ic->name, error != NULL ? error : "");
		if (romVersions.empty())
			continue;
		BuildPatch(*ic, romVersions[0], 0, firmware.data(), firmware.size(),
			config.data(), config.size(), patch, projectId, &error);
		CHECK(patch.size() > config.size(), "%s: empty patch", ic->name);
		std::string roms;
		for (size_t r = 0; r < romVersions.size(); r++)
			roms += (r ? "," : "") + std::to_string(romVersions[r]);
		printf("  %-10s %-18s project %2d, ROM %s: %zu byte patch + %zu "
			"config, %zu fragments\n", ic->name, ic->firmware, projectId,
			roms.c_str(), patch.size() - config.size(), config.size(),
			CountFragments(patch.size()));
		built++;

		// A file must never be accepted for a chip it was not built for.
		for (size_t j = 0; j < CountIcs(); j++) {
			const IcInfo* other = IcAt(j);
			if (other->lmpSubversion == ic->lmpSubversion)
				continue;
			CHECK(!BuildPatch(*other, romVersions[0], 0, firmware.data(),
				firmware.size(), NULL, 0, patch, projectId, &error),
				"%s accepted for %s", ic->firmware, other->name);
		}
	}
	printf("built Realtek patches for %d chips\n", built);
	CHECK(built >= 10, "too few Realtek chips built");
}


int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <firmware dir with intel/ and rtl_bt/>\n",
			argv[0]);
		return 2;
	}
	TestNaming(std::string(argv[1]) + "/intel");
	TestFiles(std::string(argv[1]) + "/intel");
	TestRealtek(argv[1]);
	printf(sFailures == 0 ? "PASS\n" : "%d FAILURES\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
