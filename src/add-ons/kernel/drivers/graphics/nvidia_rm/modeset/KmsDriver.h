#pragma once

#include <lock.h>

#include "Drivers_cpp.h"

#include "KmsDevice.h"
#include "TimerQueue.h"

struct nvidia_module_info;


class NvHaikuKmsDriver {
private:
	nvidia_module_info *fNvidiaModule {};
	mutex fLocker = MUTEX_INITIALIZER("nvkms");
	bool fIsKmsLoaded = false;
	NvTimerQueue fTimerQueue;

	NvHaikuKmsDevice fDevice;

	static NvHaikuKmsDriver sInstance;

public:
	~NvHaikuKmsDriver();
	status_t Init();

	static NvHaikuKmsDriver &Instance() {return sInstance;}

	nvidia_module_info &NvidiaModule() const {return *fNvidiaModule;}
	mutex &Locker() {return fLocker;}
	NvTimerQueue &TimerQueue() {return fTimerQueue;}

	static status_t InitDriver();
	static void UninitDriver();
	static const char **PublishDevices();
	static DevfsNodeInstance FindDevice(const char *name);
};
