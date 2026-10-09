/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Allwinner A733's display outputs, for the Radxa Cubie A7S:
	DisplayPort over the second USB-C port.

	The driver owns one frame buffer and shows a region of it on each output
	(the protocol of rpi_display, so that app_server's display layout and the
	Screens preferences work the same). While no display is attached, or the
	output cannot show anything yet, the DisplayPort output stands in for a
	1920x1080 display ("virtual"): the desktop then runs headless, which is
	how the board is brought up and how it runs as a server. */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <Drivers.h>
#include <KernelExport.h>

#include <boot_item.h>
#include <frame_buffer_console.h>
#include <graphic_driver.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <sunxi_display.h>

#include "registers.h"
#include "typec.h"


#define SUNXI_DISPLAY_DRIVER_MODULE_NAME	"drivers/graphics/sunxi_display/driver_v1"
#define SUNXI_DISPLAY_DEVICE_MODULE_NAME	"drivers/graphics/sunxi_display/device_v1"

#define ERROR(x...)	dprintf("sunxi_display: " x)
#define INFO(x...)	dprintf("sunxi_display: " x)

// what a headless output stands for
static const uint16 kVirtualWidth = 1920;
static const uint16 kVirtualHeight = 1080;


struct display_info {
	device_node*	node;
	mutex			lock;
	int32			openCount;
	void*			owner;			// the cookie that set a layout

	area_id			sharedArea;
	sunxi_display_shared_info* shared;

	area_id			bufferArea;
	uint8*			buffer;
	phys_addr_t		bufferAddress;
	size_t			bufferSize;

	port_id			changePort;
	int32			changeCode;
	void*			notificationOwner;

	sunxi::TypeCPort* typeC;
	thread_id		pollThread;
	int32			stopping;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - outputs


static void
init_outputs(display_info* info)
{
	sunxi_display_shared_info& shared = *info->shared;
	shared.output_count = 1;

	sunxi_display_output& output = shared.outputs[0];
	output.id = SUNXI_DISPLAY_OUTPUT_DP0;
	output.flags = SUNXI_DISPLAY_OUTPUT_CONNECTED
		| SUNXI_DISPLAY_OUTPUT_VIRTUAL;
	output.native_width = kVirtualWidth;
	output.native_height = kVirtualHeight;
	output.edid_length = 0;
	INFO("DP-1: no display, standing in for a %ux%u one\n",
		output.native_width, output.native_height);
}


/*!	Runs the USB-C port's state machine: often while something happens
	there, otherwise when the port controller signals (PL3, active low) and
	now and then.
*/
static status_t
poll_outputs(void* cookie)
{
	display_info* info = (display_info*)cookie;
	int32 changes = info->typeC->Changes();
	bigtime_t lastPoll = 0;
	while (atomic_get(&info->stopping) == 0) {
		snooze(20000);
		bigtime_t now = system_time();
		bool interrupt = !sunxi::r_pin_get(0, 3);
		if (!interrupt && info->typeC->Idle() && now - lastPoll < 250000)
			continue;
		lastPoll = now;
		info->typeC->Poll();

		int32 current = info->typeC->Changes();
		if (current == changes)
			continue;
		changes = current;
		INFO("DP-1: %s, HPD %s\n", info->typeC->DisplayPortReady()
				? "alt mode ready" : "no alt mode",
			info->typeC->HotPlug() ? "high" : "low");
	}
	return B_OK;
}


//	#pragma mark - frame buffer


/*!	A frame buffer of at least \a size bytes: contiguous, write-combining.
	An existing one that is large enough stays.
*/
static status_t
ensure_buffer(display_info* info, size_t size)
{
	size = ROUNDUP(size, B_PAGE_SIZE);
	if (info->bufferArea >= 0 && info->bufferSize >= size)
		return B_OK;

	// The display engine reads 32-bit addresses.
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 32;
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "sunxi display frame buffer",
		size, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(address, size, &entry, 1);
	if (status == B_OK) {
		status = vm_set_area_memory_type(area, entry.address,
			B_WRITE_COMBINING_MEMORY);
	}
	if (status != B_OK) {
		delete_area(area);
		return status;
	}
	memset(address, 0, size);

	// Whoever still maps the old buffer keeps its memory; nothing shows it.
	if (info->bufferArea >= 0)
		delete_area(info->bufferArea);

	info->bufferArea = area;
	info->buffer = (uint8*)address;
	info->bufferAddress = entry.address;
	info->bufferSize = size;
	info->shared->generation++;
	info->shared->physical_address = entry.address;
	return B_OK;
}


static status_t
set_layout(display_info* info, const sunxi_display_layout& layout)
{
	sunxi_display_shared_info& shared = *info->shared;

	if (layout.version != SUNXI_DISPLAY_VERSION || layout.width == 0
		|| layout.height == 0 || layout.width > 8192 || layout.height > 8192) {
		return B_BAD_VALUE;
	}

	uint32 enabled = 0;
	for (uint32 i = 0; i < shared.output_count; i++) {
		const sunxi_display_output& request = layout.outputs[i];
		if ((request.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		if ((shared.outputs[i].flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) == 0
			|| request.x < 0 || request.y < 0 || request.width == 0
			|| request.height == 0
			|| request.x + request.width > (int32)layout.width
			|| request.y + request.height > (int32)layout.height) {
			return B_BAD_VALUE;
		}
		enabled++;
	}
	if (enabled == 0)
		return B_BAD_VALUE;

	uint32 bytesPerRow = layout.width * 4;
	status_t status = ensure_buffer(info, (size_t)bytesPerRow * layout.height);
	if (status != B_OK)
		return status;

	for (uint32 i = 0; i < shared.output_count; i++) {
		sunxi_display_output& output = shared.outputs[i];
		const sunxi_display_output& request = layout.outputs[i];
		if ((output.flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) == 0)
			continue;

		uint32 kept = output.flags & (SUNXI_DISPLAY_OUTPUT_CONNECTED
			| SUNXI_DISPLAY_OUTPUT_VIRTUAL);
		if ((request.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) == 0) {
			output.flags = kept;
			continue;
		}

		output.flags = kept | SUNXI_DISPLAY_OUTPUT_ENABLED
			| (request.flags & SUNXI_DISPLAY_OUTPUT_MIRROR);
		output.x = request.x;
		output.y = request.y;
		output.width = request.width;
		output.height = request.height;
		output.mode_width = request.mode_width;
		output.mode_height = request.mode_height;
		output.scale = request.scale;
		output.render_scale = request.render_scale;

		INFO("output %" B_PRIu32 ": %ux%u at %" B_PRId32 ",%" B_PRId32
			" of %" B_PRIu32 "x%" B_PRIu32 "%s%s\n", i, request.width,
			request.height, request.x, request.y, layout.width, layout.height,
			(request.flags & SUNXI_DISPLAY_OUTPUT_MIRROR) != 0 ? ", mirror" : "",
			(output.flags & SUNXI_DISPLAY_OUTPUT_VIRTUAL) != 0
				? ", headless" : "");
	}

	shared.width = layout.width;
	shared.height = layout.height;
	shared.bytes_per_row = bytesPerRow;

	// the kernel's console and debugger follow
	frame_buffer_update((addr_t)info->buffer, layout.width, layout.height, 32,
		bytesPerRow);
	return B_OK;
}


static void
release_layout(display_info* info)
{
	sunxi_display_shared_info& shared = *info->shared;
	for (uint32 i = 0; i < shared.output_count; i++) {
		shared.outputs[i].flags &= SUNXI_DISPLAY_OUTPUT_CONNECTED
			| SUNXI_DISPLAY_OUTPUT_VIRTUAL;
	}
}


//	#pragma mark - device


static status_t
display_init_device(void* _info, void** _cookie)
{
	display_info* info = (display_info*)_info;

	info->sharedArea = create_area("sunxi display shared info",
		(void**)&info->shared, B_ANY_KERNEL_ADDRESS,
		ROUNDUP(sizeof(sunxi_display_shared_info), B_PAGE_SIZE), B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (info->sharedArea < 0)
		return info->sharedArea;
	memset(info->shared, 0, sizeof(sunxi_display_shared_info));
	info->shared->version = SUNXI_DISPLAY_VERSION;
	info->bufferArea = -1;

	init_outputs(info);

	// a frame buffer for the first output, taken while contiguous memory is
	// still easy to find
	const sunxi_display_output& first = info->shared->outputs[0];
	status_t status = ensure_buffer(info,
		(size_t)first.native_width * 4 * first.native_height);
	if (status != B_OK) {
		ERROR("no memory for the frame buffer: %s\n", strerror(status));
		delete_area(info->sharedArea);
		return status;
	}

	mutex_init(&info->lock, "sunxi display");
	info->changePort = -1;

	// the USB-C port the DisplayPort output goes through
	info->pollThread = -1;
	info->typeC = new(std::nothrow) sunxi::TypeCPort;
	if (info->typeC != NULL) {
		sunxi::r_pin_set_function(0, 3, sunxi::PIN_FUNCTION_INPUT);
		status = info->typeC->Init();
		if (status != B_OK) {
			ERROR("no USB-C port controller: %s\n", strerror(status));
			delete info->typeC;
			info->typeC = NULL;
		}
	}
	if (info->typeC != NULL) {
		info->pollThread = spawn_kernel_thread(poll_outputs,
			"sunxi display outputs", B_NORMAL_PRIORITY, info);
		if (info->pollThread >= 0)
			resume_thread(info->pollThread);
	}

	*_cookie = info;
	return B_OK;
}


static void
display_uninit_device(void* cookie)
{
	display_info* info = (display_info*)cookie;
	atomic_set(&info->stopping, 1);
	if (info->pollThread >= 0) {
		status_t result;
		wait_for_thread(info->pollThread, &result);
	}
	delete info->typeC;
	mutex_destroy(&info->lock);
	if (info->bufferArea >= 0)
		delete_area(info->bufferArea);
	delete_area(info->sharedArea);
}


static status_t
display_open(void* _info, const char* path, int openMode, void** _cookie)
{
	display_info* info = (display_info*)_info;

	// the cookie only tells one open from another
	void* cookie = malloc(1);
	if (cookie == NULL)
		return B_NO_MEMORY;

	atomic_add(&info->openCount, 1);
	*_cookie = cookie;
	return B_OK;
}


static status_t
display_close(void* cookie)
{
	return B_OK;
}


// device_manager hands the hooks the cookie of open(); the device is found
// through a global, there is only one.
static display_info* sInfo;


static status_t
display_free(void* cookie)
{
	display_info* info = sInfo;
	MutexLocker locker(info->lock);
	if (info->notificationOwner == cookie) {
		info->notificationOwner = NULL;
		info->changePort = -1;
	}
	if (info->owner == cookie) {
		release_layout(info);
		info->owner = NULL;
	}
	atomic_add(&info->openCount, -1);
	free(cookie);
	return B_OK;
}


static status_t
display_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	display_info* info = sInfo;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			return user_strlcpy((char*)buffer, SUNXI_DISPLAY_ACCELERANT,
				B_FILE_NAME_LENGTH) < B_OK ? B_BAD_ADDRESS : B_OK;

		case SUNXI_DISPLAY_GET_SHARED_AREA:
			return user_memcpy(buffer, &info->sharedArea, sizeof(area_id));

		case SUNXI_DISPLAY_GET_STATE:
		{
			if (length != sizeof(sunxi_display_shared_info))
				return B_BAD_VALUE;
			MutexLocker locker(info->lock);
			return user_memcpy(buffer, info->shared, sizeof(*info->shared));
		}

		case SUNXI_DISPLAY_SET_CHANGE_PORT:
		{
			sunxi_display_change_port request;
			if (length != sizeof(request))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&request, buffer, sizeof(request));
			if (status != B_OK)
				return status;
			if (request.port >= 0) {
				port_info port;
				status = get_port_info(request.port, &port);
				if (status != B_OK || port.team != team_get_current_team_id())
					return B_NOT_ALLOWED;
			}
			MutexLocker locker(info->lock);
			if (info->notificationOwner != NULL
				&& info->notificationOwner != cookie)
				return B_BUSY;
			info->notificationOwner = request.port >= 0 ? cookie : NULL;
			info->changePort = request.port;
			info->changeCode = request.code;
			// Catch changes between the initial output query and registration.
			if (request.port >= 0)
				write_port_etc(request.port, request.code, NULL, 0,
					B_RELATIVE_TIMEOUT, 0);
			return B_OK;
		}

		case SUNXI_DISPLAY_SET_LAYOUT:
		{
			sunxi_display_layout layout;
			if (length != sizeof(layout))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&layout, buffer, sizeof(layout));
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			if (info->owner != NULL && info->owner != cookie)
				return B_BUSY;
			status = set_layout(info, layout);
			if (status == B_OK)
				info->owner = cookie;
			return status;
		}

		case SUNXI_DISPLAY_CLONE_FRAME_BUFFER:
		{
			MutexLocker locker(info->lock);
			void* address = NULL;
			area_id area = vm_clone_area(team_get_current_team_id(),
				"sunxi display frame buffer", &address, B_ANY_ADDRESS,
				B_READ_AREA | B_WRITE_AREA, 0, info->bufferArea, true);
			if (area < 0)
				return area;

			area_info areaInfo = {};
			areaInfo.area = area;
			areaInfo.address = address;
			areaInfo.size = info->bufferSize;
			status_t status = user_memcpy(buffer, &areaInfo, sizeof(areaInfo));
			if (status != B_OK)
				vm_delete_area(team_get_current_team_id(), area, true);
			return status;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static float
display_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return -1.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "allwinner,sun60i-a733-display-engine") != 0) {
		return -1.0f;
	}

	return 1.0f;
}


static status_t
display_register_device(device_node* parent)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Allwinner display engine" } },
		{}
	};

	return sDeviceManager->register_node(parent,
		SUNXI_DISPLAY_DRIVER_MODULE_NAME, attributes, NULL, NULL);
}


static status_t
display_init_driver(device_node* node, void** _cookie)
{
	display_info* info = (display_info*)calloc(1, sizeof(display_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	sInfo = info;
	*_cookie = info;
	return B_OK;
}


static void
display_uninit_driver(void* cookie)
{
	free(cookie);
}


static status_t
display_register_child_devices(void* cookie)
{
	display_info* info = (display_info*)cookie;
	return sDeviceManager->publish_device(info->node, SUNXI_DISPLAY_DEVICE,
		SUNXI_DISPLAY_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};

static device_module_info sDisplayDevice = {
	{
		SUNXI_DISPLAY_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	display_init_device,
	display_uninit_device,
	NULL,	// removed
	display_open,
	display_close,
	display_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	display_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sDisplayDriver = {
	{
		SUNXI_DISPLAY_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	display_supports_device,
	display_register_device,
	display_init_driver,
	display_uninit_driver,
	display_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sDisplayDriver,
	(module_info*)&sDisplayDevice,
	NULL
};
