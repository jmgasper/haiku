// SPDX-License-Identifier: MIT
// Read-only view of the UEFI 2.10 PCI I/O protocol, through GetLocation.
// Unused function slots retain their ABI positions and are never called.
#pragma once

#include <efi/protocol/pci-root-bridge-io.h>

#define ROCK5_EFI_PCI_IO_PROTOCOL_GUID \
	{0x4cf5b200, 0x68b8, 0x4ca5, {0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a}}

using efi_pci_unused_function = void (*)(void);

struct efi_pci_io_read_view {
	efi_pci_unused_function PollMem;
	efi_pci_unused_function PollIo;
	struct {
		efi_pci_unused_function Read;
		efi_pci_unused_function Write;
	} Mem, Io;
	struct {
		efi_status (*Read)(efi_pci_io_read_view* self,
			efi_pci_root_bridge_io_width width, uint32_t offset,
			size_t count, void* buffer) EFIAPI;
		efi_pci_unused_function Write;
	} Pci;
	efi_pci_unused_function CopyMem;
	efi_pci_unused_function Map;
	efi_pci_unused_function Unmap;
	efi_pci_unused_function AllocateBuffer;
	efi_pci_unused_function FreeBuffer;
	efi_pci_unused_function Flush;
	efi_status (*GetLocation)(efi_pci_io_read_view* self, size_t* segment,
		size_t* bus, size_t* device, size_t* function) EFIAPI;
};

static_assert(sizeof(void*) == 8, "This diagnostic targets 64-bit UEFI");
static_assert(__builtin_offsetof(efi_pci_io_read_view, Pci) == 48,
	"UEFI PCI config access offset");
static_assert(__builtin_offsetof(efi_pci_io_read_view, GetLocation) == 112,
	"UEFI PCI location access offset");
