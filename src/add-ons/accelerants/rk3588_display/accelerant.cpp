/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <AutoDeleterOS.h>


using namespace RK3588Display;

accelerant_info* gInfo;


// Shared by the primary accelerant and its clones: the driver's description
// of the acquired frame buffer and the shared information area.
static status_t
init_common(int device, bool isClone)
{
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	MemoryDeleter infoDeleter(gInfo);
	if (gInfo == NULL)
		return B_NO_MEMORY;
	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->device = device;
	gInfo->is_clone = isClone;
	gInfo->shared_info_area = -1;
	gInfo->mode_list_area = -1;
	gInfo->frame_buffer_area = -1;

	gInfo->info.version = kAccelerantVersion;
	if (ioctl(device, kGetAccelerantInfo, &gInfo->info, sizeof(gInfo->info)) != 0)
		return errno != 0 ? errno : B_ERROR;
	if ((gInfo->info.flags & kAccelerantAcquired) == 0)
		return B_NO_INIT;

	AreaDeleter sharedDeleter(clone_area("rk3588 display shared info",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
		gInfo->info.sharedArea));
	status_t status = gInfo->shared_info_area = sharedDeleter.Get();
	if (status < B_OK)
		return status;
	if (gInfo->shared_info->version != kAccelerantVersion
		|| gInfo->shared_info->width != gInfo->info.width
		|| gInfo->shared_info->height != gInfo->info.height
		|| gInfo->shared_info->bytesPerRow != gInfo->info.bytesPerRow) {
		return B_MISMATCHED_VALUES;
	}

	infoDeleter.Detach();
	sharedDeleter.Detach();
	return B_OK;
}


static void
uninit_common(void)
{
	if (gInfo->frame_buffer_area >= 0)
		delete_area(gInfo->frame_buffer_area);
	delete_area(gInfo->shared_info_area);
	gInfo->shared_info_area = -1;
	gInfo->shared_info = NULL;
	if (gInfo->is_clone)
		close(gInfo->device);
	free(gInfo);
	gInfo = NULL;
}


// The primary accelerant acquires the frame buffer: the driver swaps the live
// window to its own buffer, which app_server then maps through a clone.
status_t
rk3588_init_accelerant(int device)
{
	if (ioctl(device, kAcquireFrameBuffer, NULL, 0) != 0)
		return errno != 0 ? errno : B_ERROR;
	status_t status = init_common(device, false);
	if (status != B_OK)
		return status;
	status = create_mode_list();
	if (status != B_OK) {
		uninit_common();
		return status;
	}
	area_info info;
	if (ioctl(device, kCloneFrameBuffer, &info, sizeof(info)) != 0) {
		status = errno != 0 ? errno : B_ERROR;
		delete_area(gInfo->mode_list_area);
		uninit_common();
		return status;
	}
	gInfo->frame_buffer_area = info.area;
	gInfo->frame_buffer = info.address;
	return B_OK;
}


ssize_t
rk3588_accelerant_clone_info_size(void)
{
	return B_PATH_NAME_LENGTH;
}


void
rk3588_get_accelerant_clone_info(void* info)
{
	ioctl(gInfo->device, kGetDeviceName, info, B_PATH_NAME_LENGTH);
}


status_t
rk3588_clone_accelerant(void* info)
{
	char path[B_PATH_NAME_LENGTH + 8];
	strcpy(path, "/dev/");
	strlcat(path, (const char*)info, sizeof(path));
	int fd = open(path, B_READ_WRITE);
	if (fd < 0)
		return errno;
	status_t status = init_common(fd, true);
	if (status != B_OK) {
		close(fd);
		return status;
	}
	status = gInfo->mode_list_area = clone_area("rk3588 display cloned modes",
		(void**)&gInfo->mode_list, B_ANY_ADDRESS, B_READ_AREA,
		gInfo->shared_info->modeListArea);
	if (status < B_OK) {
		uninit_common();
		return status;
	}
	return B_OK;
}


void
rk3588_uninit_accelerant(void)
{
	delete_area(gInfo->mode_list_area);
	gInfo->mode_list = NULL;
	uninit_common();
}


status_t
rk3588_get_accelerant_device_info(accelerant_device_info* info)
{
	info->version = B_ACCELERANT_VERSION;
	strlcpy(info->name, gInfo->shared_info->name, sizeof(info->name));
	strlcpy(info->chipset, "Rockchip RK3588 VOP2", sizeof(info->chipset));
	strlcpy(info->serial_no, "None", sizeof(info->serial_no));
	info->memory = gInfo->info.bytesPerRow * gInfo->info.height;
	info->dac_speed = gInfo->shared_info->pixelClockKHz / 1000;
	return B_OK;
}


sem_id
rk3588_accelerant_retrace_semaphore(void)
{
	return -1;
}
