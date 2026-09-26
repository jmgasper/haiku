/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Transport independent parts of the Realtek Bluetooth firmware setup,
 * following Linux btrtl.c (6.18) for USB controllers: chip identification,
 * file naming and the construction of the patch that is downloaded. Like
 * IntelBluetoothFirmware.h it has no Haiku dependencies, for host tests.
 */
#ifndef REALTEK_BLUETOOTH_FIRMWARE_H
#define REALTEK_BLUETOOTH_FIRMWARE_H


#include <stddef.h>
#include <stdint.h>

#include <vector>


namespace RealtekBluetooth {


static const uint16_t kVendorRealtek = 0x0bda;

static const uint16_t kOpcodeReadLocalVersion = 0x1001;
static const uint16_t kOpcodeDownload = 0xfc20;
static const uint16_t kOpcodeReadRegister16 = 0xfc61;
static const uint16_t kOpcodeDropFirmware = 0xfc66;
static const uint16_t kOpcodeReadRomVersion = 0xfc6d;

static const size_t kFragmentLength = 252;

static const uint16_t kLmp8723A = 0x1200;
static const uint16_t kLmp8723B = 0x8723;
static const uint16_t kLmp8821A = 0x8821;
static const uint16_t kLmp8761A = 0x8761;
static const uint16_t kLmp8822B = 0x8822;
static const uint16_t kLmp8852A = 0x8852;
static const uint16_t kLmp8851B = 0x8851;
static const uint16_t kLmp8922A = 0x8922;

// Parameters of the vendor register reads (btrtl.c RTL_CHIP_SUBVER etc.)
extern const uint8_t kRegisterChipSubversion[5];
extern const uint8_t kRegisterChipRevision[5];
extern const uint8_t kRegisterSecurityProject[5];


struct UsbId {
	uint16_t	vendor;
	uint16_t	product;
};


struct IcInfo {
	uint16_t	lmpSubversion;
	uint16_t	hciRevision;
	uint8_t		hciVersion;
	bool		configNeeded;
	bool		hasRomVersion;
	// Relative to the "rtl_bt" firmware directory, without ".bin".
	const char*	firmware;
	const char*	config;
	const char*	name;
};


// Modules listed with BTUSB_REALTEK in Linux btusb.c. Realtek's own vendor
// ID is matched by the Bluetooth interface class instead.
bool IsListedUsbDevice(uint16_t vendor, uint16_t product);
size_t CountListedUsbDevices();
const UsbId* ListedUsbDeviceAt(size_t index);

// USB controllers only (btrtl.c ic_id_table entries with HCI_USB).
const IcInfo* MatchIc(uint16_t lmpSubversion, uint16_t hciRevision,
	uint8_t hciVersion);
size_t CountIcs();
const IcInfo* IcAt(size_t index);

// Builds what is downloaded with Realtek's 0xfc20 command: the patch for
// this ROM version (epatch v1 "Realtech" or v2 "RTBTCore"), followed by
// the configuration file if there is one. Returns false with a reason.
bool BuildPatch(const IcInfo& ic, uint8_t romVersion, uint8_t keyId,
	const uint8_t* firmware, size_t firmwareSize, const uint8_t* config,
	size_t configSize, std::vector<uint8_t>& patch, int& projectId,
	const char** error);

// Download fragments: fragment "fragment" of a patch of "length" bytes
// starts at offset fragment * kFragmentLength, is "size" bytes long and
// carries "index" (bit 7 marks the last one).
size_t CountFragments(size_t length);
void FragmentAt(size_t length, size_t fragment, uint8_t& index, size_t& size);


}	// namespace RealtekBluetooth


#endif	// REALTEK_BLUETOOTH_FIRMWARE_H
