#pragma once

extern "C" {
#include "nvkms-ioctl.h"
#include "nvkms-api.h"
}


class NvKmsApi;


class NvKmsDevice {
private:
	NvKmsApi &fNvKmsApi;
	NvKmsAllocDeviceReply fInfo;

public:
	NvKmsDevice(NvKmsApi &nvKmsApi, NvU32 rmDeviceId);
	~NvKmsDevice();

	NvKmsApi &Kms() {return fNvKmsApi;}
	NvKmsDeviceHandle Get() const {return fInfo.deviceHandle;}
	const NvKmsAllocDeviceReply &Info() const {return fInfo;}
};
