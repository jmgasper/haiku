/*
 * MediaTek MT7922 wireless, driver entry points.
 *
 * Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Drivers.h>

#include "mt7922.h"


#define TRACE(x...)	dprintf("mt7922: " x)
#define ERROR(x...)	dprintf("mt7922: " x)

#define MAX_DEVICES	4


int32 api_version = B_CUR_DRIVER_API_VERSION;

pci_module_info* gPci = NULL;

static mt7922_dev* sDevices[MAX_DEVICES];
static char* sDeviceNames[MAX_DEVICES + 1];
static int32 sDeviceCount = 0;


static bool
is_supported(const pci_info& info)
{
	if (info.vendor_id != MT7922_VENDOR_ID)
		return false;

	switch (info.device_id) {
		case 0x7922:
		case 0x7961:
		case 0x0616:
			return true;
	}

	return false;
}


status_t
init_hardware(void)
{
	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return ENOSYS;

	pci_info info;
	for (int32 i = 0; pci->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (is_supported(info)) {
			put_module(B_PCI_MODULE_NAME);
			return B_OK;
		}
	}

	put_module(B_PCI_MODULE_NAME);
	return ENODEV;
}


status_t
init_driver(void)
{
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&gPci) != B_OK)
		return ENOSYS;

	memset(sDevices, 0, sizeof(sDevices));
	memset(sDeviceNames, 0, sizeof(sDeviceNames));
	sDeviceCount = 0;

	pci_info info;
	for (int32 i = 0; gPci->get_nth_pci_info(i, &info) == B_OK
			&& sDeviceCount < MAX_DEVICES; i++) {
		if (!is_supported(info))
			continue;

		mt7922_dev* device = (mt7922_dev*)malloc(sizeof(mt7922_dev));
		if (device == NULL)
			break;

		memset(device, 0, sizeof(mt7922_dev));
		device->pci = info;
		device->registersArea = -1;
		device->chosen = -1;
		/* Not under net/ yet. The network stack takes anything published
		 * there for a working interface and sets about trying to configure
		 * it, and this card cannot carry a packet until it has its firmware
		 * and its transfer rings. It moves there when that is true.
		 */
		sprintf(device->name, "mt7922/%" B_PRId32, sDeviceCount);

		sDeviceNames[sDeviceCount] = strdup(device->name);
		if (sDeviceNames[sDeviceCount] == NULL) {
			free(device);
			break;
		}

		sDevices[sDeviceCount] = device;
		sDeviceCount++;

		TRACE("found MT%04x at %d:%d:%d\n", info.device_id, info.bus,
			info.device, info.function);
	}

	if (sDeviceCount == 0) {
		put_module(B_PCI_MODULE_NAME);
		return ENODEV;
	}

	return B_OK;
}


void
uninit_driver(void)
{
	for (int32 i = 0; i < sDeviceCount; i++) {
		if (sDevices[i] != NULL) {
			mt7922_teardown(sDevices[i]);
			free(sDevices[i]);
		}
		free(sDeviceNames[i]);
	}

	sDeviceCount = 0;
	put_module(B_PCI_MODULE_NAME);
}


static status_t
device_open(const char* name, uint32 flags, void** cookie)
{
	for (int32 i = 0; i < sDeviceCount; i++) {
		if (strcmp(name, sDevices[i]->name) != 0)
			continue;

		/* The card is brought up when it is first wanted rather than when it
		 * is found, because what it needs next is a firmware image off the
		 * disk, and disks are not mounted when drivers are looked for.
		 */
		if (sDevices[i]->registers == NULL) {
			status_t status = mt7922_setup(sDevices[i]);
			if (status != B_OK)
				return status;
		}

		*cookie = sDevices[i];
		return B_OK;
	}

	return B_ERROR;
}


static status_t
device_close(void* cookie)
{
	return B_OK;
}


static status_t
device_free(void* cookie)
{
	return B_OK;
}


static status_t
device_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


static status_t
device_read(void* cookie, off_t position, void* buffer, size_t* length)
{
	*length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
device_write(void* cookie, off_t position, const void* buffer, size_t* length)
{
	*length = 0;
	return B_NOT_SUPPORTED;
}


static device_hooks sHooks = {
	device_open,
	device_close,
	device_free,
	device_control,
	device_read,
	device_write,
	NULL,
	NULL,
	NULL,
	NULL
};


const char**
publish_devices(void)
{
	return (const char**)sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
