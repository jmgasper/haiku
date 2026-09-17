#include "Device.h"

#include <PCI.h>

extern "C" {
#include <os-interface.h>
}

#include "Driver.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}

#define PCI_VENDOR_ID_NVIDIA 0x10DE


const device_hooks gNvHaikuDeviceClass = GetDevfsNodeClass<NvHaikuDriver, NvHaikuDevice, NvHaikuDeviceHandle>();


status_t NvHaikuDevice::Probe(ObjectDeleter<NvHaikuDevice> &outDevice, const pci_info &pciInfo)
{
	if (!(
		pciInfo.vendor_id == PCI_VENDOR_ID_NVIDIA &&
		pciInfo.class_base == PCI_display && (
			pciInfo.class_sub == PCI_vga ||
			pciInfo.class_sub == PCI_3d
		)
	))
		return ENODEV;

	dprintf("  %02x.%02x.%02x: %04x.%04x\n",
		pciInfo.bus,
		pciInfo.device,
		pciInfo.function,
		pciInfo.vendor_id,
		pciInfo.device_id
	);

	ObjectDeleter<NvHaikuDevice> device(new(std::nothrow) NvHaikuDevice());
	if (!device.IsSet())
		return B_NO_MEMORY;

	CHECK_RET(device->Init(pciInfo));

	outDevice.SetTo(device.Detach());
	return B_OK;
}

NvHaikuDevice::NvHaikuDevice()
{
	dprintf("+NvHaikuDevice()\n");
}

NvHaikuDevice::~NvHaikuDevice()
{
	dprintf("-NvHaikuDevice()\n");

	nv_state_t *nv = Nv();

	if (fInitSoftware) {
		dprintf("rm_free_private_state()\n");
		rm_free_private_state(nullptr, nv);
		fInitSoftware = false;
	}
}

status_t NvHaikuDevice::Init(const pci_info &pciInfo)
{
	nv_state_t *nv = Nv();

	nv->pci_info.domain    = 0;
	nv->pci_info.bus       = pciInfo.bus;
	nv->pci_info.slot      = pciInfo.device;
	nv->pci_info.function  = pciInfo.function;
	nv->pci_info.vendor_id = pciInfo.vendor_id;
	nv->pci_info.device_id = pciInfo.device_id;
	nv->subsystem_id       = pciInfo.u.h0.subsystem_id;
	nv->subsystem_vendor   = pciInfo.u.h0.subsystem_vendor_id;

	nv->handle = os_pci_init_handle(0, pciInfo.bus, pciInfo.device, pciInfo.function, nullptr, nullptr);

  nv->regs = &nv->bars[NV_GPU_BAR_INDEX_REGS];
  nv->fb   = &nv->bars[NV_GPU_BAR_INDEX_FB];

	uint32 curNvBar = 0;
	for (uint32 bar = 0; bar < 6; bar++) {
		uint32 curBar = bar;
		uint64 physAdr = pciInfo.u.h0.base_registers[bar];
		uint64 size    = pciInfo.u.h0.base_register_sizes[bar];
		if ((pciInfo.u.h0.base_register_flags[bar] & PCI_address_type) == PCI_address_type_64) {
			bar++;
			physAdr |= (uint64)pciInfo.u.h0.base_registers[bar] << 32;
			size    |= (uint64)pciInfo.u.h0.base_register_sizes[bar] << 32;
		}

		if (size > 0 && curNvBar < NV_GPU_NUM_BARS) {
			nv->bars[curNvBar].offset = NVRM_PCICFG_BAR_OFFSET(curBar);
			nv->bars[curNvBar].cpu_address = physAdr;
			nv->bars[curNvBar].size = size;
			curNvBar++;
		}

		dprintf("    BAR[%" B_PRIu32 "]: %#" B_PRIx64 ", %#" B_PRIx64 "\n",
			curBar,
			physAdr,
			size
		);
	}

	if (!rm_init_private_state(nullptr, nv))
		return B_ERROR;

	fInitSoftware = true;

	rm_set_rm_firmware_requested(nullptr, nv);


	return B_OK;
}

status_t NvHaikuDevice::Start()
{
	dprintf("NvHaikuDevice()::Start()\n");
	nv_state_t *nv = Nv();

	NvU32 pciConfig = 0;
	os_pci_read_dword(nv->handle, PCI_command, &pciConfig);
	pciConfig |= PCI_command_io | PCI_command_memory | PCI_command_master;
	os_pci_write_dword(nv->handle, PCI_command, pciConfig);

	if (NvHaikuDriver::Instance().PCI().get_msi_count(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function) < 1) {
		dprintf("[!] no MSI support\n");
		return B_ERROR;
	}

	NvHaikuDriver::Instance().PCI().configure_msi(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function, 1, &fIrq);
	NvHaikuDriver::Instance().PCI().enable_msi(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function);
	install_io_interrupt_handler(fIrq, InterruptHandler, (void *)this, 0);

	if (!rm_init_adapter(nullptr, nv))
		return B_IO_ERROR;

	fInitHardware = true;

	return B_OK;
}

status_t NvHaikuDevice::Stop()
{
	dprintf("NvHaikuDevice()::Stop()\n");
	nv_state_t *nv = Nv();

	if (fInitHardware) {
		rm_shutdown_adapter(nullptr, nv);
		fInitHardware = false;
	}

	if (fIrq != 0) {
		remove_io_interrupt_handler(fIrq, InterruptHandler, (void *)this);

		NvHaikuDriver::Instance().PCI().disable_msi(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function);
		NvHaikuDriver::Instance().PCI().unconfigure_msi(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function);
	}

	NvU32 pciConfig = 0;
	os_pci_read_dword(nv->handle, PCI_command, &pciConfig);
	pciConfig &= ~(uint32)PCI_command_master;
	os_pci_write_dword(nv->handle, PCI_command, pciConfig);

	return B_OK;
}

status_t NvHaikuDevice::AcquireRef()
{
	if (fOpenCount.fetch_add(1, std::memory_order_seq_cst) == 0) {
		status_t res = Start();
		if (res < B_OK) {
			fOpenCount--;
			return res;
		}
	}
	return B_OK;
}

void NvHaikuDevice::ReleaseRef()
{
	if (fOpenCount.fetch_sub(1, std::memory_order_seq_cst) == 1) {
		Stop();
	}
}

status_t NvHaikuDevice::Open(uint32 flags, DevfsNodeHandle &handle)
{
//	dprintf("NvHaikuDevice()::Open(%#" B_PRIx32 ")\n", flags);

	ObjectDeleter<NvHaikuDeviceHandle> handleImpl(new (std::nothrow) NvHaikuDeviceHandle(this));
	if (!handleImpl.IsSet())
		return B_NO_MEMORY;

	CHECK_RET(AcquireRef());

	handle = DevfsNodeHandle(nullptr, handleImpl.Detach());

	return B_OK;
}

void NvHaikuDevice::Closed()
{
	ReleaseRef();
}

int32 NvHaikuDevice::InterruptHandler(void* arg)
{
	return static_cast<NvHaikuDevice*>(arg)->InterruptHandlerInt();
}

int32 NvHaikuDevice::InterruptHandlerInt()
{
	// dprintf("NvHaikuDevice::InterruptHandler()\n");

	nv_state_t *nv = Nv();

	NvU32 needBottomHalf = NV_FALSE;
	NvBool isIsrHandled = rm_isr(nullptr, nv, &needBottomHalf);
	if (isIsrHandled) {
		//dprintf("rm_isr(%d)\n", needBottomHalf);
		if (needBottomHalf)
			DPCQueue::DefaultQueue(B_URGENT_DISPLAY_PRIORITY)->Add(&fDeferredInterruptHandler);
	}

	return B_HANDLED_INTERRUPT;
}

void NvHaikuDevice::DeferredInterruptHandler::DoDPC(DPCQueue* queue)
{
	(void)queue;
//	dprintf("NvHaikuDevice::DeferredInterruptHandler()\n");
	nv_state_t *nv = Base().Nv();

	rm_isr_bh(nullptr, nv);
}


NvHaikuDeviceHandle::NvHaikuDeviceHandle(NvHaikuDevice *device):
	NvHaikuBaseDeviceHandle(device)
{
//	dprintf("+NvHaikuDeviceHandle\n");
}

NvHaikuDeviceHandle::~NvHaikuDeviceHandle()
{
//	dprintf("-NvHaikuDeviceHandle\n");
}

status_t NvHaikuDeviceHandle::Close()
{
//	dprintf("NvHaikuDeviceHandle::Close()\n");
	GetDevice()->Closed();
	return NvHaikuBaseDeviceHandle::Close();
}
