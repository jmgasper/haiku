/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_FIRMWARE_LOADER_H
#define AMDGPU_FIRMWARE_LOADER_H

#include "Firmware.h"
#include <SupportDefs.h>

class InstalledFirmware {
public:
	InstalledFirmware();
	~InstalledFirmware();
	status_t Load(const char* name, bool smc);
	amdgpu::FirmwareView view;
private:
	void* fData;
	InstalledFirmware(const InstalledFirmware&) = delete;
	InstalledFirmware& operator=(const InstalledFirmware&) = delete;
};
#endif
