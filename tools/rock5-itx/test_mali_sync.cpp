/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "test_mali_sync_os.h"
#include "test_mali_sync_kernel.inc"
#include "test_mali_sync_calls.inc"
#include <cstdio>

static void
DescriptorLifetime()
{
	io_context* context = get_current_io_context(false);
	unsigned callbacks = 0;
	fd_ops ops = {};
	ops.fd_free = [](file_descriptor* descriptor) {
		// The last anonymous FD may own the only module reference. It must
		// remain loaded through the entire callback, including its return.
		assert(sModuleReferences == 1); (*(unsigned*)descriptor->cookie)++;
	};
	file_descriptor* descriptor = alloc_fd(); assert(descriptor != NULL);
	assert(descriptor->ops == NULL && descriptor->module == NULL);
	descriptor->ops = &ops; descriptor->cookie = &callbacks;
	assert(fd_hold_module(descriptor, sSyncTestModule.name) == B_OK);
	assert(fd_hold_module(descriptor, sSyncTestModule.name) == B_BAD_VALUE);
	int number = -1;
	sFailCopy = sCopies + 1;
	assert(new_fd_user(context, descriptor, O_CLOEXEC, &number) == B_BAD_ADDRESS);
	assert(number == -1 && context->num_used_fds == 0 && descriptor->open_count == 0);
	sFailCopy = 0;
	int fd = new_fd_user(context, descriptor, O_CLOEXEC, &number);
	assert(fd >= 0 && fd == number && fd_close_on_exec(context, fd));
	int alias = SyncDupFd(fd); assert(!fd_close_on_exec(context, alias));
	file_descriptor* inUse = get_fd(context, alias);
	SyncCloseFd(fd); SyncCloseFd(alias);
	assert(callbacks == 0 && sModuleReferences == 1 && sDescriptorCount == 1);
	put_fd(inUse);
	assert(callbacks == 1 && sModuleReferences == 0 && sDescriptorCount == 0);
	// Disconnection frees the payload once, retains the inert descriptor and
	// owner until its last descriptor reference goes away.
	descriptor = alloc_fd(); descriptor->ops = &ops; descriptor->cookie = &callbacks;
	assert(fd_hold_module(descriptor, sSyncTestModule.name) == B_OK);
	fd = new_fd_flags(context, descriptor, O_CLOFORK);
	assert(fd >= 0 && fd_close_on_fork(context, fd));
	inUse = get_fd(context, fd); descriptor->open_mode |= O_DISCONNECTED;
	put_fd(inUse); assert(callbacks == 2 && descriptor->ops == NULL && sModuleReferences == 1);
	SyncCloseFd(fd); assert(callbacks == 2 && sModuleReferences == 0);
	SyncBalanced();
}

static void
RollbackAndBounds()
{
	void* client = SyncOpen();
	for (unsigned allocation = 1; allocation <= 3; allocation++) {
		SyncCreate request = {1, 1, 0, 0}; sSyncFailAllocation = sSyncAllocationCalls + allocation;
		assert(SyncCall(client, kCreateSync, request) == B_NO_MEMORY && request.handle == 0);
		sSyncFailAllocation = 0;
		assert(sSyncObjects == 0 && sSyncNodes == 0 && SyncQuery(client).clientHandles == 0);
	}
	for (unsigned copy = 1; copy <= 2; copy++) {
		SyncCreate request = {1, 1, 0, 0}; sFailCopy = sCopies + copy;
		assert(SyncCall(client, kCreateSync, request) == B_BAD_ADDRESS && request.handle == 0);
		sFailCopy = 0; assert(sSyncObjects == 0 && sSyncNodes == 0);
	}
	uint32 empty = SyncMake(client), signaled = SyncMake(client, true);
	sSyncFailAllocation = sSyncAllocationCalls + 1;
	assert(SyncWaitFor(client, {{signaled, 0, 0}}) == B_NO_MEMORY);
	sSyncFailAllocation = 0;
	assert(SyncWaitFor(client, {{empty, 0, 0}}) == B_BAD_VALUE);
	assert(SyncWaitFor(client, {{empty, 0, 0}}, kSyncWaitAvailable) == B_TIMED_OUT);
	assert(SyncWaitFor(client, {{signaled, 0, 0}, {empty, 0, 0}}, 0) == B_BAD_VALUE);
	assert(SyncWaitFor(client, {{signaled, 0, 0}}, 0x80) == B_BAD_VALUE);
	assert(SyncWaitFor(client, {{signaled, 0, 0}}, 0, -2) == B_BAD_VALUE);
	assert(SyncWaitFor(client, {{signaled, 0, 0}}, 0, INT64_MAX) == B_BAD_VALUE);
	assert(SyncUpdate(client, kSignalSync, {{empty, 0, 0}, {0, 0, 0}}) == B_BAD_VALUE);
	assert(SyncQuery(client, empty).flags == 0);
	void* readonly = SyncOpen(false); SyncCreate denied = {1, 0, 0, 0};
	assert(SyncCall(readonly, kCreateSync, denied) == B_NOT_ALLOWED);
	sSyncTestTeam = 2; assert(SyncCall(client, kCreateSync, denied) == B_NOT_ALLOWED); sSyncTestTeam = 1;
	FreeSyncClient(readonly);
	for (unsigned failure = 0; failure < 5; failure++) {
		SyncFd request = {1, signaled, -1, 0, 0, 0};
		sFailDescriptor = failure == 0; sFailModule = failure == 1;
		if (failure == 2) sSyncFailAllocation = sSyncAllocationCalls + 1;
		if (failure >= 3) sFailCopy = sCopies + failure - 2;
		assert(SyncCall(client, kExportSync, request) != B_OK && request.fd == -1);
		sFailDescriptor = sFailModule = false; sSyncFailAllocation = sFailCopy = 0;
		assert(sSyncExports == 0 && sDescriptorCount == 0 && sModuleReferences == 0);
	}
	int fd = SyncExport(client, signaled);
	SyncFd imported = {1, 0, fd, 0, 0, 0}; sFailCopy = sCopies + 2;
	assert(SyncCall(client, kImportSync, imported) == B_BAD_ADDRESS && imported.handle == 0);
	sFailCopy = 0;
	std::vector<int> aliases;
	for (unsigned i = 1; i < 128; i++) aliases.push_back(SyncDupFd(fd));
	SyncFd full = {1, signaled, -1, 0, 0, 0};
	assert(SyncCall(client, kExportSync, full) == B_NO_MORE_FDS && full.fd == -1);
	assert(sSyncExports == 1 && sDescriptorCount == 1 && sModuleReferences == 1);
	for (int alias : aliases) SyncCloseFd(alias);
	uint32 importedHandle = SyncImport(client, fd);
	assert(SyncUpdate(client, kResetSync, {{signaled, 0, 0}, {importedHandle, 0, 0}}) == B_BAD_VALUE);
	assert(SyncQuery(client, signaled).flags == (kSyncAvailable | kSyncSignaled));
	SyncCloseFd(fd);
	std::vector<uint32> handles;
	for (unsigned i = 3; i < kMaxSyncHandles; i++) handles.push_back(SyncMake(client));
	SyncCreate exhausted = {1, 0, 0, 0};
	assert(SyncCall(client, kCreateSync, exhausted) == B_NO_MEMORY);
	assert(SyncQuery(client).clientHandles == kMaxSyncHandles);
	FreeSyncClient(client); SyncBalanced();
}

static void
SnapshotsAndSharing()
{
	void* a = SyncOpen(); uint32 handle = SyncMake(a);
	int shared = SyncExport(a, handle);
	void* b = SyncOpen(); uint32 alias = SyncImport(b, shared);
	SyncSubmission original = SyncEnqueue(a, {}, {{handle, 0, 0}});
	int snapshot = SyncExport(a, handle, true);
	file_descriptor* token = get_fd(get_current_io_context(false), snapshot);
	assert(token->ops->fd_select != NULL && token->ops->fd_select(token, 1, NULL) == B_NOT_SUPPORTED);
	put_fd(token);
	uint32 copied = SyncMake(b); SyncImportSnapshot(b, copied, snapshot);
	assert(SyncUpdate(a, kResetSync, {{handle, 0, 0}}) == B_OK);
	assert(SyncQuery(b, alias).flags == 0);
	assert(SyncUpdate(a, kSignalSync, {{handle, 0, 0}}) == B_OK);
	assert(SyncWaitFor(b, {{alias, 0, 0}}) == B_OK);
	assert(SyncWaitFor(b, {{copied, 0, 0}}) == B_TIMED_OUT && SyncFdWait(snapshot) == B_TIMED_OUT);
	SyncDestroy(a, handle); FreeSyncClient(a);
	FinishSyncSubmission(original, B_OK);
	assert(SyncWaitFor(b, {{copied, 0, 0}}) == B_OK && SyncFdWait(snapshot) == B_OK);
	int duplicate = SyncDupFd(shared); SyncCloseFd(shared);
	// Simulate a forked descriptor table using production reference/publication.
	file_descriptor* inherited = get_fd(get_current_io_context(false), duplicate);
	sSyncTestTeam = 2;
	int childFd = new_fd_flags(get_current_io_context(false), inherited, 0); assert(childFd >= 0);
	void* child = SyncOpen(); uint32 childHandle = SyncImport(child, childFd);
	assert(SyncWaitFor(child, {{childHandle, 0, 0}}) == B_OK);
	assert(SyncUpdate(child, kResetSync, {{childHandle, 0, 0}}) == B_OK);
	FreeSyncClient(child); SyncCloseFd(childFd); sSyncTestTeam = 1;
	assert(SyncQuery(b, alias).flags == 0 && SyncFdWait(snapshot) == B_OK);
	FreeSyncClient(b); // only exported FDs retain these objects now
	assert(sSyncObjects == 1 && sModuleReferences == 2);
	SyncCloseFd(duplicate); SyncCloseFd(snapshot); SyncBalanced();
}

static void
TimelineAndDag()
{
	void* client = SyncOpen(); uint32 timeline = SyncMake(client);
	SyncSubmission ten = SyncEnqueue(client, {}, {{timeline, 0, 10}});
	SyncSubmission twenty = SyncEnqueue(client, {}, {{timeline, 0, 20}});
	SyncSubmission thirty = SyncEnqueue(client, {}, {{timeline, 0, 30}});
	assert(SyncWaitFor(client, {{timeline, 0, 30}}, kSyncWaitAvailable) == B_OK);
	assert(SyncWaitFor(client, {{timeline, 0, 31}}, kSyncWaitAvailable) == B_TIMED_OUT);
	FinishSyncSubmission(twenty, B_OK);
	assert(SyncWaitFor(client, {{timeline, 0, 20}}) == B_TIMED_OUT);
	FinishSyncSubmission(ten, B_OK);
	assert(SyncWaitFor(client, {{timeline, 0, 1}, {timeline, 0, 15}}) == B_OK);
	SyncInfo info = SyncQuery(client, timeline); assert(info.submitted == 30 && info.completed == 20);
	assert(SyncUpdate(client, kSignalSync, {{timeline, 0, 20}}) == B_BAD_VALUE);
	int snapshot = SyncExport(client, timeline, true, 15); assert(SyncFdWait(snapshot) == B_OK);
	FinishSyncSubmission(thirty, B_CANCELED);
	assert(SyncUpdate(client, kSignalSync, {{timeline, 0, 40}}) == B_OK);
	status_t result;
	assert(SyncWaitFor(client, {{timeline, 0, 40}}, kSyncWaitAll, 0, NULL, &result) == B_OK
		&& result == B_CANCELED);
	assert(SyncWaitFor(client, {{timeline, 0, 15}}, kSyncWaitAll, 0, NULL, &result) == B_OK && result == B_OK);
	SyncCloseFd(snapshot); SyncDestroy(client, timeline);
	uint32 source = SyncMake(client), destination = SyncMake(client);
	SyncSubmission pending = SyncEnqueue(client, {}, {{source, 0, 0}});
	// Repeated overlapping DAG branches exercise memoized, bounded traversal.
	for (unsigned i = 1; i < 120; i++) {
		SyncTransfer transfer = {1, 0, {source, 0, 0}, {destination, 0, i}, 0};
		assert(SyncCall(client, kTransferSync, transfer) == B_OK);
		transfer = {1, 0, {destination, 0, i}, {source, 0, 0}, 0};
		assert(SyncCall(client, kTransferSync, transfer) == B_OK);
	}
	assert(SyncWaitFor(client, {{destination, 0, 119}}) == B_TIMED_OUT);
	FinishSyncSubmission(pending, B_OK);
	assert(SyncWaitFor(client, {{source, 0, 0}, {destination, 0, 119}}) == B_OK);
	// Serial timelines must prune completed history instead of reaching quota.
	for (uint64 i = 120; i < 1400; i++) {
		SyncSubmission job = SyncEnqueue(client, {}, {{destination, 0, i}});
		FinishSyncSubmission(job, B_OK);
	}
	assert(SyncQuery(client, destination).completed == 1399);
	FreeSyncClient(client); SyncBalanced();
}

static void
ConcurrentWaitAndSubmissionRollback()
{
	void* client = SyncOpen(); uint32 source = SyncMake(client), output = SyncMake(client);
	std::atomic<status_t> completed{123}, available{123};
	std::thread completion([&]() { completed = SyncWaitFor(client, {{source, 0, 0}},
		kSyncWaitForSubmit, 2000000); });
	std::thread availability([&]() { available = SyncWaitFor(client, {{source, 0, 0}},
		kSyncWaitAvailable, 2000000); });
	SyncAwait([]() { return sConditionWaiters == 2; });
	SyncSubmission job = SyncEnqueue(client, {}, {{source, 0, 0}});
	// Replace immediately, before either thread needs to be scheduled. The
	// publication callback must already have captured the pending old fence.
	assert(SyncUpdate(client, kResetSync, {{source, 0, 0}}) == B_OK);
	assert(SyncUpdate(client, kSignalSync, {{source, 0, 0}}) == B_OK);
	availability.join(); assert(available == B_OK && completed == 123);
	FinishSyncSubmission(job, B_OK); completion.join(); assert(completed == B_OK);
	SyncSubmission waitJob = SyncEnqueue(client, {}, {{source, 0, 0}});
	SyncPoint points[] = {{source, 0, 0}, {output, 0, 5}};
	for (unsigned failure = 1; failure <= 3; failure++) {
		SyncSubmission failed = {}; sSyncFailAllocation = sSyncAllocationCalls + failure;
		assert(PrepareSyncSubmission(client, points, 1, 1, failed) == B_NO_MEMORY && failed.state == NULL);
		sSyncFailAllocation = 0; assert(SyncQuery(client, output).flags == 0);
	}
	SyncSubmission aborted = {};
	assert(PrepareSyncSubmission(client, points, 1, 1, aborted) == B_OK);
	AbortSyncSubmission(aborted); assert(aborted.state == NULL && SyncQuery(client, output).flags == 0);
	SyncSubmission dependent = SyncEnqueue(client, {{source, 0, 0}}, {{output, 0, 5}});
	assert(ReadySyncSubmission(dependent) == B_WOULD_BLOCK);
	sInterruptWait = true;
	assert(SyncWaitFor(client, {{output, 0, 5}}, kSyncWaitAll, 100000) == B_INTERRUPTED);
	assert(SyncWaitFor(client, {{output, 0, 5}}, kSyncWaitAll, 1000) == B_TIMED_OUT);
	FinishSyncSubmission(waitJob, B_CANCELED);
	assert(ReadySyncSubmission(dependent) == B_CANCELED);
	FinishSyncSubmission(dependent, B_CANCELED);
	uint32 signaled = SyncMake(client, true), first = 99; status_t result;
	assert(SyncWaitFor(client, {{output, 0, 5}, {signaled, 0, 0}}, 0, 0, &first, &result) == B_OK
		&& first == 0 && result == B_CANCELED);
	assert(SyncUpdate(client, kResetSync, {{source, 0, 0}}) == B_OK);
	completed = 123;
	std::thread closing([&]() { completed = SyncWaitFor(client, {{source, 0, 0}},
		kSyncWaitForSubmit, -1); });
	SyncAwait([]() { return sConditionWaiters == 1; });
	CloseSyncClient(client); closing.join(); assert(completed == B_CANCELED);
	FreeSyncClient(client); SyncBalanced();
}

int main()
{
	DescriptorLifetime(); RollbackAndBounds(); SnapshotsAndSharing(); TimelineAndDag();
	ConcurrentWaitAndSubmissionRollback();
	puts("MALI_CSF_SYNC_TEST_PASS");
}
