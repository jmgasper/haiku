#pragma once

#include "BaseDevice.h"


class NvHaikuControlDevice: public NvHaikuBaseDevice {
public:
	NvHaikuControlDevice();
	~NvHaikuControlDevice();

	status_t Open(uint32 flags, DevfsNodeHandle &handle);
};


class NvHaikuControlDeviceHandle: public NvHaikuBaseDeviceHandle {
public:
	NvHaikuControlDeviceHandle(NvHaikuControlDevice *device);
};


extern const device_hooks gNvHaikuControlDeviceClass;
extern NvHaikuControlDevice *gNvHaikuControlDeviceInst;
