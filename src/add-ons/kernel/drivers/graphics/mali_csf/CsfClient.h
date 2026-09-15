/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_CLIENT_H
#define MALI_CSF_CLIENT_H

#include "CsfQueue.h"

namespace MaliCSF {
status_t OpenClient(bool writable, void** cookie);
status_t AccessClient(void* cookie);
void CloseClient(void* cookie);
void FreeClient(void* cookie);
status_t ControlClient(void* cookie, uint32 op, void* buffer, size_t length);

// Kernel-only ownership for scheduled work. A lease pins an immutable root and
// every buffer mapped by that generation, independently of handles/fd lifetime.
// Start with a zeroed lease; release only after hardware no longer uses the root.
struct ClientVmLease {
	void* state;
	uint64_t rootPhysical;
	uint64_t generation;
	uint64_t userLimit;
};
status_t AcquireClientVm(void* cookie, uint32 handle, uint64_t expectedGeneration,
	ClientVmLease& lease);
void ReleaseClientVm(ClientVmLease& lease);
// The caller must own the lease for the whole access. Neither helper activates
// hardware or changes ownership. Range checks permit command streams crossing
// adjacent mappings, but reject holes and addresses outside the user VA range.
const uint64_t* ClientVmRoot(const ClientVmLease& lease);
bool ClientVmRange(const ClientVmLease& lease, uint64_t address, uint64_t bytes);
}
#endif
