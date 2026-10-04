/*
 * Copyright 2022, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_GENERIC_MSI_H
#define _KERNEL_ARCH_GENERIC_MSI_H

#include <SupportDefs.h>


// An interrupt controller's translated device identity, supplied by the host
// bridge. This is not Haiku's enumeration-order PCI domain number.
typedef struct msi_requester {
	uint64 controller_address;
	uint32 device_id;
} msi_requester;


#ifdef __cplusplus

class MSIInterface {
public:
	virtual status_t AllocateVectors(
		uint32 count, uint32& startVector, uint64& address, uint32& data) = 0;
	virtual void FreeVectors(uint32 count, uint32 startVector) = 0;
	virtual status_t AllocateVectorsForDevice(const msi_requester* requester,
		uint32 count, uint32& startVector, uint64& address, uint32& data)
		{ return AllocateVectors(count, startVector, address, data); }
};


extern "C" {
void msi_set_interface(MSIInterface* interface);
#endif

bool		msi_supported();
status_t	msi_allocate_vectors(uint32 count, uint32 *startVector,
				uint64 *address, uint32 *data);
status_t	msi_allocate_vectors_for_device(const msi_requester* requester,
				uint32 count, uint32* startVector, uint64* address, uint32* data);
void		msi_free_vectors(uint32 count, uint32 startVector);

#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_ARCH_GENERIC_MSI_H
