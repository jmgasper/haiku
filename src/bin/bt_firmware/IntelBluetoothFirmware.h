/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Transport independent parts of the Intel Bluetooth firmware setup: device
 * identification, version parsing, firmware file naming and the layout of
 * the firmware files. The sequence follows Linux btintel.c (6.18), so the
 * same linux-firmware files are selected for the same controller.
 *
 * This file has no Haiku dependencies so that it can be tested on the build
 * host against real firmware files.
 */
#ifndef INTEL_BLUETOOTH_FIRMWARE_H
#define INTEL_BLUETOOTH_FIRMWARE_H


#include <stddef.h>
#include <stdint.h>


namespace IntelBluetooth {


static const uint16_t kVendorIntel = 0x8087;

static const uint16_t kOpcodeReset = 0x0c03;
static const uint16_t kOpcodeIntelReset = 0xfc01;
static const uint16_t kOpcodeReadVersion = 0xfc05;
static const uint16_t kOpcodeSecureSend = 0xfc09;
static const uint16_t kOpcodeReadBootParams = 0xfc0d;
static const uint16_t kOpcodeWriteBootParams = 0xfc0e;
static const uint16_t kOpcodeManufacturer = 0xfc11;
static const uint16_t kOpcodeWriteDdc = 0xfc8b;
static const uint16_t kOpcodePatch = 0xfc8e;

static const size_t kRsaHeaderLength = 644;
static const size_t kEcdsaHeaderLength = 320;
static const size_t kCssVersionOffset = 8;

static const uint8_t kImageBootloader = 0x01;
static const uint8_t kImageIntermediate = 0x02;
static const uint8_t kImageOperational = 0x03;

static const uint8_t kLegacyFirmwareBootloader = 0x06;
static const uint8_t kLegacyFirmwareOperational = 0x23;


enum Generation {
	kGenerationUnknown = 0,
	// Wireless 7260/7265/3160/3165/3168: ROM firmware, optional .bseq patch.
	kGenerationLegacyRom,
	// 8260/8265/9260/9560/AX200/AX201: RSA secured bootloader,
	// ibt-<variant>-<revision>[-<fw revision>].sfi
	kGenerationLegacyBootloader,
	// AX210 and newer: TLV version information, RSA or ECDSA secured
	// bootloader, ibt-<cnvi>-<cnvr>[-<id>].sfi
	kGenerationTlv
};


struct UsbDevice {
	uint16_t	product;
	const char*	name;
	// The controller may be left in SW_RFKILL or with a broken initial
	// command credit; Linux sends HCI_Reset before anything else.
	bool		needsInitialReset;
};


struct LegacyVersion {
	uint8_t		hwPlatform;
	uint8_t		hwVariant;
	uint8_t		hwRevision;
	uint8_t		fwVariant;
	uint8_t		fwRevision;
	uint8_t		fwBuildNumber;
	uint8_t		fwBuildWeek;
	uint8_t		fwBuildYear;
	uint8_t		fwPatchNumber;
};


struct BootParams {
	uint8_t		otpFormat;
	uint8_t		otpContent;
	uint8_t		otpPatch;
	uint16_t	deviceRevision;
	uint8_t		secureBoot;
	uint8_t		keyFromHeader;
	uint8_t		keyType;
	uint8_t		otpLock;
	uint8_t		apiLock;
	uint8_t		debugLock;
	uint8_t		otpAddress[6];
	uint8_t		minimumBuildNumber;
	uint8_t		minimumBuildWeek;
	uint8_t		minimumBuildYear;
	uint8_t		limitedCommandComplete;
	uint8_t		unlockedState;
};


static const size_t kFirmwareIdLength = 8;

struct TlvVersion {
	uint32_t	cnviTop;
	uint32_t	cnvrTop;
	uint32_t	cnviBt;
	uint32_t	cnvrBt;
	uint16_t	deviceRevision;
	uint8_t		imageType;
	uint16_t	timestamp;
	uint8_t		buildType;
	uint32_t	buildNumber;
	uint8_t		secureBoot;
	uint8_t		otpLock;
	uint8_t		apiLock;
	uint8_t		debugLock;
	uint8_t		minimumBuildNumber;
	uint8_t		minimumBuildWeek;
	uint8_t		minimumBuildYear;
	uint8_t		limitedCommandComplete;
	uint8_t		secureBootEngine;
	uint32_t	gitSha1;
	char		firmwareId[kFirmwareIdLength];
	uint8_t		otpAddress[6];
};


struct SfiLayout {
	bool		ecdsa;
	size_t		payloadOffset;
	uint32_t	bootAddress;
	uint8_t		buildNumber;
	uint8_t		buildWeek;
	uint8_t		buildYear;
	size_t		fragments;
};


struct PatchStep {
	uint16_t		opcode;
	const uint8_t*	parameters;
	uint8_t			parameterLength;
	uint8_t			eventCode;
	const uint8_t*	eventParameters;
	uint8_t			eventLength;
};


// Returns NULL for USB devices that are not Intel Bluetooth controllers.
const UsbDevice* FindUsbDevice(uint16_t vendor, uint16_t product);
size_t CountUsbDevices();
const UsbDevice* UsbDeviceAt(size_t index);

inline uint8_t
HardwarePlatform(uint32_t cnvxBt)
{
	return (cnvxBt & 0x0000ff00) >> 8;
}

inline uint8_t
HardwareVariant(uint32_t cnvxBt)
{
	return (cnvxBt & 0x003f0000) >> 16;
}

const char* VariantName(uint8_t hwVariant);

// "reply" is the Command Complete return parameters after the status byte.
bool IsLegacyVersionReply(const uint8_t* reply, size_t length);
bool ParseLegacyVersion(const uint8_t* reply, size_t length,
	LegacyVersion& version);
bool ParseBootParams(const uint8_t* reply, size_t length, BootParams& params);
bool ParseTlvVersion(const uint8_t* reply, size_t length,
	TlvVersion& version);

Generation LegacyGeneration(uint8_t hwVariant);
Generation TlvGeneration(uint8_t hwVariant);

// File names are relative to the "intel" firmware directory.
bool LegacyFirmwareName(const LegacyVersion& version, uint16_t deviceRevision,
	const char* suffix, char* name, size_t size);
void TlvFirmwareName(const TlvVersion& version, const char* suffix,
	char* name, size_t size);
void RomPatchNames(const LegacyVersion& version, char* specific,
	size_t specificSize, char* fallback, size_t fallbackSize);

// Validates the headers of an .sfi file for the controller and finds the
// boot address and firmware build in its command stream.
bool InspectSfi(const uint8_t* data, size_t size, bool tlv,
	uint8_t hwVariant, uint8_t secureBootEngine, SfiLayout& layout,
	const char** error);

// Secure Send data fragments are runs of whole HCI commands whose total
// length is a multiple of four; the files carry NOPs for this purpose.
bool NextSfiFragment(const uint8_t* data, size_t size, size_t& offset,
	size_t& length);

// DDC files are a list of records: length, 16 bit id, value.
bool NextDdcRecord(const uint8_t* data, size_t size, size_t& offset,
	size_t& length);

// .bseq files alternate 0x01 <command> and one or more 0x02 <event>; the
// last event is the one the controller must answer with.
bool NextPatchStep(const uint8_t* data, size_t size, size_t& offset,
	PatchStep& step);


}	// namespace IntelBluetooth


#endif	// INTEL_BLUETOOTH_FIRMWARE_H
