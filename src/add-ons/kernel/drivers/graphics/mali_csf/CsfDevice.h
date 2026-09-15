/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_DEVICE_H
#define MALI_CSF_DEVICE_H

bool FirmwareMemoryRetained();
status_t RunFirmwareRequest(const MaliCSF::ResourceInfo& resources, void* buffer,
	size_t length, bool& needsRecovery);
status_t RunCommandRequest(const MaliCSF::ResourceInfo& resources, void* buffer,
	size_t length, bool& needsRecovery, bool shader = false);

#endif
