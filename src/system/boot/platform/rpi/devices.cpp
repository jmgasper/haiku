/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2003-2006, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */

/*	The loader cannot read the SD card or USB disks. Its only "disk" is the
	boot archive the firmware loaded along with it: a tgz of the kernel and
	the modules needed to mount the boot volume. */


#include <string.h>

#include <KernelExport.h>

#include <boot/disk_identifier.h>
#include <boot/partitions.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <boot/vfs.h>

#include "dtb.h"


status_t
platform_add_boot_device(struct stage2_args* args, NodeList* devicesList)
{
	if (gBootArchive.size == 0) {
		dprintf("The firmware loaded no boot archive (\"initramfs\" in "
			"config.txt)\n");
		return B_DEVICE_NOT_FOUND;
	}

	MemoryDisk* disk = new(std::nothrow) MemoryDisk(
		(const uint8*)gBootArchive.start, gBootArchive.size, "boot.tgz");
	if (disk == NULL)
		return B_NO_MEMORY;

	devicesList->Add(disk);
	return B_OK;
}


status_t
platform_get_boot_partitions(struct stage2_args* args, Node* device,
	NodeList* list, NodeList* partitionList)
{
	NodeIterator iterator = list->GetIterator();
	boot::Partition* partition = (boot::Partition*)iterator.Next();
	if (partition == NULL)
		return B_ENTRY_NOT_FOUND;

	partitionList->Insert(partition);
	return B_OK;
}


status_t
platform_add_block_devices(stage2_args* args, NodeList* devicesList)
{
	return B_OK;
}


status_t
platform_register_boot_device(Node* device, disk_identifier* defaultDiskID)
{
	// The kernel has to find the boot volume by itself: booting "from an
	// image" makes it look at every disk for a system.
	disk_identifier identifier;
	memset(&identifier, 0, sizeof(identifier));
	identifier.bus_type = UNKNOWN_BUS;
	identifier.device_type = UNKNOWN_DEVICE;
	identifier.device.unknown.size = device->Size();
	for (int32 i = 0; i < NUM_DISK_CHECK_SUMS; i++) {
		identifier.device.unknown.check_sums[i].offset = -1;
		identifier.device.unknown.check_sums[i].sum = 0;
	}

	gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_HARD_DISK);
	gBootParams.SetBool(BOOT_VOLUME_BOOTED_FROM_IMAGE, true);
	gBootParams.SetData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE,
		&identifier, sizeof(identifier));
	return B_OK;
}


void
platform_cleanup_devices()
{
}
