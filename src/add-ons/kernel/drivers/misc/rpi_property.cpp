/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	A lab device: /dev/misc/rpi_property passes one property tag to the
	Raspberry Pi's firmware. Not in any image; it must only be installed on
	a Raspberry Pi (the firmware module does not probe). */


#include <Drivers.h>
#include <KernelExport.h>

#include <new>
#include <string.h>

#include <rpi_firmware.h>


#define RPI_PROPERTY_REQUEST	(B_DEVICE_OP_CODES_END + 1)

struct rpi_property_request {
	uint32	tag;
	uint32	size;			// bytes of data, a multiple of 4
	uint32	data[256];
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static rpi_firmware_module_info* sFirmware;
static const char* sDeviceNames[] = { "misc/rpi_property", NULL };


static status_t
property_open(const char* name, uint32 flags, void** _cookie)
{
	*_cookie = NULL;
	return B_OK;
}


static status_t
property_close(void* cookie)
{
	return B_OK;
}


static status_t
property_free(void* cookie)
{
	return B_OK;
}


static status_t
property_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	if (op != RPI_PROPERTY_REQUEST)
		return B_DEV_INVALID_IOCTL;

	rpi_property_request* request = new(std::nothrow) rpi_property_request;
	if (request == NULL)
		return B_NO_MEMORY;

	status_t status = user_memcpy(request, buffer, sizeof(*request));
	if (status == B_OK && (request->size > sizeof(request->data)
			|| (request->size & 3) != 0)) {
		status = B_BAD_VALUE;
	}
	if (status == B_OK) {
		status = sFirmware->property(request->tag, request->data,
			request->size);
	}
	if (status == B_OK)
		status = user_memcpy(buffer, request, sizeof(*request));

	delete request;
	return status;
}


static status_t
property_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_OK;
}


static status_t
property_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	return B_NOT_ALLOWED;
}


static device_hooks sHooks = {
	property_open,
	property_close,
	property_free,
	property_control,
	property_read,
	property_write
};


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	return get_module(RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware);
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
	return &sHooks;
}
