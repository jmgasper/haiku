/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Realtek USB controllers (RTL8723/8761/8821/8822/8851/8852/8922 families),
 * following Linux btrtl.c (6.18) btrtl_initialize() and
 * btrtl_download_firmware(). Every command is a control transfer and every
 * answer arrives on the interrupt endpoint.
 */


#include "BluetoothFirmware.h"

#include <string.h>
#include <unistd.h>

#include "RealtekBluetoothFirmware.h"


using namespace RealtekBluetooth;


static bool
ReadRegister16(Controller& controller, const uint8_t (&parameters)[5],
	uint16_t& value)
{
	uint8_t reply[8];
	size_t length = sizeof(reply);
	if (!controller.Command(kOpcodeReadRegister16, parameters,
			sizeof(parameters), reply, &length, kInitTimeout)
		|| length != 2) {
		return false;
	}
	value = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	return true;
}


struct LocalVersion {
	uint8_t		hciVersion;
	uint16_t	hciRevision;
	uint8_t		lmpVersion;
	uint16_t	lmpSubversion;
};


static bool
ReadLocalVersion(Controller& controller, LocalVersion& version)
{
	uint8_t reply[16];
	size_t length = sizeof(reply);
	if (!controller.Command(kOpcodeReadLocalVersion, NULL, 0, reply, &length,
			kInitTimeout)
		|| length != 8) {
		ERROR("%s: Read Local Version failed\n", controller.Path());
		return false;
	}
	version.hciVersion = reply[0];
	version.hciRevision = (uint16_t)reply[1] | ((uint16_t)reply[2] << 8);
	version.lmpVersion = reply[3];
	version.lmpSubversion = (uint16_t)reply[6] | ((uint16_t)reply[7] << 8);
	return true;
}


static bool
LoadFile(const char* base, const char* suffix, std::vector<uint8_t>& data,
	bool required, std::string& name)
{
	name = std::string(base) + suffix;
	return LoadFirmware("rtl_bt", name.c_str(), data, required);
}


bool
SetupRealtek(Controller& controller, bool infoOnly)
{
	const IcInfo* ic = NULL;
	LocalVersion version;
	bool dropped = false;

	while (true) {
		uint16_t subversion;
		if (!ReadRegister16(controller, kRegisterChipSubversion,
				subversion)) {
			ERROR("%s: cannot read the chip subversion\n", controller.Path());
			return false;
		}

		bool haveVersion = false;
		if (subversion == kLmp8822B) {
			uint16_t revision;
			if (!ReadRegister16(controller, kRegisterChipRevision,
					revision)) {
				return false;
			}
			// The RTL8822E is identified from its registers alone.
			if (revision == 0x000e) {
				version.hciVersion = 0x0c;
				version.hciRevision = revision;
				version.lmpVersion = 0x0c;
				version.lmpSubversion = subversion;
				haveVersion = true;
			}
		}
		if (!haveVersion && !ReadLocalVersion(controller, version))
			return false;

		ic = MatchIc(version.lmpSubversion, version.hciRevision,
			version.hciVersion);
		LOG("  HCI version 0x%02x revision 0x%04x, LMP version 0x%02x "
			"subversion 0x%04x: %s\n", version.hciVersion,
			version.hciRevision, version.lmpVersion, version.lmpSubversion,
			ic != NULL ? ic->name : "no ROM match");

		// A controller already running a patch reports that patch's
		// version. Linux drops the patch (0xfc66) and looks again.
		if (ic != NULL || dropped || infoOnly)
			break;
		LOG("  dropping the running patch to identify the ROM\n");
		if (!controller.Post(kOpcodeDropFirmware, NULL, 0))
			return false;
		usleep(200000);
		dropped = true;
	}

	if (ic == NULL) {
		LOG("  unknown or already patched controller, no firmware "
			"download\n");
		return true;
	}

	uint8_t romVersion = 0;
	if (ic->hasRomVersion) {
		uint8_t reply[4];
		size_t length = sizeof(reply);
		if (!controller.Command(kOpcodeReadRomVersion, NULL, 0, reply,
				&length, kInitTimeout)
			|| length != 1) {
			ERROR("%s: cannot read the ROM version\n", controller.Path());
			return false;
		}
		romVersion = reply[0];
	}

	uint16_t project;
	if (!ReadRegister16(controller, kRegisterSecurityProject, project)) {
		ERROR("%s: cannot read the security key\n", controller.Path());
		return false;
	}
	const uint8_t keyId = project & 0xff;
	LOG("  ROM version %u, key id %u\n", romVersion, keyId);

	std::string firmwareName;
	std::string path;
	if (ic->lmpSubversion == kLmp8852A && version.hciRevision == 0x000c
		&& FindFirmware("rtl_bt", (std::string(ic->firmware) + "_v2.bin")
			.c_str(), path)) {
		firmwareName = std::string(ic->firmware) + "_v2.bin";
	} else
		firmwareName = std::string(ic->firmware) + ".bin";
	std::string configName = ic->config != NULL
		? std::string(ic->config) + ".bin" : std::string();
	LOG("  firmware %s: %s\n", firmwareName.c_str(),
		FindFirmware("rtl_bt", firmwareName.c_str(), path)
			? path.c_str() : "not installed");
	if (!configName.empty()) {
		LOG("  configuration %s: %s\n", configName.c_str(),
			FindFirmware("rtl_bt", configName.c_str(), path)
				? path.c_str() : "not installed");
	}
	if (infoOnly)
		return true;

	std::vector<uint8_t> firmware;
	if (!LoadFirmware("rtl_bt", firmwareName.c_str(), firmware, true))
		return false;
	// Parts with a security key take no configuration file.
	std::vector<uint8_t> config;
	if (!configName.empty() && keyId == 0) {
		std::string name;
		if (!LoadFile(ic->config, ".bin", config, ic->configNeeded, name)
			&& ic->configNeeded) {
			return false;
		}
	}

	std::vector<uint8_t> patch;
	int projectId;
	const char* error;
	if (!BuildPatch(*ic, romVersion, keyId, firmware.data(), firmware.size(),
			config.data(), config.size(), patch, projectId, &error)) {
		ERROR("%s: %s: %s\n", controller.Path(), firmwareName.c_str(), error);
		return false;
	}
	LOG("  project %d, patch %zu bytes (configuration %zu), %zu fragments\n",
		projectId, patch.size(), config.size(), CountFragments(patch.size()));

	for (size_t fragment = 0; fragment < CountFragments(patch.size());
			fragment++) {
		uint8_t parameters[1 + kFragmentLength];
		uint8_t index;
		size_t size;
		FragmentAt(patch.size(), fragment, index, size);
		parameters[0] = index;
		memcpy(parameters + 1, patch.data() + fragment * kFragmentLength,
			size);
		uint8_t reply[4];
		size_t length = sizeof(reply);
		if (!controller.Command(kOpcodeDownload, parameters, size + 1, reply,
				&length, kInitTimeout)
			|| length != 1) {
			ERROR("%s: download fragment %zu failed\n", controller.Path(),
				fragment);
			return false;
		}
	}

	if (!ReadLocalVersion(controller, version))
		return false;
	LOG("  firmware version 0x%04x%04x running\n", version.hciRevision,
		version.lmpSubversion);
	return true;
}
