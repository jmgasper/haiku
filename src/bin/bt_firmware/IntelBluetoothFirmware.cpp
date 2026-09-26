/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "IntelBluetoothFirmware.h"

#include <stdio.h>
#include <string.h>


namespace IntelBluetooth {


// The Intel entries of Linux btusb.c (BTUSB_INTEL_COMBINED).
static const UsbDevice kUsbDevices[] = {
	{ 0x07dc, "Wireless 7260", true },
	{ 0x0a2a, "Wireless 7265 / 3160 / 3165", true },
	{ 0x0aa7, "Wireless-AC 3168", true },
	{ 0x0a2b, "Wireless 8260 / 8265", false },
	{ 0x0aaa, "Wireless-AC 9460 / 9560", false },
	{ 0x0025, "Wireless-AC 9260", false },
	{ 0x0026, "Wi-Fi 6 AX201", false },
	{ 0x0029, "Wi-Fi 6 AX200", false },
	{ 0x0032, "Wi-Fi 6E AX210", false },
	{ 0x0033, "Wi-Fi 6E AX211 / AX411", false },
	{ 0x0035, "Wi-Fi 7 BE2xx", false },
	{ 0x0036, "Wi-Fi 7 BE2xx", false },
	{ 0x0037, "Wi-Fi 7 BE2xx", false },
	{ 0x0038, "Wi-Fi 7 BE2xx", false },
	{ 0x0039, "Wi-Fi 7 BE2xx", false },
};


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


static uint16_t
swab16(uint16_t value)
{
	return (uint16_t)((value << 8) | (value >> 8));
}


// INTEL_CNVX_TOP_PACK_SWAB(INTEL_CNVX_TOP_TYPE(top), INTEL_CNVX_TOP_STEP(top))
static uint16_t
PackTop(uint32_t top)
{
	const uint32_t type = top & 0x00000fff;
	const uint32_t step = (top & 0x0f000000) >> 24;
	return swab16((uint16_t)((type << 4) | step));
}


const UsbDevice*
FindUsbDevice(uint16_t vendor, uint16_t product)
{
	if (vendor != kVendorIntel)
		return NULL;
	for (size_t i = 0; i < CountUsbDevices(); i++) {
		if (kUsbDevices[i].product == product)
			return &kUsbDevices[i];
	}
	return NULL;
}


size_t
CountUsbDevices()
{
	return sizeof(kUsbDevices) / sizeof(kUsbDevices[0]);
}


const UsbDevice*
UsbDeviceAt(size_t index)
{
	return index < CountUsbDevices() ? &kUsbDevices[index] : NULL;
}


const char*
VariantName(uint8_t hwVariant)
{
	switch (hwVariant) {
		case 0x07: return "WP (7260)";
		case 0x08: return "StP (7265/3160/3165/3168)";
		case 0x0b: return "SfP (8260)";
		case 0x0c: return "WsP (8265)";
		case 0x11: return "JfP (9460/9560)";
		case 0x12: return "ThP (9260)";
		case 0x13: return "HrP (AX201)";
		case 0x14: return "CcP (AX200)";
		case 0x17: return "TyP (AX210)";
		case 0x18: return "Slr (AX211)";
		case 0x19: return "Slr-F (AX211)";
		case 0x1b: return "Mgr (AX411)";
		case 0x1c: return "GaP (AX211)";
		case 0x1d: return "BlazarU (BE200)";
		case 0x1e: return "BlazarI (BE201)";
		case 0x1f: return "Scorpius Peak";
		case 0x22: return "BlazarIW";
		default: return "unknown";
	}
}


bool
IsLegacyVersionReply(const uint8_t* reply, size_t length)
{
	// Linux: skb->len == sizeof(struct intel_version) && data[1] == 0x37,
	// where the structure starts with the status byte.
	return length == 9 && reply[0] == 0x37;
}


bool
ParseLegacyVersion(const uint8_t* reply, size_t length, LegacyVersion& version)
{
	if (length != 9)
		return false;
	version.hwPlatform = reply[0];
	version.hwVariant = reply[1];
	version.hwRevision = reply[2];
	version.fwVariant = reply[3];
	version.fwRevision = reply[4];
	version.fwBuildNumber = reply[5];
	version.fwBuildWeek = reply[6];
	version.fwBuildYear = reply[7];
	version.fwPatchNumber = reply[8];
	return true;
}


bool
ParseBootParams(const uint8_t* reply, size_t length, BootParams& params)
{
	// struct intel_boot_params without its leading status byte
	if (length != 22)
		return false;
	params.otpFormat = reply[0];
	params.otpContent = reply[1];
	params.otpPatch = reply[2];
	params.deviceRevision = le16(reply + 3);
	params.secureBoot = reply[5];
	params.keyFromHeader = reply[6];
	params.keyType = reply[7];
	params.otpLock = reply[8];
	params.apiLock = reply[9];
	params.debugLock = reply[10];
	memcpy(params.otpAddress, reply + 11, 6);
	params.minimumBuildNumber = reply[17];
	params.minimumBuildWeek = reply[18];
	params.minimumBuildYear = reply[19];
	params.limitedCommandComplete = reply[20];
	params.unlockedState = reply[21];
	return true;
}


bool
ParseTlvVersion(const uint8_t* reply, size_t length, TlvVersion& version)
{
	memset(&version, 0, sizeof(version));
	for (size_t offset = 0; offset < length;) {
		if (offset + 2 > length)
			return false;
		const uint8_t type = reply[offset];
		const uint8_t size = reply[offset + 1];
		const uint8_t* value = reply + offset + 2;
		if (offset + 2 + size > length)
			return false;

		switch (type) {
			case 0x10:
				if (size >= 4) version.cnviTop = le32(value);
				break;
			case 0x11:
				if (size >= 4) version.cnvrTop = le32(value);
				break;
			case 0x12:
				if (size >= 4) version.cnviBt = le32(value);
				break;
			case 0x13:
				if (size >= 4) version.cnvrBt = le32(value);
				break;
			case 0x16:
				if (size >= 2) version.deviceRevision = le16(value);
				break;
			case 0x1c:
				if (size >= 1) version.imageType = value[0];
				break;
			case 0x1d:
				if (size >= 2) {
					version.minimumBuildWeek = value[0];
					version.minimumBuildYear = value[1];
					version.timestamp = le16(value);
				}
				break;
			case 0x1e:
				if (size >= 1) version.buildType = value[0];
				break;
			case 0x1f:
				if (size >= 4) {
					version.minimumBuildNumber = value[0];
					version.buildNumber = le32(value);
				}
				break;
			case 0x28:
				if (size >= 1) version.secureBoot = value[0];
				break;
			case 0x2a:
				if (size >= 1) version.otpLock = value[0];
				break;
			case 0x2b:
				if (size >= 1) version.apiLock = value[0];
				break;
			case 0x2c:
				if (size >= 1) version.debugLock = value[0];
				break;
			case 0x2d:
				if (size >= 3) {
					version.minimumBuildNumber = value[0];
					version.minimumBuildWeek = value[1];
					version.minimumBuildYear = value[2];
				}
				break;
			case 0x2e:
				if (size >= 1) version.limitedCommandComplete = value[0];
				break;
			case 0x2f:
				if (size >= 1) version.secureBootEngine = value[0];
				break;
			case 0x30:
				if (size >= 6) memcpy(version.otpAddress, value, 6);
				break;
			case 0x32:
				if (size >= 4) version.gitSha1 = le32(value);
				break;
			case 0x50:
			{
				// Linux copies it with snprintf("%s"): stop at a NUL.
				size_t idLength = 0;
				while (idLength < size && idLength < kFirmwareIdLength - 1
					&& value[idLength] != '\0') {
					idLength++;
				}
				memcpy(version.firmwareId, value, idLength);
				version.firmwareId[idLength] = '\0';
				break;
			}
		}
		offset += 2 + size;
	}
	return true;
}


Generation
LegacyGeneration(uint8_t hwVariant)
{
	switch (hwVariant) {
		case 0x07:
		case 0x08:
			return kGenerationLegacyRom;
		case 0x0b:
		case 0x0c:
		case 0x11:
		case 0x12:
		case 0x13:
		case 0x14:
			return kGenerationLegacyBootloader;
		default:
			return kGenerationUnknown;
	}
}


Generation
TlvGeneration(uint8_t hwVariant)
{
	switch (hwVariant) {
		// The operational firmware of these legacy bootloader parts answers
		// in TLV form, but their file names come from the legacy version.
		case 0x11:
		case 0x12:
		case 0x13:
		case 0x14:
			return kGenerationLegacyBootloader;
		case 0x17:
		case 0x18:
		case 0x19:
		case 0x1b:
		case 0x1c:
		case 0x1d:
		case 0x1e:
		case 0x1f:
		case 0x22:
			return kGenerationTlv;
		default:
			return kGenerationUnknown;
	}
}


bool
LegacyFirmwareName(const LegacyVersion& version, uint16_t deviceRevision,
	const char* suffix, char* name, size_t size)
{
	switch (version.hwVariant) {
		case 0x0b:
		case 0x0c:
			snprintf(name, size, "ibt-%u-%u.%s", version.hwVariant,
				deviceRevision, suffix);
			return true;
		case 0x11:
		case 0x12:
		case 0x13:
		case 0x14:
			snprintf(name, size, "ibt-%u-%u-%u.%s", version.hwVariant,
				version.hwRevision, version.fwRevision, suffix);
			return true;
		default:
			return false;
	}
}


void
TlvFirmwareName(const TlvVersion& version, const char* suffix, char* name,
	size_t size)
{
	const unsigned cnvi = PackTop(version.cnviTop);
	const unsigned cnvr = PackTop(version.cnvrTop);

	// Only Blazar and later load an intermediate loader image first, and
	// name the operational image after the transport ("usb" or "pci").
	if (HardwareVariant(version.cnviBt) >= 0x1e) {
		if (version.imageType == kImageBootloader) {
			snprintf(name, size, "ibt-%04x-%04x-iml.%s", cnvi, cnvr, suffix);
			return;
		}
		if (version.firmwareId[0] != '\0') {
			snprintf(name, size, "ibt-%04x-%04x-%s.%s", cnvi, cnvr,
				version.firmwareId, suffix);
			return;
		}
	}
	snprintf(name, size, "ibt-%04x-%04x.%s", cnvi, cnvr, suffix);
}


void
RomPatchNames(const LegacyVersion& version, char* specific,
	size_t specificSize, char* fallback, size_t fallbackSize)
{
	snprintf(specific, specificSize, "ibt-hw-%x.%x.%x-fw-%x.%x.%x.%x.%x.bseq",
		version.hwPlatform, version.hwVariant, version.hwRevision,
		version.fwVariant, version.fwRevision, version.fwBuildNumber,
		version.fwBuildWeek, version.fwBuildYear);
	snprintf(fallback, fallbackSize, "ibt-hw-%x.%x.bseq", version.hwPlatform,
		version.hwVariant);
}


bool
InspectSfi(const uint8_t* data, size_t size, bool tlv, uint8_t hwVariant,
	uint8_t secureBootEngine, SfiLayout& layout, const char** error)
{
	memset(&layout, 0, sizeof(layout));
	*error = NULL;

	if (size < kRsaHeaderLength) {
		*error = "file is shorter than the RSA header";
		return false;
	}
	if (le32(data + kCssVersionOffset) != 0x00010000) {
		*error = "invalid RSA CSS header version";
		return false;
	}

	layout.payloadOffset = kRsaHeaderLength;
	if (tlv && hwVariant >= 0x17) {
		// Both headers are present; the engine selects which one is sent.
		if (size < kRsaHeaderLength + kEcdsaHeaderLength
			|| data[kRsaHeaderLength] != 0x06
			|| le32(data + kRsaHeaderLength + kCssVersionOffset)
				!= 0x00020000) {
			*error = "invalid ECDSA CSS header";
			return false;
		}
		if (secureBootEngine > 1) {
			*error = "unknown secure boot engine";
			return false;
		}
		layout.ecdsa = secureBootEngine == 1;
		layout.payloadOffset = kRsaHeaderLength + kEcdsaHeaderLength;
	} else if (tlv && secureBootEngine != 0) {
		*error = "RSA-only controller reports a non-RSA secure boot engine";
		return false;
	}

	bool bootFound = false;
	for (size_t offset = layout.payloadOffset; offset < size;) {
		if (offset + 3 > size || offset + 3 + data[offset + 2] > size) {
			*error = "truncated command in the firmware payload";
			return false;
		}
		const uint16_t opcode = le16(data + offset);
		const uint8_t length = data[offset + 2];
		if (opcode == kOpcodeWriteBootParams && length >= 7 && !bootFound) {
			layout.bootAddress = le32(data + offset + 3);
			layout.buildNumber = data[offset + 7];
			layout.buildWeek = data[offset + 8];
			layout.buildYear = data[offset + 9];
			bootFound = true;
		}
		offset += 3 + length;
	}

	size_t offset = layout.payloadOffset;
	size_t length;
	while (NextSfiFragment(data, size, offset, length))
		layout.fragments++;
	if (offset != size) {
		*error = "payload does not end on a 4 byte fragment boundary";
		return false;
	}
	if (!bootFound) {
		*error = "no Write Boot Params command in the payload";
		return false;
	}
	return true;
}


bool
NextSfiFragment(const uint8_t* data, size_t size, size_t& offset,
	size_t& length)
{
	length = 0;
	while (offset + length < size) {
		const size_t command = offset + length;
		if (command + 3 > size || command + 3 + data[command + 2] > size)
			return false;
		length += 3 + data[command + 2];
		if (length % 4 == 0) {
			offset += length;
			return true;
		}
	}
	return false;
}


bool
NextDdcRecord(const uint8_t* data, size_t size, size_t& offset,
	size_t& length)
{
	if (offset >= size)
		return false;
	length = (size_t)data[offset] + 1;
	if (length < 3 || offset + length > size)
		return false;
	return true;
}


bool
NextPatchStep(const uint8_t* data, size_t size, size_t& offset,
	PatchStep& step)
{
	memset(&step, 0, sizeof(step));
	if (offset + 4 > size || data[offset] != 0x01)
		return false;
	step.opcode = le16(data + offset + 1);
	step.parameterLength = data[offset + 3];
	step.parameters = data + offset + 4;
	size_t next = offset + 4 + step.parameterLength;
	if (next > size)
		return false;

	bool haveEvent = false;
	while (next + 3 <= size && data[next] == 0x02) {
		const uint8_t length = data[next + 2];
		if (next + 3 + length > size)
			return false;
		step.eventCode = data[next + 1];
		step.eventLength = length;
		step.eventParameters = data + next + 3;
		haveEvent = true;
		next += 3 + length;
	}
	if (!haveEvent)
		return false;
	offset = next;
	return true;
}


}	// namespace IntelBluetooth
