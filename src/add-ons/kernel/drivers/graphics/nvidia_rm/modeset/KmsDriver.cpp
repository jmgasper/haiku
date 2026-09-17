#include "KmsDriver.h"

#include <new>

#include <util/AutoLock.h>

extern "C" {
#include <nvkms.h>
}

#include "Drivers_cpp.h"
#include "rm/nv-haiku-kernel.h"

#include "KmsDevice.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


NvHaikuKmsDriver NvHaikuKmsDriver::sInstance;


static void
nvkms_suspend(NvU32 gpuId)
{
	MutexLocker lock(&NvHaikuKmsDriver::Instance().Locker());
	nvKmsSuspend(gpuId);
}


static void
nvkms_resume(NvU32 gpuId)
{
	MutexLocker lock(&NvHaikuKmsDriver::Instance().Locker());
	nvKmsResume(gpuId);
}


static const nvidia_modeset_callbacks_t sKmsCallbacks = {
	.suspend = nvkms_suspend,
	.resume = nvkms_resume,
};


NvHaikuKmsDriver::~NvHaikuKmsDriver()
{
	if (fNvidiaModule != nullptr)
		fNvidiaModule->set_callbacks(nullptr);
	if (fIsKmsLoaded) {
		MutexLocker lock(&fLocker);
		nvKmsModuleUnload();
	}
	// Drain the timer queue without the NVKMS lock held: DoDPC acquires it,
	// and Fini() waits for in-flight DPCs to complete.
	fTimerQueue.Fini();
	if (fNvidiaModule != nullptr) {
		put_module(NV_HAIKU_MODULE_NAME);
	}
}

status_t NvHaikuKmsDriver::Init()
{
	CHECK_RET(get_module(NV_HAIKU_MODULE_NAME, (module_info**)&fNvidiaModule));

	CHECK_RET(fTimerQueue.Init());

	NvBool done;
	{
		MutexLocker lock(&fLocker);
		done = nvKmsModuleLoad();
	}
	if (!done) {
		return B_NO_INIT;
	}
	fIsKmsLoaded = true;

	fNvidiaModule->set_callbacks(&sKmsCallbacks);

	return B_OK;
}


status_t NvHaikuKmsDriver::InitDriver()
{
	new(&Instance()) NvHaikuKmsDriver();
	status_t res = Instance().Init();
	if (res < B_OK) {
		Instance().~NvHaikuKmsDriver();
	}
	return res;
}

void NvHaikuKmsDriver::UninitDriver()
{
	Instance().~NvHaikuKmsDriver();
}

const char **NvHaikuKmsDriver::PublishDevices()
{
	static const char *deviceNames[] = {
		"nvidia-modeset",
		nullptr
	};
	return deviceNames;
}

DevfsNodeInstance NvHaikuKmsDriver::FindDevice(const char *name)
{
	if (strcmp(name, "nvidia-modeset") == 0) {
		return DevfsNodeInstance(&gNvHaikuKmsDeviceClass, &Instance().fDevice);
	}
	return DevfsNodeInstance();
}


DRIVER_ENTRY_POINTS(NvHaikuKmsDriver)
