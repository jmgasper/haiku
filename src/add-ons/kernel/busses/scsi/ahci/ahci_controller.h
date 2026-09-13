/*
 * Copyright 2007, Marcus Overhagen. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _AHCI_CONTROLLER_H
#define _AHCI_CONTROLLER_H


#include "ahci_defs.h"
#include "ahci_port.h"
#include <bus/PCIInterrupts.h>


class AHCIController {
public:
							AHCIController(device_node *node,
								pci_device_module_info *pciModule,
								pci_device *pciDevice);
							~AHCIController();

			status_t		Init();
			void			Uninit();

			void			ExecuteRequest(scsi_ccb *request);
			uchar			AbortRequest(scsi_ccb *request);
			uchar			TerminateRequest(scsi_ccb *request);
			uchar			ResetDevice(uchar targetID, uchar targetLUN);
			void			GetRestrictions(uchar targetID, bool *isATAPI,
								bool *noAutoSense, uint32 *maxBlocks);

			device_node *	DeviceNode() { return fNode; }

private:
			bool			IsDevicePresent(uint device);
			status_t		ResetController();
			status_t		ConfigureInterrupts(const pci_info& info);
			void			FlushPostedWrites();

	static	int32			Interrupt(void *data);

	friend class AHCIPort;

private:
	device_node *			fNode;
	pci_device_module_info *fPCI;
	pci_device *			fPCIDevice;
	uint16					fPCIVendorID;
	uint16					fPCIDeviceID;
	uint32					fFlags;
	pci_intx_module_info*	fIntx;
	uint8					fBus;
	uint8					fDevice;
	uint8					fFunction;
	bool					fPCIEnabled;

	volatile ahci_hba *		fRegs;
	area_id					fRegsArea;
	int						fCommandSlotCount;
	int						fPortCount;
	uint32					fPortImplementedMask;
	uint32					fIRQ;
	bool					fMSIConfigured;
	bool					fInterruptInstalled;
	AHCIPort *				fPort[32];

// --- Instance check workaround begin
	port_id fInstanceCheck;
// --- Instance check workaround end

};


inline void
AHCIController::FlushPostedWrites()
{
	memory_full_barrier();
	volatile uint32 dummy = fRegs->ghc;
	dummy = dummy;
	memory_full_barrier();
}

#endif	// _AHCI_CONTROLLER_H
