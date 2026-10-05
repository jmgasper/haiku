/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "RealtekBluetoothFirmware.h"

#include <string.h>


namespace RealtekBluetooth {


const uint8_t kRegisterChipSubversion[5] = { 0x10, 0x38, 0x04, 0x28, 0x80 };
const uint8_t kRegisterChipRevision[5] = { 0x10, 0x3a, 0x04, 0x28, 0x80 };
const uint8_t kRegisterSecurityProject[5] = { 0x10, 0xa4, 0xad, 0x00, 0xb0 };


static const UsbId kListedUsbDevices[] = {
	{ 0x0489, 0xe085 },
	{ 0x0489, 0xe08b },
	{ 0x0489, 0xe112 },
	{ 0x0489, 0xe122 },
	{ 0x0489, 0xe123 },
	{ 0x0489, 0xe125 },
	{ 0x0489, 0xe12f },
	{ 0x0489, 0xe130 },
	{ 0x04c5, 0x161f },
	{ 0x04c5, 0x165c },
	{ 0x04c5, 0x1675 },
	{ 0x04ca, 0x4005 },
	{ 0x04ca, 0x4006 },
	{ 0x04ca, 0x4007 },
	{ 0x04f2, 0xb49f },
	{ 0x0930, 0x021d },
	{ 0x0b05, 0x17dc },
	{ 0x0b05, 0x185c },
	{ 0x0b05, 0x18ef },
	{ 0x0b05, 0x190e },
	{ 0x0b05, 0x1bef },
	{ 0x0b05, 0x1d70 },
	{ 0x0bda, 0x2852 },
	{ 0x0bda, 0x385a },
	{ 0x0bda, 0x4852 },
	{ 0x0bda, 0x4853 },
	{ 0x0bda, 0x8520 },
	{ 0x0bda, 0x8771 },
	{ 0x0bda, 0x887b },
	{ 0x0bda, 0x8922 },
	{ 0x0bda, 0xa728 },
	{ 0x0bda, 0xb009 },
	{ 0x0bda, 0xb00c },
	{ 0x0bda, 0xb850 },
	{ 0x0bda, 0xb85b },
	{ 0x0bda, 0xc123 },
	{ 0x0bda, 0xc822 },
	{ 0x0bda, 0xc852 },
	{ 0x0cb5, 0xc547 },
	{ 0x0cb8, 0xc549 },
	{ 0x0cb8, 0xc558 },
	{ 0x0cb8, 0xc559 },
	{ 0x1358, 0xc123 },
	{ 0x13d3, 0x3394 },
	{ 0x13d3, 0x3410 },
	{ 0x13d3, 0x3414 },
	{ 0x13d3, 0x3416 },
	{ 0x13d3, 0x3458 },
	{ 0x13d3, 0x3459 },
	{ 0x13d3, 0x3461 },
	{ 0x13d3, 0x3462 },
	{ 0x13d3, 0x3494 },
	{ 0x13d3, 0x3526 },
	{ 0x13d3, 0x3529 },
	{ 0x13d3, 0x3533 },
	{ 0x13d3, 0x3548 },
	{ 0x13d3, 0x3549 },
	{ 0x13d3, 0x3553 },
	{ 0x13d3, 0x3555 },
	{ 0x13d3, 0x3570 },
	{ 0x13d3, 0x3571 },
	{ 0x13d3, 0x3572 },
	{ 0x13d3, 0x3586 },
	{ 0x13d3, 0x3587 },
	{ 0x13d3, 0x3591 },
	{ 0x13d3, 0x3592 },
	{ 0x13d3, 0x3600 },
	{ 0x13d3, 0x3601 },
	{ 0x13d3, 0x3612 },
	{ 0x13d3, 0x3616 },
	{ 0x13d3, 0x3617 },
	{ 0x13d3, 0x3618 },
	{ 0x2001, 0x332a },
	{ 0x2357, 0x0604 },
	{ 0x2550, 0x8761 },
	{ 0x2b89, 0x6275 },
	{ 0x2b89, 0x8761 },
	{ 0x2c0a, 0x8761 },
	{ 0x2c4e, 0x0128 },
	{ 0x2ff8, 0x3051 },
	{ 0x2ff8, 0xb011 },
	{ 0x3625, 0x010b },
	{ 0x37ad, 0x0600 },
	{ 0x6655, 0x8771 },
	{ 0x7392, 0xa611 },
	{ 0x7392, 0xc611 },
	{ 0x7392, 0xe611 },
};


static const IcInfo kIcs[] = {
	{ kLmp8723A, 0xb, 0x6, false, false, "rtl8723a_fw", NULL, "RTL8723AU" },
	{ kLmp8723B, 0xb, 0x6, false, true, "rtl8723b_fw", "rtl8723b_config",
		"RTL8723BU" },
	{ kLmp8723B, 0xd, 0x8, true, true, "rtl8723d_fw", "rtl8723d_config",
		"RTL8723DU" },
	{ kLmp8821A, 0xa, 0x6, false, true, "rtl8821a_fw", "rtl8821a_config",
		"RTL8821AU" },
	{ kLmp8821A, 0xc, 0x8, false, true, "rtl8821c_fw", "rtl8821c_config",
		"RTL8821CU" },
	{ kLmp8761A, 0xa, 0x6, false, true, "rtl8761a_fw", "rtl8761a_config",
		"RTL8761AU" },
	{ kLmp8761A, 0xb, 0xa, false, true, "rtl8761bu_fw", "rtl8761bu_config",
		"RTL8761BU" },
	{ kLmp8822B, 0xc, 0xa, false, true, "rtl8822cu_fw", "rtl8822cu_config",
		"RTL8822CU" },
	{ kLmp8822B, 0xb, 0x7, true, true, "rtl8822b_fw", "rtl8822b_config",
		"RTL8822BU" },
	{ kLmp8852A, 0xa, 0xb, false, true, "rtl8852au_fw", "rtl8852au_config",
		"RTL8852AU" },
	{ kLmp8852A, 0xb, 0xb, false, true, "rtl8852bu_fw", "rtl8852bu_config",
		"RTL8852BU" },
	{ kLmp8852A, 0xc, 0xc, false, true, "rtl8852cu_fw", "rtl8852cu_config",
		"RTL8852CU" },
	{ kLmp8851B, 0xb, 0xc, false, true, "rtl8851bu_fw", "rtl8851bu_config",
		"RTL8851BU" },
	{ kLmp8922A, 0xa, 0xc, false, true, "rtl8922au_fw", "rtl8922au_config",
		"RTL8922AU" },
	{ kLmp8852A, 0x87, 0xc, false, true, "rtl8852btu_fw", "rtl8852btu_config",
		"RTL8852BTU" },
};


// Project IDs in the firmware files, and the LMP subversion they are for.
static const struct {
	uint16_t	lmpSubversion;
	uint8_t		id;
} kProjects[] = {
	{ kLmp8723A, 0 },
	{ kLmp8723B, 1 },
	{ kLmp8821A, 2 },
	{ kLmp8761A, 3 },
	{ 0x8703, 7 },
	{ kLmp8822B, 8 },
	{ kLmp8723B, 9 },	// 8723D
	{ kLmp8821A, 10 },	// 8821C
	{ kLmp8822B, 13 },	// 8822C
	{ kLmp8761A, 14 },	// 8761B
	{ kLmp8852A, 18 },	// 8852A
	{ kLmp8852A, 20 },	// 8852B
	{ kLmp8852A, 25 },	// 8852C
	{ kLmp8851B, 36 },	// 8851B
	{ kLmp8922A, 44 },	// 8922A
	{ kLmp8852A, 47 },	// 8852BT
};


static const char kSignatureV1[8] = { 'R', 'e', 'a', 'l', 't', 'e', 'c', 'h' };
static const char kSignatureV2[8] = { 'R', 'T', 'B', 'T', 'C', 'o', 'r', 'e' };
static const uint8_t kExtensionSignature[4] = { 0x51, 0x04, 0xfd, 0x77 };

static const size_t kHeaderV1Length = 14;	// signature, version, count
static const size_t kHeaderV2Length = 20;	// signature, version[8], count

static const uint32_t kSectionSnippets = 0x01;
static const uint32_t kSectionDummyHeader = 0x02;
static const uint32_t kSectionSecurityHeader = 0x03;


static uint16_t
le16(const uint8_t* data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}


static uint32_t
le32(const uint8_t* data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
		| ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}


bool
IsListedUsbDevice(uint16_t vendor, uint16_t product)
{
	for (size_t i = 0; i < CountListedUsbDevices(); i++) {
		if (kListedUsbDevices[i].vendor == vendor
			&& kListedUsbDevices[i].product == product) {
			return true;
		}
	}
	return false;
}


size_t
CountListedUsbDevices()
{
	return sizeof(kListedUsbDevices) / sizeof(kListedUsbDevices[0]);
}


const UsbId*
ListedUsbDeviceAt(size_t index)
{
	return index < CountListedUsbDevices() ? &kListedUsbDevices[index] : NULL;
}


const IcInfo*
MatchIc(uint16_t lmpSubversion, uint16_t hciRevision, uint8_t hciVersion)
{
	for (size_t i = 0; i < CountIcs(); i++) {
		if (kIcs[i].lmpSubversion == lmpSubversion
			&& kIcs[i].hciRevision == hciRevision
			&& kIcs[i].hciVersion == hciVersion) {
			return &kIcs[i];
		}
	}
	return NULL;
}


size_t
CountIcs()
{
	return sizeof(kIcs) / sizeof(kIcs[0]);
}


const IcInfo*
IcAt(size_t index)
{
	return index < CountIcs() ? &kIcs[index] : NULL;
}


struct Subsection {
	uint8_t			priority;
	const uint8_t*	data;
	uint32_t		length;
};


// btrtl_parse_section(): keeps the subsections for this ROM version (and,
// in security headers, this key), ordered by priority. A new entry goes in
// front of existing ones of equal priority, like btrtl_insert_ordered_subsec.
static void
ParseSection(uint32_t opcode, const uint8_t* data, uint32_t length,
	uint8_t romVersion, uint8_t keyId, std::vector<Subsection>& subsections)
{
	if (length < 4)
		return;
	const uint16_t count = le16(data);
	uint32_t offset = 4;
	for (uint16_t i = 0; i < count; i++) {
		if (length - offset < 8)
			break;
		const uint8_t* header = data + offset;
		const uint32_t subsectionLength = le32(header + 4);
		offset += 8;
		if (length - offset < subsectionLength)
			break;
		const uint8_t* body = data + offset;
		offset += subsectionLength;

		if (header[0] != romVersion + 1)
			continue;
		if (opcode == kSectionSecurityHeader && header[2] != keyId)
			continue;

		Subsection subsection = { header[1], body, subsectionLength };
		size_t position = 0;
		while (position < subsections.size()
			&& subsections[position].priority < subsection.priority) {
			position++;
		}
		subsections.insert(subsections.begin() + position, subsection);
	}
}


static bool
ParseV2(const uint8_t* firmware, size_t size, uint8_t romVersion,
	uint8_t keyId, std::vector<uint8_t>& patch, const char** error)
{
	// The last seven bytes are the project ID trailer, not sections.
	const size_t length = size - 7;
	const uint32_t count = le32(firmware + 16);
	std::vector<Subsection> subsections;
	size_t offset = kHeaderV2Length;
	for (uint32_t i = 0; i < count; i++) {
		if (length - offset < 8)
			break;
		const uint32_t opcode = le32(firmware + offset);
		const uint32_t sectionLength = le32(firmware + offset + 4);
		offset += 8;
		if (length - offset < sectionLength)
			break;
		const uint8_t* section = firmware + offset;
		offset += sectionLength;

		switch (opcode) {
			case kSectionSecurityHeader:
				// A controller without a key ignores all security headers.
				if (keyId == 0)
					break;
				// fall through
			case kSectionSnippets:
			case kSectionDummyHeader:
				ParseSection(opcode, section, sectionLength, romVersion,
					keyId, subsections);
				break;
		}
	}

	patch.clear();
	for (size_t i = 0; i < subsections.size(); i++) {
		patch.insert(patch.end(), subsections[i].data,
			subsections[i].data + subsections[i].length);
	}
	if (patch.empty()) {
		*error = "no patch for this ROM version in the v2 firmware";
		return false;
	}
	return true;
}


bool
BuildPatch(const IcInfo& ic, uint8_t romVersion, uint8_t keyId,
	const uint8_t* firmware, size_t size, const uint8_t* config,
	size_t configSize, std::vector<uint8_t>& patch, int& projectId,
	const char** error)
{
	*error = NULL;
	projectId = -1;
	patch.clear();

	if (ic.lmpSubversion == kLmp8723A) {
		// The oldest part takes its file as it is, without an epatch header.
		// (Linux only rejects the v1 signature; a v2 file is just as wrong.)
		if (size < 8 || memcmp(firmware, kSignatureV1, 8) == 0
			|| memcmp(firmware, kSignatureV2, 8) == 0) {
			*error = "unexpected epatch signature for an RTL8723A";
			return false;
		}
		patch.assign(firmware, firmware + size);
		return true;
	}

	if (size <= 8) {
		*error = "firmware file too short";
		return false;
	}
	const bool v1 = memcmp(firmware, kSignatureV1, 8) == 0;
	const bool v2 = memcmp(firmware, kSignatureV2, 8) == 0;
	if (!v1 && !v2) {
		*error = "bad epatch signature";
		return false;
	}
	size_t minimum = (v1 ? kHeaderV1Length : kHeaderV2Length)
		+ sizeof(kExtensionSignature) + 3;
	if (size < minimum) {
		*error = "firmware file too short";
		return false;
	}
	if (memcmp(firmware + size - sizeof(kExtensionSignature),
			kExtensionSignature, sizeof(kExtensionSignature)) != 0) {
		*error = "extension section signature mismatch";
		return false;
	}

	// Walk the instructions backwards from the end to the project ID.
	const uint8_t* cursor = firmware + size - sizeof(kExtensionSignature);
	while (cursor >= firmware + kHeaderV1Length + 3) {
		const uint8_t opcode = *--cursor;
		const uint8_t length = *--cursor;
		const uint8_t data = *--cursor;
		if (opcode == 0xff)
			break;
		if (length == 0) {
			*error = "instruction with length 0";
			return false;
		}
		if (opcode == 0 && length == 1) {
			projectId = data;
			break;
		}
		if ((size_t)(cursor - firmware) < length)
			break;
		cursor -= length;
	}
	if (projectId < 0) {
		*error = "no project ID in the firmware";
		return false;
	}
	size_t project = 0;
	while (project < sizeof(kProjects) / sizeof(kProjects[0])
		&& kProjects[project].id != projectId) {
		project++;
	}
	if (project == sizeof(kProjects) / sizeof(kProjects[0])) {
		*error = "unknown project ID";
		return false;
	}
	if (kProjects[project].lmpSubversion != ic.lmpSubversion) {
		*error = "firmware is for a different chip";
		return false;
	}

	if (v2) {
		if (!ParseV2(firmware, size, romVersion, keyId, patch, error))
			return false;
	} else {
		const uint16_t count = le16(firmware + 12);
		minimum += 8 * (size_t)count;
		if (size < minimum) {
			*error = "patch table runs past the end of the file";
			return false;
		}
		const uint8_t* chipIds = firmware + kHeaderV1Length;
		const uint8_t* lengths = chipIds + 2 * count;
		const uint8_t* offsets = lengths + 2 * count;
		uint16_t patchLength = 0;
		uint32_t patchOffset = 0;
		for (uint16_t i = 0; i < count; i++) {
			if (le16(chipIds + 2 * i) == romVersion + 1) {
				patchLength = le16(lengths + 2 * i);
				patchOffset = le32(offsets + 4 * i);
				break;
			}
		}
		if (patchOffset == 0) {
			*error = "no patch for this ROM version";
			return false;
		}
		if (patchLength < 4 || patchOffset > size
			|| patchLength > size - patchOffset) {
			*error = "patch runs past the end of the file";
			return false;
		}
		// The patch ends with the firmware version from the header.
		patch.assign(firmware + patchOffset,
			firmware + patchOffset + patchLength - 4);
		patch.insert(patch.end(), firmware + 8, firmware + 12);
	}

	if (config != NULL && configSize > 0)
		patch.insert(patch.end(), config, config + configSize);
	return true;
}


size_t
CountFragments(size_t length)
{
	return length / kFragmentLength + 1;
}


void
FragmentAt(size_t length, size_t fragment, uint8_t& index, size_t& size)
{
	// Indices count 0..0x7f, then wrap to 1 (btrtl.c rtl_download_firmware).
	index = fragment <= 0x7f ? fragment : 1 + (fragment - 0x80) % 0x7f;
	size = kFragmentLength;
	if (fragment == CountFragments(length) - 1) {
		index |= 0x80;
		size = length % kFragmentLength;
	}
}


}	// namespace RealtekBluetooth
