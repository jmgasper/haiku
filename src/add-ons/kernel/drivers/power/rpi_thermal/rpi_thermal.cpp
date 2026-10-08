/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Raspberry Pi's SoC temperature, read from the firmware, published as
	/dev/power/rpi_thermal/0 in the text form of the other thermal drivers
	(acpi_thermal, amd_thermal): programs like AirTop read it as they read
	those. */


#include <stdio.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>

#include <rpi_firmware.h>


#define RPI_FIRMWARE_GET_MAX_TEMPERATURE	0x0003000a

int32 api_version = B_CUR_DRIVER_API_VERSION;

static rpi_firmware_module_info* sFirmware;
static const char* sDeviceNames[] = { "power/rpi_thermal/0", NULL };


static status_t
thermal_open(const char* name, uint32 flags, void** _cookie)
{
	*_cookie = NULL;
	return B_OK;
}


static status_t
thermal_close(void* cookie)
{
	return B_OK;
}


static status_t
thermal_free(void* cookie)
{
	return B_OK;
}


static status_t
thermal_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


static status_t
read_temperature(uint32 tag, uint32* _milliCelsius)
{
	uint32 data[2] = { 0, 0 };
	status_t status = sFirmware->property(tag, data, sizeof(data));
	if (status != B_OK)
		return status;
	*_milliCelsius = data[1];
	return B_OK;
}


static status_t
thermal_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	if (position > 0) {
		*_length = 0;
		return B_OK;
	}

	uint32 current = 0, maximum = 0;
	status_t status = read_temperature(RPI_FIRMWARE_GET_TEMPERATURE, &current);
	if (status != B_OK)
		return status;
	read_temperature(RPI_FIRMWARE_GET_MAX_TEMPERATURE, &maximum);

	char text[256];
	int length = snprintf(text, sizeof(text),
		"  Critical Temperature: %" B_PRIu32 ".%" B_PRIu32 " C\n"
		"  Current Temperature: %" B_PRIu32 ".%" B_PRIu32 " C\n",
		maximum / 1000, (maximum % 1000) / 100,
		current / 1000, (current % 1000) / 100);
	if (length < 0)
		return B_ERROR;
	if ((size_t)length > *_length)
		length = *_length;
	if (user_memcpy(buffer, text, length) != B_OK)
		return B_BAD_ADDRESS;
	*_length = length;
	return B_OK;
}


static status_t
thermal_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sDeviceHooks = {
	thermal_open,
	thermal_close,
	thermal_free,
	thermal_control,
	thermal_read,
	thermal_write
};


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(RPI_FIRMWARE_MODULE_NAME,
		(module_info**)&sFirmware);
	if (status != B_OK)
		return status;

	// only where the firmware answers
	uint32 temperature;
	if (read_temperature(RPI_FIRMWARE_GET_TEMPERATURE, &temperature) != B_OK) {
		put_module(RPI_FIRMWARE_MODULE_NAME);
		return B_DEVICE_NOT_FOUND;
	}
	return B_OK;
}


void
uninit_driver()
{
	put_module(RPI_FIRMWARE_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
