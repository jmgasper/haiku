/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <LEDeviceStatus.h>

#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <OS.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


namespace Bluetooth {

static bigtime_t
BootTime()
{
	system_info info;
	if (get_system_info(&info) != B_OK)
		return 0;
	return info.boot_time;
}


status_t
LEDeviceStatusPath(char* path, size_t capacity)
{
	char settings[B_PATH_NAME_LENGTH];
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, -1, false,
		settings, sizeof(settings));
	if (status != B_OK)
		return status;
	int length = snprintf(path, capacity, "%s/bluetooth/le_status", settings);
	return length > 0 && (size_t)length < capacity ? B_OK : B_NAME_TOO_LONG;
}


static status_t
ReadArchive(BMessage& archive)
{
	char path[B_PATH_NAME_LENGTH];
	status_t status = LEDeviceStatusPath(path, sizeof(path));
	if (status != B_OK)
		return status;
	BFile file(path, B_READ_ONLY);
	status = file.InitCheck();
	if (status == B_OK)
		status = archive.Unflatten(&file);
	if (status == B_OK && archive.GetInt64("boot_time", 0) != BootTime())
		return B_ENTRY_NOT_FOUND;
	return status;
}


status_t
ReadLEDeviceStatus(std::vector<LEDeviceStatus>& devices)
{
	devices.clear();
	BMessage archive;
	status_t status = ReadArchive(archive);
	if (status != B_OK)
		return status == B_ENTRY_NOT_FOUND ? B_OK : status;

	BMessage device;
	for (int32 i = 0; archive.FindMessage("device", i, &device) == B_OK;
			i++) {
		const void* address;
		const void* local;
		ssize_t size, localSize;
		if (device.FindData("address", B_RAW_TYPE, &address, &size) != B_OK
			|| size != 6
			|| device.FindData("local", B_RAW_TYPE, &local, &localSize) != B_OK
			|| localSize != 6)
			continue;
		LEDeviceStatus entry;
		memcpy(entry.address, address, 6);
		memcpy(entry.localAddress, local, 6);
		entry.addressType = device.GetUInt8("address_type", 0);
		entry.connected = device.GetBool("connected", false);
		entry.battery = device.GetInt32("battery", -1);
		entry.updated = device.GetInt64("updated", 0);
		devices.push_back(entry);
	}
	return B_OK;
}


status_t
PublishLEDeviceStatus(const LEDeviceStatus& status)
{
	std::vector<LEDeviceStatus> devices;
	ReadLEDeviceStatus(devices);
	bool found = false;
	for (size_t i = 0; i < devices.size(); i++) {
		if (memcmp(devices[i].address, status.address, 6) == 0
			&& memcmp(devices[i].localAddress, status.localAddress, 6) == 0) {
			devices[i] = status;
			found = true;
		}
	}
	if (!found)
		devices.push_back(status);

	BMessage archive;
	archive.AddInt64("boot_time", BootTime());
	for (size_t i = 0; i < devices.size(); i++) {
		BMessage device;
		device.AddData("address", B_RAW_TYPE, devices[i].address, 6);
		device.AddData("local", B_RAW_TYPE, devices[i].localAddress, 6);
		device.AddUInt8("address_type", devices[i].addressType);
		device.AddBool("connected", devices[i].connected);
		device.AddInt32("battery", devices[i].battery);
		device.AddInt64("updated", devices[i].updated);
		archive.AddMessage("device", &device);
	}

	char path[B_PATH_NAME_LENGTH];
	status_t result = LEDeviceStatusPath(path, sizeof(path));
	if (result != B_OK)
		return result;
	char directory[B_PATH_NAME_LENGTH];
	strlcpy(directory, path, sizeof(directory));
	char* slash = strrchr(directory, '/');
	if (slash != NULL) {
		*slash = '\0';
		mkdir(directory, 0755);
	}
	char temporary[B_PATH_NAME_LENGTH + 8];
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	{
		BFile file(temporary, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		result = file.InitCheck();
		if (result == B_OK)
			result = archive.Flatten(&file);
		if (result == B_OK)
			result = file.Sync();
	}
	if (result == B_OK && rename(temporary, path) != 0)
		result = errno;
	if (result != B_OK)
		unlink(temporary);
	return result;
}

} // namespace Bluetooth
