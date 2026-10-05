/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _DRIVERS_BUS_PCI_INTERRUPTS_H
#define _DRIVERS_BUS_PCI_INTERRUPTS_H

#include <module.h>


// An optional interface for platforms whose INTx vectors do not fit the PCI
// configuration-space interrupt-line byte. Existing PCI module ABIs are unchanged.
#define B_PCI_INTX_MODULE_NAME "bus_managers/pci/intx/v1"
#define B_PCI_INTX_CONTROLLER_MODULE "pci/intx/controller_module"

typedef struct pci_intx_module_info {
	module_info info;
	status_t (*get_irq)(uint8 bus, uint8 device, uint8 function, uint32* irq);
	// The driver serializes these calls with its MSI operations. Install the
	// handler before enabling, and disable before removing the handler.
	status_t (*set_enabled)(uint8 bus, uint8 device, uint8 function, bool enabled);
} pci_intx_module_info;


// A host controller advertises this module by a string attribute named
// B_PCI_INTX_CONTROLLER_MODULE. The cookie and BDF belong to that controller;
// the public interface above uses the legacy PCI module's virtual bus numbers.
// An advertised provider's error must never fall back to the 8-bit line value.
typedef struct pci_intx_controller_module_info {
	module_info info;
	status_t (*get_irq)(void* cookie, uint8 bus, uint8 device, uint8 function,
		uint8 pin, uint32* irq);
	status_t (*set_enabled)(void* cookie, uint8 bus, uint8 device, uint8 function,
		uint8 pin, bool enabled);
} pci_intx_controller_module_info;

#endif
