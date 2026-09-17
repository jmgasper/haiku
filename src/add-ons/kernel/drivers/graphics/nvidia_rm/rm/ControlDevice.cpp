#include "ControlDevice.h"

#include <KernelExport.h>

#include <AutoDeleter.h>

#include "Driver.h"


const device_hooks gNvHaikuControlDeviceClass = GetDevfsNodeClass<NvHaikuDriver, NvHaikuControlDevice, NvHaikuControlDeviceHandle>();
NvHaikuControlDevice *gNvHaikuControlDeviceInst {};


NvHaikuControlDevice::NvHaikuControlDevice()
{
	Nv()->flags |= NV_FLAG_CONTROL;
	gNvHaikuControlDeviceInst = this;
	rm_init_event_locks(nullptr, Nv());
}

NvHaikuControlDevice::~NvHaikuControlDevice()
{
	rm_destroy_event_locks(nullptr, Nv());
	gNvHaikuControlDeviceInst = nullptr;
}

status_t NvHaikuControlDevice::Open(uint32 flags, DevfsNodeHandle &handle)
{
//	dprintf("NvHaikuControlDevice()::Open(%#" B_PRIx32 ")\n", flags);

	ObjectDeleter<NvHaikuControlDeviceHandle> handleImpl(new (std::nothrow) NvHaikuControlDeviceHandle(this));
	if (!handleImpl.IsSet())
		return B_NO_MEMORY;

	handle = DevfsNodeHandle(nullptr, handleImpl.Detach());

	return B_OK;
}


NvHaikuControlDeviceHandle::NvHaikuControlDeviceHandle(NvHaikuControlDevice *device):
	NvHaikuBaseDeviceHandle(device)
{
}
