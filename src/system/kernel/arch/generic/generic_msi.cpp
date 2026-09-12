/*
 * Copyright 2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <arch/generic/msi.h>


MSIInterface* sMSIInterface;


void
msi_set_interface(MSIInterface* interface)
{
	sMSIInterface = interface;
}


bool
msi_supported()
{
	return sMSIInterface != NULL;
}


status_t
msi_allocate_vectors(uint32 count, uint32 *startVector, uint64 *address, uint32 *data)
{
	return sMSIInterface->AllocateVectors(count, *startVector, *address, *data);
}


void
msi_free_vectors(uint32 count, uint32 startVector)
{
	sMSIInterface->FreeVectors(count, startVector);
}


status_t
msi_allocate_vectors_for_device(const msi_requester* requester, uint32 count,
	uint32* startVector, uint64* address, uint32* data)
{
	if (sMSIInterface == NULL)
		return B_NOT_SUPPORTED;
	if (startVector == NULL || address == NULL || data == NULL)
		return B_BAD_VALUE;
	return sMSIInterface->AllocateVectorsForDevice(requester, count,
		*startVector, *address, *data);
}
