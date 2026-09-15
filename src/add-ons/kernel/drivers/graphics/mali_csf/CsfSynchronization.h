/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_SYNCHRONIZATION_H
#define MALI_CSF_SYNCHRONIZATION_H

#include "CsfSync.h"

namespace MaliCSF {
status_t OpenSyncClient(bool writable, void** client);
void CloseSyncClient(void* client);
void FreeSyncClient(void* client);
status_t ControlSync(void* client, uint32 op, void* user, size_t length);

struct SyncSubmission { void* state; };
// Called under the runtime lock. Success keeps the sync lock until Commit or
// Abort; callers perform only the checked submission copyout in between.
status_t PrepareSyncSubmission(void* client, const SyncPoint* points,
	uint32 waits, uint32 signals, SyncSubmission& submission);
void CommitSyncSubmission(SyncSubmission& submission);
void AbortSyncSubmission(SyncSubmission& submission);
// Never acquire the runtime lock from any synchronization operation.
status_t ReadySyncSubmission(const SyncSubmission& submission);
void FinishSyncSubmission(SyncSubmission& submission, status_t result);
}
#endif
