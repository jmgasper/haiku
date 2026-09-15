/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <KernelExport.h>
#include <condition_variable.h>
#include <fs/fd.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>

#include "CsfSynchronization.h"

using namespace MaliCSF;

static mutex sSyncLock = MUTEX_INITIALIZER("Mali CSF synchronization");
static ConditionVariable sSyncChanged;
static bool sSyncInitialized;
static uint32 sSyncClients, sSyncObjects, sSyncNodes, sSyncEvents, sSyncExports, sSyncWaiters;
static uint32 sNextSyncHandle = 1;
static uint64 sSyncCheckSerial;
static const uint32 kMaxSyncObjects = 4096;
static const uint32 kMaxSyncNodes = 16384;
static const uint32 kMaxSyncEvents = 4096;
static const uint32 kMaxSyncDepth = 256;

struct SyncEvent {
	uint32 references;
	bool done;
	status_t result;
};

// Each node is an immutable fence snapshot. Only completed-history pruning and
// cached observations change after publication, under sSyncLock. Dependencies
// always refer to older nodes; depth and allocation quotas bound traversal.
struct SyncNode {
	uint32 references, depth;
	uint64 value, completedBefore;
	SyncEvent* event;
	SyncNode* previous;
	SyncNode* dependency;
	SyncNode* garbageNext;
	status_t priorError;
	bool done;
	status_t result;
	uint64 checked;
};

struct SyncObject { uint32 references; SyncNode* head; };
struct SyncEntry { SyncEntry* next; uint32 handle; SyncObject* object; };
struct SyncClient {
	team_id owner;
	bool writable, closed;
	uint32 count;
	SyncEntry* first;
};
struct SyncToken { SyncNode* node; status_t immediate; bool captured; };
struct SyncMutation { SyncObject* object; SyncNode* node; uint64 value; };
struct SyncTransaction {
	SyncToken waits[kMaxSyncPoints];
	SyncMutation signals[kMaxSyncPoints];
	uint32 waitCount, signalCount;
	SyncEvent* event;
	bool committed;
};
struct SyncDescriptor { SyncObject* object; SyncToken snapshot; };
struct SyncPendingCapture {
	SyncPendingCapture* next;
	SyncObject* object;
	uint64 value;
	SyncToken* token;
};
static SyncPendingCapture* sSyncPendingCaptures;

static void
PutSyncEvent(SyncEvent* event)
{
	if (event != NULL && --event->references == 0) { sSyncEvents--; free(event); }
}

static void
PutSyncNode(SyncNode* node)
{
	if (node == NULL || --node->references != 0) return;
	// An intrusive list of zero-reference nodes avoids recursion on a DAG.
	node->garbageNext = NULL;
	while (node != NULL) {
		SyncNode* next = node->garbageNext;
		SyncNode* children[] = {node->previous, node->dependency};
		for (unsigned i = 0; i < 2; i++) {
			SyncNode* child = children[i];
			if (child != NULL && --child->references == 0) {
				child->garbageNext = next; next = child;
			}
		}
		PutSyncEvent(node->event);
		sSyncNodes--; free(node); node = next;
	}
}

static status_t
SyncNodeStatus(SyncNode* node)
{
	if (node == NULL) return B_OK;
	if (node->done) return node->result;
	uint64 serial = ++sSyncCheckSerial;
	SyncNode* stack[kMaxSyncDepth];
	unsigned count = 1;
	stack[0] = node;
	while (count != 0) {
		SyncNode* current = stack[count - 1];
		SyncNode* child = current->previous;
		if (child == NULL || child->done || child->checked == serial)
			child = current->dependency;
		if (child != NULL && !child->done && child->checked != serial) {
			ASSERT(count < kMaxSyncDepth);
			stack[count++] = child;
			continue;
		}
		status_t own = current->event == NULL ? B_OK
			: !current->event->done ? B_WOULD_BLOCK : current->event->result;
		status_t before = current->previous == NULL ? B_OK : current->previous->result;
		status_t dependency = current->dependency == NULL ? B_OK : current->dependency->result;
		current->result = own == B_WOULD_BLOCK || before == B_WOULD_BLOCK
			|| dependency == B_WOULD_BLOCK ? B_WOULD_BLOCK
			: own != B_OK ? own : current->priorError != B_OK ? current->priorError
			: before != B_OK ? before : dependency;
		current->done = current->result != B_WOULD_BLOCK;
		current->checked = serial;
		count--;
	}
	return node->result;
}

static void
CompactSyncNode(SyncNode* node)
{
	if (node == NULL) return;
	status_t status = SyncNodeStatus(node->previous);
	// Keep failed history so looking up a point before the failed submission
	// still returns its own result. Successful history needs only its frontier.
	if (node->previous != NULL && status == B_OK) {
		SyncNode* previous = node->previous;
		node->completedBefore = previous->value;
		if (node->priorError == B_OK) node->priorError = status;
		node->previous = NULL; PutSyncNode(previous);
	}
	status = SyncNodeStatus(node->dependency);
	if (node->dependency != NULL && status != B_WOULD_BLOCK) {
		if (node->priorError == B_OK) node->priorError = status;
		SyncNode* dependency = node->dependency;
		node->dependency = NULL; PutSyncNode(dependency);
	}
	node->depth = 1;
	if (node->previous != NULL) node->depth = node->previous->depth + 1;
	if (node->dependency != NULL && node->dependency->depth + 1 > node->depth)
		node->depth = node->dependency->depth + 1;
}

static SyncNode*
NewSyncNode(uint64 value, SyncEvent* event, SyncNode* previous,
	SyncNode* dependency, status_t immediate = B_OK)
{
	CompactSyncNode(previous);
	CompactSyncNode(dependency);
	uint32 depth = 1;
	if (previous != NULL) depth = previous->depth + 1;
	if (dependency != NULL && dependency->depth + 1 > depth) depth = dependency->depth + 1;
	if (sSyncNodes == kMaxSyncNodes || depth > kMaxSyncDepth) return NULL;
	SyncNode* node = (SyncNode*)calloc(1, sizeof(SyncNode));
	if (node == NULL) return NULL;
	node->references = 1; node->depth = depth; node->value = value;
	node->event = event; node->previous = previous; node->dependency = dependency;
	node->priorError = immediate;
	if (event != NULL) event->references++;
	if (previous != NULL) previous->references++;
	if (dependency != NULL) dependency->references++;
	sSyncNodes++;
	return node;
}

static void
PutSyncObject(SyncObject* object)
{
	if (object != NULL && --object->references == 0) {
		PutSyncNode(object->head); sSyncObjects--; free(object);
	}
}

static void
PutSyncToken(SyncToken& token)
{
	PutSyncNode(token.node); token = {};
}

static bool
CaptureSyncPoint(SyncObject* object, uint64 value, SyncToken& token)
{
	SyncNode* node = object->head;
	if (node == NULL || (value != 0 && value > node->value)) return false;
	for (;;) {
		CompactSyncNode(node);
		if (value == 0) break;
		if (value <= node->completedBefore) {
			token = {NULL, B_OK, true}; return true;
		}
		if (node->previous == NULL || node->previous->value < value) break;
		node = node->previous;
	}
	node->references++;
	token = {node, B_OK, true};
	return true;
}

static status_t
SyncTokenStatus(const SyncToken& token)
{
	return token.node == NULL ? token.immediate : SyncNodeStatus(token.node);
}

// Capture at publication, while holding the same lock as replacement. Merely
// waking a waiter could miss a short-lived fence replaced before it runs.
static void
NotifySyncChanged()
{
	for (SyncPendingCapture* pending = sSyncPendingCaptures; pending != NULL; pending = pending->next)
		if (!pending->token->captured) CaptureSyncPoint(pending->object, pending->value, *pending->token);
	sSyncChanged.NotifyAll();
}

class SyncWaitRegistration {
public:
	SyncWaitRegistration(SyncObject** objects, const uint64* values, SyncToken* tokens, uint32 count)
	{
		if (sSyncWaiters == kMaxSyncObjects) return;
		entries = (SyncPendingCapture*)calloc(count, sizeof(SyncPendingCapture));
		if (entries == NULL) return;
		sSyncWaiters++;
		for (uint32 i = 0; i < count; i++) {
			if (objects[i] == NULL || tokens[i].captured) continue;
			SyncPendingCapture& entry = entries[used++];
			entry = {sSyncPendingCaptures, objects[i], values[i], &tokens[i]};
			sSyncPendingCaptures = &entry;
		}
	}
	~SyncWaitRegistration()
	{
		if (entries == NULL) return;
		sSyncWaiters--;
		for (uint32 i = 0; i < used; i++) {
			SyncPendingCapture** link = &sSyncPendingCaptures;
			while (*link != &entries[i]) link = &(*link)->next;
			*link = entries[i].next;
		}
		free(entries);
	}
	bool IsValid() const { return entries != NULL; }
private:
	uint32 used = 0;
	SyncPendingCapture* entries = NULL;
};

static status_t
CheckSyncClient(SyncClient* client, bool write = true)
{
	if (client == NULL || client->closed || client->owner != team_get_current_team_id()
		|| (write && !client->writable)) return B_NOT_ALLOWED;
	return B_OK;
}

static SyncEntry**
FindSyncEntry(SyncClient* client, uint32 handle)
{
	SyncEntry** entry = &client->first;
	while (*entry != NULL && (*entry)->handle != handle) entry = &(*entry)->next;
	return entry;
}

static SyncObject*
FindSyncObject(SyncClient* client, const SyncPoint& point)
{
	if (point.flags != 0) return NULL;
	SyncEntry* entry = *FindSyncEntry(client, point.handle);
	return entry == NULL ? NULL : entry->object;
}

static void
DeleteSyncEntry(SyncClient* client, SyncEntry** link)
{
	SyncEntry* entry = *link; *link = entry->next;
	PutSyncObject(entry->object); free(entry); client->count--;
}

static status_t
AddSyncEntry(SyncClient* client, SyncObject* object, void* userHandle)
{
	if (client->count == kMaxSyncHandles || sNextSyncHandle == 0) return B_NO_MEMORY;
	SyncEntry* entry = (SyncEntry*)calloc(1, sizeof(SyncEntry));
	if (entry == NULL) return B_NO_MEMORY;
	entry->handle = sNextSyncHandle++;
	status_t status = user_memcpy(userHandle, &entry->handle, sizeof(entry->handle));
	if (status != B_OK) { free(entry); return status; }
	entry->object = object; object->references++;
	entry->next = client->first; client->first = entry; client->count++;
	return B_OK;
}

template<typename T>
static status_t
ReadSyncRequest(void* user, size_t length, T& request, bool trailing = false)
{
	if (trailing ? length < sizeof(T) : length != sizeof(T)) return B_BAD_VALUE;
	if (user == NULL || user_memcpy(&request, user, sizeof(T)) != B_OK) return B_BAD_ADDRESS;
	return request.version == kClientVersion ? B_OK : B_BAD_VALUE;
}

static status_t
ReadSyncPoints(void* user, size_t length, size_t offset, uint32 count, SyncPoint* points)
{
	if (count == 0 || count > kMaxSyncPoints || length != offset + count * sizeof(SyncPoint))
		return B_BAD_VALUE;
	return user_memcpy(points, (uint8*)user + offset, count * sizeof(SyncPoint));
}

status_t
MaliCSF::OpenSyncClient(bool writable, void** cookie)
{
	SyncClient* client = (SyncClient*)calloc(1, sizeof(SyncClient));
	if (client == NULL) return B_NO_MEMORY;
	client->owner = team_get_current_team_id(); client->writable = writable;
	MutexLocker locker(sSyncLock);
	if (!sSyncInitialized) { sSyncChanged.Init(&sSyncChanged, "Mali CSF sync"); sSyncInitialized = true; }
	sSyncClients++; *cookie = client;
	return B_OK;
}

void
MaliCSF::CloseSyncClient(void* cookie)
{
	MutexLocker locker(sSyncLock);
	SyncClient* client = (SyncClient*)cookie;
	if (client->closed) return;
	client->closed = true;
	while (client->first != NULL) DeleteSyncEntry(client, &client->first);
	NotifySyncChanged();
}

void
MaliCSF::FreeSyncClient(void* cookie)
{
	CloseSyncClient(cookie);
	MutexLocker locker(sSyncLock);
	sSyncClients--; free(cookie);
}

static void
CommitSyncMutations(SyncMutation* mutations, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		SyncMutation& mutation = mutations[i];
		PutSyncNode(mutation.object->head);
		mutation.object->head = mutation.node; mutation.node = NULL;
	}
	NotifySyncChanged();
}

static void
DropSyncMutations(SyncMutation* mutations, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		PutSyncNode(mutations[i].node); PutSyncObject(mutations[i].object);
		mutations[i] = {};
	}
}

static status_t
PrepareSyncMutations(SyncClient* client, const SyncPoint* points, uint32 count,
	SyncMutation* mutations, SyncEvent* event, const SyncToken* dependency = NULL,
	bool reset = false)
{
	for (uint32 i = 0; i < count; i++) {
		SyncObject* object = FindSyncObject(client, points[i]);
		if (object == NULL) return B_BAD_VALUE;
		for (uint32 j = 0; j < i; j++) if (mutations[j].object == object) return B_BAD_VALUE;
		uint64 value = points[i].point;
		if (reset ? value != 0 : value != 0 && object->head != NULL && value <= object->head->value)
			return B_BAD_VALUE;
		mutations[i].object = object; object->references++; mutations[i].value = value;
		if (reset) continue;
		mutations[i].node = NewSyncNode(value, event, value != 0 ? object->head : NULL,
			dependency != NULL ? dependency->node : NULL,
			dependency != NULL ? dependency->immediate : B_OK);
		if (mutations[i].node == NULL) return B_NO_MEMORY;
	}
	return B_OK;
}

static status_t
UpdateSync(SyncClient* client, uint32 op, void* user, size_t length)
{
	SyncBatch request;
	status_t status = ReadSyncRequest(user, length, request, true);
	if (status != B_OK) return status;
	if (request.flags != 0 || request.reserved != 0) return B_BAD_VALUE;
	SyncPoint points[kMaxSyncPoints];
	status = ReadSyncPoints(user, length, sizeof(request), request.count, points);
	if (status != B_OK) return status;
	SyncMutation mutations[kMaxSyncPoints] = {};
	MutexLocker locker(sSyncLock);
	status = CheckSyncClient(client);
	if (status == B_OK) status = PrepareSyncMutations(client, points, request.count,
		mutations, NULL, NULL, op == kResetSync);
	if (status == B_OK) CommitSyncMutations(mutations, request.count);
	DropSyncMutations(mutations, request.count);
	return status;
}

static status_t
SyncDeadline(int64 timeout, bigtime_t& deadline)
{
	bigtime_t now = system_time();
	if (timeout < -1 || timeout > INT64_MAX - now) return B_BAD_VALUE;
	deadline = timeout < 0 ? 0 : now + timeout; return B_OK;
}

static status_t
WaitSyncTokens(SyncClient* client, SyncObject** objects, const uint64* values,
	SyncToken* tokens, uint32 count, uint32 flags, int64 timeout, uint32& first,
	status_t& result)
{
	if ((flags & ~(kSyncWaitAll | kSyncWaitForSubmit | kSyncWaitAvailable)) != 0)
		return B_BAD_VALUE;
	bigtime_t deadline;
	status_t status = SyncDeadline(timeout, deadline);
	if (status != B_OK) return status;
	SyncWaitRegistration registration(objects, values, tokens, count);
	if (!registration.IsValid()) return B_NO_MEMORY;
	for (;;) {
		bool all = true, any = false;
		status_t error = B_OK;
		for (uint32 i = 0; i < count; i++) {
			if (!tokens[i].captured && objects[i] != NULL)
				CaptureSyncPoint(objects[i], values[i], tokens[i]);
			if (!tokens[i].captured && !(flags & (kSyncWaitForSubmit | kSyncWaitAvailable)))
				return B_BAD_VALUE;
			status_t current = !tokens[i].captured ? B_WOULD_BLOCK
				: (flags & kSyncWaitAvailable) ? B_OK : SyncTokenStatus(tokens[i]);
			if (current == B_WOULD_BLOCK) all = false;
			else {
				if (!any) { first = i; result = current; }
				any = true;
				if (error == B_OK) error = current;
			}
		}
		if ((flags & kSyncWaitAll) ? all : any) {
			if (flags & kSyncWaitAll) { first = 0; result = error; }
			return B_OK;
		}
		if (client != NULL && client->closed) return B_CANCELED;
		if (timeout == 0) return B_TIMED_OUT;
		status = sSyncChanged.Wait(&sSyncLock, B_CAN_INTERRUPT
			| (timeout >= 0 ? B_ABSOLUTE_TIMEOUT : 0), deadline);
		if (status != B_OK) return status;
	}
}

static status_t
WaitSync(SyncClient* client, void* user, size_t length)
{
	SyncWait request;
	status_t status = ReadSyncRequest(user, length, request, true);
	if (status != B_OK) return status;
	if (request.first != 0 || request.result != 0 || request.reserved != 0) return B_BAD_VALUE;
	SyncPoint points[kMaxSyncPoints];
	status = ReadSyncPoints(user, length, sizeof(request), request.count, points);
	if (status != B_OK) return status;
	SyncObject* objects[kMaxSyncPoints] = {};
	SyncToken tokens[kMaxSyncPoints] = {};
	uint64 values[kMaxSyncPoints];
	MutexLocker locker(sSyncLock);
	status = CheckSyncClient(client);
	for (uint32 i = 0; status == B_OK && i < request.count; i++) {
		objects[i] = FindSyncObject(client, points[i]); values[i] = points[i].point;
		if (objects[i] == NULL) status = B_BAD_VALUE;
		else objects[i]->references++;
	}
	if (status == B_OK) status = WaitSyncTokens(client, objects, values, tokens, request.count,
		request.flags, request.timeoutMicros, request.first, request.result);
	if (status == B_OK) status = user_memcpy(user, &request, sizeof(request));
	for (uint32 i = 0; i < request.count; i++) { PutSyncToken(tokens[i]); PutSyncObject(objects[i]); }
	return status;
}

static status_t
SyncDescriptorClose(file_descriptor*) { return B_OK; }

static void
SyncDescriptorFree(file_descriptor* descriptor)
{
	MutexLocker locker(sSyncLock);
	SyncDescriptor* data = (SyncDescriptor*)descriptor->cookie;
	PutSyncObject(data->object); PutSyncToken(data->snapshot);
	sSyncExports--; free(data);
}

static status_t
SyncDescriptorControl(file_descriptor* descriptor, ulong op, void* user, size_t length)
{
	if (op != kWaitSyncFile) return B_DEV_INVALID_IOCTL;
	SyncFileWait request;
	status_t status = ReadSyncRequest(user, length, request);
	if (status != B_OK) return status;
	if (request.result != 0 || request.reserved != 0) return B_BAD_VALUE;
	SyncDescriptor* data = (SyncDescriptor*)descriptor->cookie;
	if (data->object == NULL && request.point != 0) return B_BAD_VALUE;
	MutexLocker locker(sSyncLock);
	SyncToken token = data->snapshot;
	if (token.node != NULL) token.node->references++;
	uint32 first = 0;
	status = WaitSyncTokens(NULL, &data->object, &request.point, &token, 1, request.flags,
		request.timeoutMicros, first, request.result);
	PutSyncToken(token);
	if (status == B_OK) status = user_memcpy(user, &request, sizeof(request));
	return status;
}

// Exported descriptors are importable tokens with an explicit wait ioctl.
// poll/select and inter-driver Linux sync_file interoperability are not exposed.
static status_t
SyncDescriptorSelect(file_descriptor*, uint8, selectsync*)
{
	// A NULL hook makes Haiku report ordinary files immediately ready. That
	// fallback must never make an unfinished fence appear complete.
	return B_NOT_SUPPORTED;
}

static fd_ops sSyncFDOps = {
	SyncDescriptorClose, SyncDescriptorFree, NULL, NULL, NULL, NULL, NULL,
	SyncDescriptorControl, NULL, SyncDescriptorSelect, NULL, NULL, NULL, NULL, NULL
};

static status_t
ExportSync(SyncClient* client, void* user, size_t length)
{
	SyncFd request;
	status_t status = ReadSyncRequest(user, length, request);
	if (status != B_OK) return status;
	if (request.fd != -1 || request.reserved != 0 || request.flags > kSyncSnapshotFd
		|| (request.flags == 0 && request.point != 0)) return B_BAD_VALUE;
	file_descriptor* descriptor = alloc_fd();
	if (descriptor == NULL) return B_NO_MEMORY;
	status = fd_hold_module(descriptor, "drivers/graphics/mali_csf/device_v1");
	if (status != B_OK) { put_fd(descriptor); return status; }
	{
		MutexLocker locker(sSyncLock);
		status = CheckSyncClient(client);
		SyncObject* object = status == B_OK
			? FindSyncObject(client, {request.handle, 0, request.point}) : NULL;
		if (status == B_OK && object == NULL) status = B_ENTRY_NOT_FOUND;
		SyncDescriptor* data = status == B_OK && sSyncExports < kMaxSyncObjects
			? (SyncDescriptor*)calloc(1, sizeof(SyncDescriptor)) : NULL;
		if (status == B_OK && data == NULL) status = B_NO_MEMORY;
		if (status == B_OK) {
			if (request.flags == kSyncSnapshotFd) {
				if (!CaptureSyncPoint(object, request.point, data->snapshot)) status = B_BAD_VALUE;
			} else { data->object = object; object->references++; }
			if (status == B_OK) {
				sSyncExports++; descriptor->cookie = data; descriptor->ops = &sSyncFDOps;
				descriptor->open_mode = O_RDWR | O_CLOEXEC;
			} else free(data);
		}
	}
	if (status == B_OK) {
		int fd = new_fd_user(get_current_io_context(false), descriptor, O_CLOEXEC,
			(int*)((uint8*)user + offsetof(SyncFd, fd)));
		if (fd >= B_OK) return B_OK; // published slot owns the descriptor reference
		status = fd;
	}
	put_fd(descriptor); return status;
}

static status_t
ImportSync(SyncClient* client, void* user, size_t length)
{
	SyncFd request;
	status_t status = ReadSyncRequest(user, length, request);
	if (status != B_OK) return status;
	if (request.fd < 0 || request.reserved != 0 || request.flags > kSyncSnapshotFd
		|| (request.flags == 0 && (request.point != 0 || request.handle != 0))) return B_BAD_VALUE;
	file_descriptor* descriptor = get_fd(get_current_io_context(false), request.fd);
	if (descriptor == NULL) return B_FILE_ERROR;
	if (descriptor->ops != &sSyncFDOps) { put_fd(descriptor); return B_BAD_VALUE; }
	{
		MutexLocker locker(sSyncLock);
		status = CheckSyncClient(client);
		SyncDescriptor* data = (SyncDescriptor*)descriptor->cookie;
		if (status == B_OK && (request.flags == 0 ? data->object == NULL : data->object != NULL))
			status = B_BAD_VALUE;
		if (status == B_OK && request.flags == 0) {
			status = AddSyncEntry(client, data->object, (uint8*)user + offsetof(SyncFd, handle));
		} else if (status == B_OK) {
			SyncPoint point = {request.handle, 0, request.point};
			SyncMutation mutation = {};
			status = PrepareSyncMutations(client, &point, 1, &mutation, NULL, &data->snapshot);
			if (status == B_OK) CommitSyncMutations(&mutation, 1);
			DropSyncMutations(&mutation, 1);
		}
	}
	put_fd(descriptor); return status;
}

status_t
MaliCSF::ControlSync(void* cookie, uint32 op, void* user, size_t length)
{
	SyncClient* client = (SyncClient*)cookie;
	if (op == kSignalSync || op == kResetSync) return UpdateSync(client, op, user, length);
	if (op == kWaitSync) return WaitSync(client, user, length);
	if (op == kExportSync) return ExportSync(client, user, length);
	if (op == kImportSync) return ImportSync(client, user, length);
	MutexLocker locker(sSyncLock);
	status_t status = CheckSyncClient(client, op != kGetSyncInfo);
	if (status != B_OK) return status;
	if (op == kCreateSync) {
		SyncCreate request;
		status = ReadSyncRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.flags > kSyncInitiallySignaled || request.handle != 0 || request.reserved != 0)
			return B_BAD_VALUE;
		if (sSyncObjects == kMaxSyncObjects) return B_NO_MEMORY;
		SyncObject* object = (SyncObject*)calloc(1, sizeof(SyncObject));
		if (object == NULL) return B_NO_MEMORY;
		object->references = 1; sSyncObjects++;
		if (request.flags != 0) {
			object->head = NewSyncNode(0, NULL, NULL, NULL);
			if (object->head == NULL) status = B_NO_MEMORY;
		}
		if (status == B_OK) status = AddSyncEntry(client, object,
			(uint8*)user + offsetof(SyncCreate, handle));
		PutSyncObject(object); return status;
	}
	if (op == kDestroySync) {
		SyncHandle request;
		status = ReadSyncRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.reserved != 0) return B_BAD_VALUE;
		SyncEntry** entry = FindSyncEntry(client, request.handle);
		if (*entry == NULL) return B_ENTRY_NOT_FOUND;
		DeleteSyncEntry(client, entry); return B_OK;
	}
	if (op == kGetSyncInfo) {
		SyncInfo request;
		status = ReadSyncRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.reserved != 0) return B_BAD_VALUE;
		SyncInfo info = {}; info.version = kClientVersion; info.handle = request.handle;
		if (request.handle != 0) {
			SyncObject* object = FindSyncObject(client, {request.handle, 0, 0});
			if (object == NULL) return B_ENTRY_NOT_FOUND;
			SyncNode* node = object->head;
			if (node != NULL) {
				CompactSyncNode(node); info.submitted = node->value; info.flags = kSyncAvailable;
				status_t state = SyncNodeStatus(node);
				if (state != B_WOULD_BLOCK) { info.flags |= kSyncSignaled; info.error = state; }
				for (; node != NULL; node = node->previous) {
					if (SyncNodeStatus(node) != B_WOULD_BLOCK) { info.completed = node->value; break; }
					if (node->completedBefore > info.completed) info.completed = node->completedBefore;
				}
			}
		}
		info.globalClients = sSyncClients; info.globalObjects = sSyncObjects;
		info.globalPoints = sSyncNodes; info.globalEvents = sSyncEvents;
		info.globalExports = sSyncExports; info.clientHandles = client->count;
		info.globalWaits = sSyncWaiters;
		return user_memcpy(user, &info, sizeof(info));
	}
	if (op == kTransferSync) {
		SyncTransfer request;
		status = ReadSyncRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.flags != 0 || request.reserved != 0) return B_BAD_VALUE;
		SyncObject* source = FindSyncObject(client, request.source);
		SyncToken token = {};
		if (source == NULL || !CaptureSyncPoint(source, request.source.point, token)) return B_BAD_VALUE;
		SyncMutation mutation = {};
		status = PrepareSyncMutations(client, &request.destination, 1, &mutation, NULL, &token);
		if (status == B_OK) CommitSyncMutations(&mutation, 1);
		DropSyncMutations(&mutation, 1); PutSyncToken(token); return status;
	}
	return B_DEV_INVALID_IOCTL;
}

static void
DeleteSyncTransaction(SyncTransaction* transaction)
{
	for (uint32 i = 0; i < transaction->waitCount; i++) PutSyncToken(transaction->waits[i]);
	DropSyncMutations(transaction->signals, transaction->signalCount);
	PutSyncEvent(transaction->event); free(transaction);
}

status_t
MaliCSF::PrepareSyncSubmission(void* cookie, const SyncPoint* points, uint32 waits,
	uint32 signals, SyncSubmission& submission)
{
	if (submission.state != NULL || waits > kMaxSyncPoints || signals > kMaxSyncPoints)
		return B_BAD_VALUE;
	if (points == NULL && (waits != 0 || signals != 0)) return B_BAD_VALUE;
	SyncTransaction* transaction = (SyncTransaction*)calloc(1, sizeof(SyncTransaction));
	if (transaction == NULL) return B_NO_MEMORY;
	transaction->waitCount = waits; transaction->signalCount = signals;
	SyncClient* client = (SyncClient*)cookie;
	mutex_lock(&sSyncLock);
	status_t status = CheckSyncClient(client);
	for (uint32 i = 0; status == B_OK && i < waits; i++) {
		SyncObject* object = FindSyncObject(client, points[i]);
		if (object == NULL || !CaptureSyncPoint(object, points[i].point, transaction->waits[i]))
			status = B_BAD_VALUE;
	}
	if (status == B_OK && signals != 0) {
		transaction->event = sSyncEvents < kMaxSyncEvents
			? (SyncEvent*)calloc(1, sizeof(SyncEvent)) : NULL;
		if (transaction->event == NULL) status = B_NO_MEMORY;
		else { transaction->event->references = 1; sSyncEvents++; }
	}
	if (status == B_OK) status = PrepareSyncMutations(client, signals != 0 ? points + waits : NULL, signals,
		transaction->signals, transaction->event);
	if (status != B_OK) {
		DeleteSyncTransaction(transaction); mutex_unlock(&sSyncLock); return status;
	}
	submission.state = transaction;
	return B_OK;
}

void
MaliCSF::CommitSyncSubmission(SyncSubmission& submission)
{
	SyncTransaction* transaction = (SyncTransaction*)submission.state;
	CommitSyncMutations(transaction->signals, transaction->signalCount);
	DropSyncMutations(transaction->signals, transaction->signalCount);
	transaction->signalCount = 0; transaction->committed = true;
	mutex_unlock(&sSyncLock);
}

void
MaliCSF::AbortSyncSubmission(SyncSubmission& submission)
{
	DeleteSyncTransaction((SyncTransaction*)submission.state); submission.state = NULL;
	mutex_unlock(&sSyncLock);
}

status_t
MaliCSF::ReadySyncSubmission(const SyncSubmission& submission)
{
	if (submission.state == NULL) return B_OK;
	MutexLocker locker(sSyncLock);
	SyncTransaction* transaction = (SyncTransaction*)submission.state;
	bool pending = false;
	for (uint32 i = 0; i < transaction->waitCount; i++) {
		status_t status = SyncTokenStatus(transaction->waits[i]);
		if (status == B_WOULD_BLOCK) pending = true;
		else if (status != B_OK) return status;
	}
	return pending ? B_WOULD_BLOCK : B_OK;
}

void
MaliCSF::FinishSyncSubmission(SyncSubmission& submission, status_t result)
{
	if (submission.state == NULL) return;
	MutexLocker locker(sSyncLock);
	SyncTransaction* transaction = (SyncTransaction*)submission.state;
	ASSERT(transaction->committed && result != B_WOULD_BLOCK);
	if (transaction->event != NULL) {
		transaction->event->result = result; transaction->event->done = true;
		sSyncChanged.NotifyAll();
	}
	DeleteSyncTransaction(transaction); submission.state = NULL;
}
