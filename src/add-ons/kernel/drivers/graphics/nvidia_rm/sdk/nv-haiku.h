#pragma once

#include <OS.h>
#include <drivers/Drivers.h>


#define NVIDIA_CONTROL_DEVICE_NAME "nvidiactl"
#define NVIDIA_DEVICE_NAME "graphics/nvidia"


enum {
	NV_HAIKU_BASE = B_DEVICE_OP_CODES_END + 1,
};

enum {
	NV_HAIKU_GET_COOKIE = 0, // kernel only
	NV_HAIKU_MAP,
};


typedef struct {
	char name[B_OS_NAME_LENGTH];
	void *address;
	uint32 addressSpec;
	uint32 protection;
} nv_haiku_map_params;
