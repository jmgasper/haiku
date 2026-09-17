#pragma once

#include <drivers/Drivers.h>


#define NVIDIA_CONTROL_DEVIVE_NAME "nvidiactl"
#define NVIDIA_DEVIVE_NAME "graphics/nvidia"


enum {
	NV_HAIKU_BASE = B_DEVICE_OP_CODES_END + 1,
};

enum {
	NV_HAIKU_GET_COOKIE = 0, // kernel only
	NV_HAIKU_MAP,
	NV_HAIKU_READ_REGISTER, // development aid
	NV_HAIKU_READ_VRAM, // development aid
	NV_HAIKU_WAIT_FOR_RESUME,
};

// NV_HAIKU_WAIT_FOR_RESUME: waits until the resume generation differs from
// the passed one (or the timeout expires) and returns the current one. Display
// state is lost when resuming from suspend, so clients restore their modes
// when the generation changes.
typedef struct {
	uint32 generation;
	uint32 reserved;
	int64 timeout;
} nv_haiku_resume_params;

typedef struct {
	uint64 offset;
	uint32 values[16];
} nv_haiku_vram_params;

typedef struct {
	uint32 offset;
	uint32 value;
} nv_haiku_register_params;


typedef struct {
	char name[B_OS_NAME_LENGTH];
	void *address;
	uint32 addressSpec;
	uint32 protection;
} nv_haiku_map_params;
