/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "FirmwareLoader.h"
#include <KernelExport.h>
#include <FindDirectory.h>
#include <StorageDefs.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

InstalledFirmware::InstalledFirmware()
	: view(), fData(NULL)
{
}

InstalledFirmware::~InstalledFirmware()
{
	free(fData);
}

status_t
InstalledFirmware::Load(const char* name, bool smc)
{
	// Only fixed driver filenames are accepted; never search user directories.
	if (fData != NULL || name == NULL || strchr(name, '/') != NULL)
		return B_BAD_VALUE;
	const directory_which places[] = {B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY};
	int fd = -1;
	char path[B_PATH_NAME_LENGTH];
	for (directory_which place : places) {
		status_t status = find_directory(place, -1, false, path, sizeof(path));
		if (status != B_OK)
			return status;
		if (strlcat(path, "/firmware/amdgpu/", sizeof(path)) >= sizeof(path)
			|| strlcat(path, name, sizeof(path)) >= sizeof(path))
			return B_NAME_TOO_LONG;
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0)
			break;
		if (errno != B_ENTRY_NOT_FOUND)
			return errno;
	}
	if (fd < 0)
		return B_ENTRY_NOT_FOUND;
	struct stat info;
	status_t status = fstat(fd, &info) == 0 ? B_OK : errno;
	if (status == B_OK && (!S_ISREG(info.st_mode) || info.st_size < 36
		|| info.st_size > (smc ? 0x20100 : 65536)))
		status = B_BAD_DATA;
	if (status == B_OK) {
		fData = malloc(info.st_size);
		if (fData == NULL)
			status = B_NO_MEMORY;
	}
	if (status == B_OK) {
		ssize_t bytes = read(fd, fData, info.st_size);
		if (bytes != info.st_size)
			status = bytes < 0 ? errno : B_IO_ERROR;
	}
	close(fd);
	if (status == B_OK && !(smc
		? amdgpu::ParseSmcFirmware(fData, info.st_size, view)
		: amdgpu::ParseSdmaFirmware(fData, info.st_size, view)))
		status = B_BAD_DATA;
	if (status == B_OK)
		dprintf("amdgpu: installed firmware %s version %#x feature %u\n",
			path, (unsigned)view.version, (unsigned)view.featureVersion);
	return status;
}
