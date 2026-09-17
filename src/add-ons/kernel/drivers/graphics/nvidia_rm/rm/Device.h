#pragma once

#include <atomic>

#include "BaseDevice.h"

#include <KernelExport.h>

#include <DPC.h>

#include <AutoDeleter.h>

#include "ContainerOf.h"


struct pci_info;


class NvHaikuDevice: public NvHaikuBaseDevice {
private:
	uint32 fDeviceIndex = 0;
	std::atomic<int32> fOpenCount {};
	bool fInitSoftware: 1 = false;
	bool fInitHardware: 1 = false;

	uint32 fIrq = 0;

	class DeferredInterruptHandler: public DPCCallback {
	public:
		NvHaikuDevice &Base() {return ContainerOf(*this, &NvHaikuDevice::fDeferredInterruptHandler);};

		void DoDPC(DPCQueue *queue) final;
	} fDeferredInterruptHandler;

	static int32 InterruptHandler(void* arg);
	int32 InterruptHandlerInt();

	status_t Start();
	status_t Stop();

public:
	static status_t Probe(ObjectDeleter<NvHaikuDevice> &outDevice, const pci_info &pciInfo);

	NvHaikuDevice();
	~NvHaikuDevice();

	uint32 DeviceIndex() const {return fDeviceIndex;}
	void SetDeviceIndex(uint32 value) {fDeviceIndex = value;}

	status_t Init(const pci_info &pciInfo);

	status_t AcquireRef();
	void ReleaseRef();

	status_t Open(uint32 flags, DevfsNodeHandle &handle);
	void Closed();
};


class NvHaikuDeviceHandle: public NvHaikuBaseDeviceHandle {
public:
	NvHaikuDeviceHandle(NvHaikuDevice *device);
	~NvHaikuDeviceHandle();

	NvHaikuDevice *GetDevice() {return static_cast<NvHaikuDevice*>(GetBaseDevice());}

	status_t Close();
};


extern const device_hooks gNvHaikuDeviceClass;
