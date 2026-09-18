#include "Driver.h"
#include "RmStack.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>
#include <kdevice_manager.h>

#include "nv-include.h"
#include "nv-haiku.h"
#include "nv-haiku-kernel.h"

#include "ControlDevice.h"
#include "Device.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


NvHaikuDriver NvHaikuDriver::sInstance;


static bool ParseUint32(const char *str, char *&strEnd, uint32 &res)
{
	// do not allow leading spaces or zeroes
	if (!(strcmp(str, "0") == 0 || (str[0] >= '1' && str[0] <= '9'))) {
		return false;
	}
	long val = strtol(str, &strEnd, 10);
	if (strEnd == str) {
		return false;
	}
	if ((val == LONG_MIN || val == LONG_MAX) && errno == ERANGE) {
		return false;
	}
	if (val < 0 || val > UINT32_MAX) {
		return false;
	}
	res = val;
	return true;
}

static bool ParseDeviceName(const char *name, uint32 &index)
{
	const char *deviceNamePrefix = NVIDIA_DEVICE_NAME;
	size_t deviceNamePrefixLen = strlen(deviceNamePrefix);
	if (strncmp(deviceNamePrefix, name, deviceNamePrefixLen) != 0) {
		return false;
	}

	char *strEnd {};
	if (!ParseUint32(name + deviceNamePrefixLen, strEnd, index)) {
		return false;
	}
	if (*strEnd != '\0') {
		return false;
	}
	return true;
}


NvHaikuDriver::~NvHaikuDriver()
{
	dprintf("-NvHaikuDriver\n");

	if (fPowerHookAdded)
		device_manager_remove_power_hook(PowerHook, this);

	for (int32 i = 0; i < fDevices.Count(); i++) {
		delete fDevices[i];
	}

	if (fControlDevice != nullptr) {
		delete fControlDevice;
	}

	if (fInitRmDone) {
		RmStack stack;
		rm_shutdown_rm(stack.Get());
	}

	if (fWorkQueueInitDone)
		nv_haiku_work_queue_uninit();

	fIntrSafePool.ReclaimAll();

	if (fPCI != nullptr) {
		put_module(B_PCI_MODULE_NAME);
	}
}

status_t NvHaikuDriver::Init()
{
	dprintf("+NvHaikuDriver\n");

	CHECK_RET(get_module(B_PCI_MODULE_NAME, (module_info**)&fPCI));

	fResumeCondition.Init(this, "nvidia_rm resume");

	fIntrSafePool.Maintain();

	CHECK_RET(nv_haiku_work_queue_init());
	fWorkQueueInitDone = true;

	RmStack stack;
	if (!stack.IsValid())
		return B_NO_MEMORY;
	if (!rm_init_rm(stack.Get())) {
		return B_ERROR;
	}
	fInitRmDone = true;

	nv_haiku_apply_registry_settings(stack.Get());

	fControlDevice = new(std::nothrow) NvHaikuControlDevice();
	if (fControlDevice == nullptr) {
		return B_NO_MEMORY;
	}

	dprintf("Scan PCI:\n");
	pci_info pciInfo {};
	for (uint32 i = 0; fPCI->get_nth_pci_info(i, &pciInfo) >= B_OK; i++) {
		ObjectDeleter<NvHaikuDevice> device;
		if (NvHaikuDevice::Probe(device, pciInfo) < B_OK)
			continue;

		uint32 deviceIndex = fDevices.Count();
		CHECK_RET(fDevices.Add(device.Get()));
		device->SetDeviceIndex(deviceIndex);
		device.Detach();
	}

	CHECK_RET(fDeviceNamesArray.Init(1 + fDevices.Count()));
	fDeviceNamesArray.SetName(0, NVIDIA_CONTROL_DEVICE_NAME);

	if (device_manager_add_power_hook(PowerHook, this, "nvidia_rm") == B_OK)
		fPowerHookAdded = true;
	for (int32 i = 0; i < fDevices.Count(); i++) {
		char name[128];
		sprintf(name, NVIDIA_DEVICE_NAME "%" B_PRId32, i);
		fDeviceNamesArray.SetName(1 + i, name);
	}

	return B_OK;
}

status_t NvHaikuDriver::PowerHook(void *cookie, bool resume, int32 state)
{
	NvHaikuDriver *driver = static_cast<NvHaikuDriver*>(cookie);
	status_t result = B_OK;

	if (!resume) {
		// Like Linux: stop user channels of all GPUs before suspending them.
		for (int32 i = 0; i < driver->fDevices.Count(); i++)
			driver->fDevices[i]->PreemptUserChannels();
		for (int32 i = driver->fDevices.Count() - 1; i >= 0; i--) {
			status_t status = driver->fDevices[i]->Suspend();
			if (status != B_OK)
				result = status;
		}
	} else {
		for (int32 i = 0; i < driver->fDevices.Count(); i++) {
			status_t status = driver->fDevices[i]->Resume();
			if (status != B_OK)
				result = status;
		}
		for (int32 i = 0; i < driver->fDevices.Count(); i++)
			driver->fDevices[i]->RestoreUserChannels();

		driver->fResumeGeneration++;
		driver->fResumeCondition.NotifyAll();
	}

	return result;
}


// The accelerant tells us where the screen's frame buffer is; anyone drawing
// with the GPU can then ask for it and put finished frames there directly.
void NvHaikuDriver::PublishScanout(const nv_haiku_scanout_info &info)
{
	MutexLocker _(&fScanoutLock);
	fScanout = info;
	dprintf("nvidia_rm: scanout published: %" B_PRIu32 "x%" B_PRIu32 ", %" B_PRIu32
		" bytes per row, client %#" B_PRIx32 ", memory %#" B_PRIx32 "\n",
		info.width, info.height, info.bytes_per_row, info.client, info.memory);
}

status_t NvHaikuDriver::GetScanout(nv_haiku_scanout_info &info)
{
	MutexLocker _(&fScanoutLock);
	if (fScanout.memory == 0)
		return B_DEV_NOT_READY;

	info = fScanout;
	return B_OK;
}


status_t NvHaikuDriver::WaitForResume(uint32 &generation, bigtime_t timeout)
{
	if (fResumeGeneration == generation) {
		ConditionVariableEntry entry;
		fResumeCondition.Add(&entry);
		if (fResumeGeneration == generation) {
			status_t status = entry.Wait(B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT,
				timeout);
			if (status != B_OK && status != B_TIMED_OUT && status != B_WOULD_BLOCK)
				return status;
		}
	}

	generation = fResumeGeneration;
	return B_OK;
}


status_t NvHaikuDriver::InitDriver()
{
	dprintf("nvidia_rm: InitDriver\n");
	module_info *moduleInfo;
	return get_module(NV_HAIKU_MODULE_NAME, &moduleInfo);
}

void NvHaikuDriver::UninitDriver()
{
	dprintf("nvidia_rm: UninitDriver\n");
	put_module(NV_HAIKU_MODULE_NAME);
}

const char **NvHaikuDriver::PublishDevices()
{
	return Instance().fDeviceNamesArray.Names();
}

DevfsNodeInstance NvHaikuDriver::FindDevice(const char *name)
{
	if (strcmp(name, NVIDIA_CONTROL_DEVICE_NAME) == 0) {
		return DevfsNodeInstance(&gNvHaikuControlDeviceClass, Instance().fControlDevice);
	}
	uint32 index;
	if (ParseDeviceName(name, index)) {
		if (index >= (uint32)Instance().fDevices.Count()) {
			return DevfsNodeInstance();
		}
		return DevfsNodeInstance(&gNvHaikuDeviceClass, Instance().fDevices[index]);
	}
	return DevfsNodeInstance();
}


DRIVER_ENTRY_POINTS(NvHaikuDriver)
