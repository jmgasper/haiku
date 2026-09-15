/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <KernelExport.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <stdlib.h>
#if defined(__aarch64__)
#include <arch/arm64/cache_line_size.h>
#endif

#include "CsfClient.h"
#include "CsfPageTable.h"

using namespace MaliCSF;

static mutex sClientLock = MUTEX_INITIALIZER("Mali CSF clients");
static const uint32 kMaxClients = 64;
static const uint64 kMaxGlobalBufferBytes = UINT64_C(512) << 20;
static uint32 sClients, sBuffers, sNextHandle = 1;
static uint64 sBufferBytes;
static const uint32 kMaxGlobalVms = 64;
static const uint32 kMaxGlobalGenerations = 512;
static const uint32 kMaxGlobalTablePages = 4096;
static uint32 sVms, sGenerations, sTablePages, sAccounts;

struct ClientMemory {
	area_id area;
	void* address;
	size_t bytes;
	uint64* pages;
};

// A generation can outlive its client. Keep its resident allocations charged
// to this account until the final mapping/work reference releases them.
struct ClientAccount {
	uint32 references;
	uint32 buffers;
	uint64 bytes;
};

struct ClientBuffer : ClientMemory {
	ClientBuffer* next;
	uint32 handle;
	uint32 references;
	ClientAccount* account;
};

struct ClientMapping {
	uint64 address;
	uint64 bytes;
	uint64 offset;
	ClientBuffer* buffer;
	uint32 flags;
};

struct ClientGeneration {
	uint32 references;
	uint32 count;
	uint64 number;
	uint64 userLimit;
	uint64 mappedBytes;
	ClientMemory tables;
	ClientMapping* mappings;
};

struct ClientVm {
	ClientVm* next;
	uint32 handle;
	ClientGeneration* current;
};

struct Client {
	team_id owner;
	bool writable;
	bool closed;
	ClientAccount* account;
	ClientBuffer* first;
	uint32 vms;
	ClientVm* firstVm;
};

static status_t
MakeClientRamNoncacheable(area_id area, void* address, size_t bytes)
{
#if defined(__aarch64__)
	// The allocator clears through its cached kernel alias. Evict that data
	// before changing the memory type, and before creating any user aliases.
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)address; p < (addr_t)address + bytes; p += line)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	// ARM64 encodes the type in each PTE; physicalBase is unused here. The
	// backing RAM can be scattered. vm_clone_area preserves this memory type.
	status_t status = vm_set_area_memory_type(area, 0, B_WRITE_COMBINING_MEMORY);
	memory_full_barrier();
	return status;
#else
	return B_NOT_SUPPORTED;
#endif
}

static status_t
CheckAccess(const Client* client)
{
	return !client->closed && client->owner == team_get_current_team_id()
		? B_OK : B_NOT_ALLOWED;
}

static void
DeleteMemory(ClientMemory& memory)
{
	if (memory.area >= B_OK)
		delete_area(memory.area);
	free(memory.pages);
	memset(&memory, 0, sizeof(memory));
	memory.area = -1;
}

static status_t
AllocateMemory(const char* name, size_t bytes, ClientMemory& memory)
{
	memory.area = -1;
	memory.bytes = bytes;
	memory.pages = (uint64*)malloc(bytes / B_PAGE_SIZE * sizeof(uint64));
	if (memory.pages == NULL)
		return B_NO_MEMORY;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	memory.area = create_area_etc(B_SYSTEM_TEAM, name, bytes,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &memory.address);
	status_t status = memory.area < B_OK ? memory.area : B_OK;
	for (size_t offset = 0; status == B_OK && offset < bytes; offset += B_PAGE_SIZE) {
		physical_entry entry;
		status = get_memory_map((uint8*)memory.address + offset, B_PAGE_SIZE, &entry, 1);
		if (status == B_OK && (entry.size < B_PAGE_SIZE
			|| !GpuPageTable::ValidPhysical(entry.address))) {
			status = B_BAD_VALUE;
		}
		if (status == B_OK)
			memory.pages[offset / B_PAGE_SIZE] = entry.address;
	}
	if (status == B_OK)
		status = MakeClientRamNoncacheable(memory.area, memory.address, bytes);
	if (status != B_OK)
		DeleteMemory(memory);
	return status;
}

static void
ReleaseAccount(ClientAccount* account)
{
	if (--account->references != 0)
		return;
	sAccounts--;
	free(account);
}

static void
ReleaseBuffer(ClientBuffer* buffer)
{
	if (--buffer->references != 0)
		return;
	buffer->account->bytes -= buffer->bytes;
	buffer->account->buffers--;
	sBufferBytes -= buffer->bytes;
	sBuffers--;
	ReleaseAccount(buffer->account);
	// User CPU clones retain the VM cache independently after this area is gone.
	DeleteMemory(*buffer);
	free(buffer);
}

static void
ReleaseGeneration(ClientGeneration* generation)
{
	if (--generation->references != 0)
		return;
	for (uint32 i = 0; i < generation->count; i++)
		ReleaseBuffer(generation->mappings[i].buffer);
	sTablePages -= generation->tables.bytes / B_PAGE_SIZE;
	sGenerations--;
	DeleteMemory(generation->tables);
	free(generation->mappings);
	free(generation);
}

static status_t
CreateGeneration(const ClientMapping* mappings, uint32 count, uint64 limit,
	uint64 number, ClientGeneration** result)
{
	PageTableRegion* regions = NULL;
	if (count != 0) {
		regions = (PageTableRegion*)malloc(count * sizeof(PageTableRegion));
		if (regions == NULL)
			return B_NO_MEMORY;
		for (uint32 i = 0; i < count; i++) {
			const ClientMapping& map = mappings[i];
			regions[i] = {map.address, map.bytes, map.offset, map.buffer->bytes,
				map.buffer->pages, map.flags};
		}
	}
	unsigned pages = GpuPageTable::CountPages(regions, count, limit);
	if (pages == 0 || sGenerations == kMaxGlobalGenerations
		|| pages > kMaxGlobalTablePages - sTablePages) {
		free(regions);
		return pages == 0 ? B_BAD_VALUE : B_NO_MEMORY;
	}
	ClientGeneration* generation = (ClientGeneration*)calloc(1, sizeof(ClientGeneration));
	if (generation == NULL) {
		free(regions);
		return B_NO_MEMORY;
	}
	if (count != 0) {
		generation->mappings = (ClientMapping*)malloc(count * sizeof(ClientMapping));
		if (generation->mappings == NULL) {
			free(regions);
			free(generation);
			return B_NO_MEMORY;
		}
		memcpy(generation->mappings, mappings, count * sizeof(ClientMapping));
	}
	status_t status = AllocateMemory("Mali CSF VM page tables", pages * B_PAGE_SIZE,
		generation->tables);
	if (status == B_OK && !GpuPageTable::Build(generation->tables.address,
		generation->tables.bytes, generation->tables.pages, pages, regions, count, limit)) {
		status = B_BAD_VALUE;
	}
	free(regions);
	if (status != B_OK) {
		DeleteMemory(generation->tables);
		free(generation->mappings);
		free(generation);
		return status;
	}
	// Complete the NC page-table stores before any later hardware activation.
	memory_full_barrier();
	generation->references = 1;
	generation->count = count;
	generation->number = number;
	generation->userLimit = limit;
	for (uint32 i = 0; i < count; i++) {
		generation->mappings[i].buffer->references++;
		generation->mappedBytes += mappings[i].bytes;
	}
	sGenerations++;
	sTablePages += pages;
	*result = generation;
	return B_OK;
}

status_t
MaliCSF::OpenClient(bool writable, void** cookie)
{
	Client* client = (Client*)calloc(1, sizeof(Client));
	if (client == NULL)
		return B_NO_MEMORY;
	client->account = (ClientAccount*)calloc(1, sizeof(ClientAccount));
	if (client->account == NULL) {
		free(client);
		return B_NO_MEMORY;
	}
	MutexLocker locker(sClientLock);
	if (sClients == kMaxClients) {
		free(client->account);
		free(client);
		return B_BUSY;
	}
	client->owner = team_get_current_team_id();
	client->writable = writable;
	client->account->references = 1;
	sAccounts++;
	sClients++;
	*cookie = client;
	return B_OK;
}

status_t
MaliCSF::AccessClient(void* cookie)
{
	MutexLocker locker(sClientLock);
	return CheckAccess((Client*)cookie);
}

static void
DeleteBuffer(ClientBuffer** link)
{
	ClientBuffer* buffer = *link;
	*link = buffer->next;
	ReleaseBuffer(buffer);
}

static void
DeleteVm(Client* client, ClientVm** link)
{
	ClientVm* vm = *link;
	*link = vm->next;
	client->vms--;
	sVms--;
	ReleaseGeneration(vm->current);
	free(vm);
}

static void
CloseLocked(Client* client)
{
	client->closed = true;
	while (client->firstVm != NULL)
		DeleteVm(client, &client->firstVm);
	while (client->first != NULL)
		DeleteBuffer(&client->first);
}

void
MaliCSF::CloseClient(void* cookie)
{
	MutexLocker locker(sClientLock);
	CloseLocked((Client*)cookie);
}

void
MaliCSF::FreeClient(void* cookie)
{
	MutexLocker locker(sClientLock);
	CloseLocked((Client*)cookie);
	ReleaseAccount(((Client*)cookie)->account);
	sClients--;
	free(cookie);
}

template<typename Request>
static status_t
ReadRequest(void* user, size_t length, Request& request)
{
	if (length != sizeof(request))
		return B_BAD_VALUE;
	if (user == NULL || user_memcpy(&request, user, sizeof(request)) != B_OK)
		return B_BAD_ADDRESS;
	return request.version == kClientVersion ? B_OK : B_BAD_VALUE;
}

static ClientBuffer**
FindBuffer(Client* client, uint32 handle)
{
	ClientBuffer** link = &client->first;
	while (*link != NULL && (*link)->handle != handle)
		link = &(*link)->next;
	return link;
}

static status_t
CreateBuffer(Client* client, BufferCreate& request)
{
	if (request.flags != 0 || request.reserved != 0 || request.reserved2 != 0
		|| request.handle != 0 || request.bytes == 0 || request.bytes > kMaxBufferBytes) {
		return B_BAD_VALUE;
	}
	size_t bytes = (request.bytes + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);
	if (client->account->buffers == kMaxClientBuffers || sNextHandle == 0
		|| bytes > kMaxClientBufferBytes - client->account->bytes
		|| bytes > kMaxGlobalBufferBytes - sBufferBytes) {
		return B_NO_MEMORY;
	}
	ClientBuffer* buffer = (ClientBuffer*)calloc(1, sizeof(ClientBuffer));
	if (buffer == NULL)
		return B_NO_MEMORY;
	status_t status = AllocateMemory("Mali CSF client buffer", bytes, *buffer);
	if (status != B_OK) {
		free(buffer);
		return status;
	}
	buffer->handle = sNextHandle++;
	buffer->references = 1;
	buffer->account = client->account;
	buffer->account->references++;
	buffer->next = client->first;
	client->first = buffer;
	client->account->buffers++;
	client->account->bytes += bytes;
	sBuffers++;
	sBufferBytes += bytes;
	request.bytes = bytes;
	request.handle = buffer->handle;
	return B_OK;
}

static ClientVm**
FindVm(Client* client, uint32 handle)
{
	ClientVm** link = &client->firstVm;
	while (*link != NULL && (*link)->handle != handle)
		link = &(*link)->next;
	return link;
}

static status_t
CreateVm(Client* client, VmCreate& request)
{
	uint64 limit = request.userLimit != 0 ? request.userLimit : kVmUserLimit;
	if (request.flags != 0 || request.handle != 0 || request.reserved != 0
		|| request.reserved2 != 0 || (limit & (B_PAGE_SIZE - 1)) != 0
		|| limit > kVmUserLimit) {
		return B_BAD_VALUE;
	}
	if (client->vms == kMaxClientVms || sVms == kMaxGlobalVms || sNextHandle == 0)
		return B_NO_MEMORY;
	ClientVm* vm = (ClientVm*)calloc(1, sizeof(ClientVm));
	if (vm == NULL)
		return B_NO_MEMORY;
	status_t status = CreateGeneration(NULL, 0, limit, 1, &vm->current);
	if (status != B_OK) {
		free(vm);
		return status;
	}
	vm->handle = sNextHandle++;
	vm->next = client->firstVm;
	client->firstVm = vm;
	client->vms++;
	sVms++;
	request.handle = vm->handle;
	request.userLimit = limit;
	return B_OK;
}

// The scratch arrays have room for a replacement to split one existing range
// into two pieces and insert a new mapping. Coalesce before applying the quota.
static status_t
ApplyVmOperation(Client* client, uint64 limit, const VmOperation& operation,
	const ClientMapping* input, uint32 inputCount, ClientMapping* output, uint32& count)
{
	if (operation.reserved != 0
		|| !GpuPageTable::ValidRange(operation.address, operation.bytes, limit)) {
		return B_BAD_VALUE;
	}
	ClientBuffer* buffer = NULL;
	if (operation.type == kVmMapOperation) {
		if (!GpuPageTable::ValidPermissions(operation.flags) || (operation.offset & 4095) != 0)
			return B_BAD_VALUE;
		buffer = *FindBuffer(client, operation.buffer);
		if (buffer == NULL)
			return B_ENTRY_NOT_FOUND;
		if (operation.offset > buffer->bytes || operation.bytes > buffer->bytes - operation.offset)
			return B_BAD_VALUE;
	} else if (operation.type != kVmUnmapOperation || operation.flags != 0
		|| operation.buffer != 0 || operation.offset != 0) {
		return B_BAD_VALUE;
	}
	uint64 start = operation.address, end = start + operation.bytes;
	count = 0;
	for (uint32 i = 0; i < inputCount; i++) {
		ClientMapping map = input[i];
		uint64 mapEnd = map.address + map.bytes;
		if (mapEnd <= start || map.address >= end) {
			output[count++] = map;
			continue;
		}
		if (map.address < start) {
			output[count] = map;
			output[count++].bytes = start - map.address;
		}
		if (mapEnd > end) {
			map.offset += end - map.address;
			map.address = end;
			map.bytes = mapEnd - end;
			output[count++] = map;
		}
	}
	if (buffer != NULL) {
		uint32 at = 0;
		while (at < count && output[at].address < start)
			at++;
		memmove(output + at + 1, output + at, (count - at) * sizeof(ClientMapping));
		output[at] = {start, operation.bytes, operation.offset, buffer, operation.flags};
		count++;
	}
	uint32 merged = 0;
	uint64 mappedBytes = 0;
	for (uint32 i = 0; i < count; i++) {
		ClientMapping map = output[i];
		if (map.bytes > kMaxVmMappedBytes - mappedBytes)
			return B_NO_MEMORY;
		mappedBytes += map.bytes;
		if (merged != 0 && output[merged - 1].buffer == map.buffer
			&& output[merged - 1].flags == map.flags
			&& output[merged - 1].address + output[merged - 1].bytes == map.address
			&& output[merged - 1].offset + output[merged - 1].bytes == map.offset) {
			output[merged - 1].bytes += map.bytes;
		} else {
			output[merged++] = map;
		}
	}
	count = merged;
	return count <= kMaxVmMappings ? B_OK : B_NO_MEMORY;
}

static status_t
BindVm(Client* client, void* user, size_t length)
{
	VmBind request;
	if (length < sizeof(request))
		return B_BAD_VALUE;
	status_t status = ReadRequest(user, sizeof(request), request);
	if (status != B_OK)
		return status;
	if (request.flags != 0 || request.newGeneration != 0 || request.operationCount == 0
		|| request.operationCount > kMaxVmOperations
		|| length != sizeof(request) + request.operationCount * sizeof(VmOperation)) {
		return B_BAD_VALUE;
	}
	ClientVm* vm = *FindVm(client, request.handle);
	if (vm == NULL)
		return B_ENTRY_NOT_FOUND;
	ClientGeneration* old = vm->current;
	if (request.expectedGeneration != 0 && request.expectedGeneration != old->number)
		return B_BUSY;
	if (old->number == UINT64_MAX)
		return B_NO_MEMORY;
	VmOperation* operations = (VmOperation*)malloc(request.operationCount * sizeof(VmOperation));
	if (operations == NULL)
		return B_NO_MEMORY;
	status = user_memcpy(operations, (const uint8*)user + sizeof(request),
		request.operationCount * sizeof(VmOperation));
	if (status != B_OK) {
		free(operations);
		return status;
	}
	const uint32 capacity = kMaxVmMappings + 2;
	ClientMapping* scratch = (ClientMapping*)malloc(2 * capacity * sizeof(ClientMapping));
	if (scratch == NULL) {
		free(operations);
		return B_NO_MEMORY;
	}
	ClientMapping* current = scratch;
	ClientMapping* next = scratch + capacity;
	uint32 count = old->count;
	if (count != 0)
		memcpy(current, old->mappings, count * sizeof(ClientMapping));
	for (uint32 i = 0; status == B_OK && i < request.operationCount; i++) {
		uint32 nextCount;
		status = ApplyVmOperation(client, old->userLimit, operations[i], current, count,
			next, nextCount);
		if (status == B_OK) {
			ClientMapping* swap = current; current = next; next = swap;
			count = nextCount;
		}
	}
	ClientGeneration* candidate = NULL;
	if (status == B_OK) {
		status = CreateGeneration(current, count, old->userLimit, old->number + 1, &candidate);
	}
	free(scratch);
	free(operations);
	if (status != B_OK)
		return status;
	request.newGeneration = candidate->number;
	status = user_memcpy(user, &request, sizeof(request));
	if (status != B_OK) {
		ReleaseGeneration(candidate);
		return status;
	}
	// No hardware root changes here. A future submission leases this generation;
	// already queued work keeps its previous root until it has completed.
	vm->current = candidate;
	ReleaseGeneration(old);
	return B_OK;
}

status_t
MaliCSF::AcquireClientVm(void* cookie, uint32 handle, uint64 expectedGeneration,
	ClientVmLease& lease)
{
	MutexLocker locker(sClientLock);
	Client* client = (Client*)cookie;
	status_t status = CheckAccess(client);
	if (status != B_OK)
		return status;
	if (!client->writable)
		return B_NOT_ALLOWED;
	if (lease.state != NULL)
		return B_BAD_VALUE;
	ClientVm* vm = *FindVm(client, handle);
	if (vm == NULL)
		return B_ENTRY_NOT_FOUND;
	ClientGeneration* current = vm->current;
	if (expectedGeneration != 0 && expectedGeneration != current->number)
		return B_BUSY;
	if (current->references == UINT32_MAX)
		return B_NO_MEMORY;
	current->references++;
	lease.state = current;
	lease.rootPhysical = current->tables.pages[0];
	lease.generation = current->number;
	lease.userLimit = current->userLimit;
	return B_OK;
}

void
MaliCSF::ReleaseClientVm(ClientVmLease& lease)
{
	MutexLocker locker(sClientLock);
	if (lease.state != NULL)
		ReleaseGeneration((ClientGeneration*)lease.state);
	memset(&lease, 0, sizeof(lease));
}

const uint64*
MaliCSF::ClientVmRoot(const ClientVmLease& lease)
{
	const ClientGeneration* generation = (const ClientGeneration*)lease.state;
	return generation == NULL ? NULL : (const uint64*)generation->tables.address;
}

bool
MaliCSF::ClientVmRange(const ClientVmLease& lease, uint64 address, uint64 bytes)
{
	const ClientGeneration* generation = (const ClientGeneration*)lease.state;
	if (generation == NULL || bytes == 0 || address >= generation->userLimit
		|| bytes > generation->userLimit - address)
		return false;
	for (uint32 i = 0; i < generation->count; i++) {
		const ClientMapping& map = generation->mappings[i];
		uint64 end = map.address + map.bytes;
		if (address >= end)
			continue;
		if (address < map.address)
			return false;
		if (bytes <= end - address)
			return true;
		bytes -= end - address;
		address = end;
	}
	return false;
}

status_t
MaliCSF::ControlClient(void* cookie, uint32 op, void* user, size_t length)
{
	MutexLocker locker(sClientLock);
	Client* client = (Client*)cookie;
	status_t status = CheckAccess(client);
	if (status != B_OK)
		return status;
	if (op == kGetClientInfo) {
		ClientInfo request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved[0] != 0 || request.reserved[1] != 0)
			return B_BAD_VALUE;
		ClientInfo info = {};
		info.version = kClientVersion;
		info.capabilities = client->writable ? kClientCpuBuffers | kClientVmMappings | kClientQueues | kClientSynchronization : 0;
		info.maxBuffers = kMaxClientBuffers;
		info.maxBufferBytes = kMaxBufferBytes;
		info.maxClientBytes = kMaxClientBufferBytes;
		info.bufferCount = client->account->buffers;
		info.bufferBytes = client->account->bytes;
		info.globalBufferBytes = sBufferBytes;
		info.globalBuffers = sBuffers;
		info.globalClients = sClients;
		return user_memcpy(user, &info, sizeof(info));
	}
	if (op == kGetVmInfo) {
		VmInfo request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved != 0)
			return B_BAD_VALUE;
		VmInfo info = {};
		info.version = kClientVersion;
		info.handle = request.handle;
		if (request.handle != 0) {
			ClientVm* vm = *FindVm(client, request.handle);
			if (vm == NULL)
				return B_ENTRY_NOT_FOUND;
			ClientGeneration* current = vm->current;
			info.userLimit = current->userLimit;
			info.generation = current->number;
			info.mappedBytes = current->mappedBytes;
			info.mappings = current->count;
			info.tablePages = current->tables.bytes / B_PAGE_SIZE;
		}
		info.clientVms = client->vms;
		info.globalVms = sVms;
		info.globalGenerations = sGenerations;
		info.globalTablePages = sTablePages;
		return user_memcpy(user, &info, sizeof(info));
	}
	if ((op < kCreateBuffer || op > kGetBufferInfo) && (op < kCreateVm || op > kBindVm))
		return B_DEV_INVALID_IOCTL;
	if (!client->writable)
		return B_NOT_ALLOWED;
	if (op == kCreateVm) {
		VmCreate request;
		status = ReadRequest(user, length, request);
		if (status == B_OK)
			status = CreateVm(client, request);
		if (status != B_OK)
			return status;
		status = user_memcpy(user, &request, sizeof(request));
		if (status != B_OK)
			DeleteVm(client, FindVm(client, request.handle));
		return status;
	}
	if (op == kDestroyVm) {
		VmHandle request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved != 0)
			return B_BAD_VALUE;
		ClientVm** link = FindVm(client, request.handle);
		if (*link == NULL)
			return B_ENTRY_NOT_FOUND;
		DeleteVm(client, link);
		return B_OK;
	}
	if (op == kBindVm)
		return BindVm(client, user, length);
	if (op == kCreateBuffer) {
		BufferCreate request;
		status = ReadRequest(user, length, request);
		if (status == B_OK)
			status = CreateBuffer(client, request);
		if (status != B_OK)
			return status;
		status = user_memcpy(user, &request, sizeof(request));
		if (status != B_OK)
			DeleteBuffer(FindBuffer(client, request.handle));
		return status;
	}
	if (op == kDestroyBuffer) {
		BufferHandle request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved != 0)
			return B_BAD_VALUE;
		ClientBuffer** link = FindBuffer(client, request.handle);
		if (*link == NULL)
			return B_ENTRY_NOT_FOUND;
		DeleteBuffer(link);
		return B_OK;
	}
	if (op == kGetBufferInfo) {
		BufferInfo request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved != 0)
			return B_BAD_VALUE;
		ClientBuffer* buffer = *FindBuffer(client, request.handle);
		if (buffer == NULL)
			return B_ENTRY_NOT_FOUND;
		request.bytes = buffer->bytes;
		request.flags = kBufferNormalNoncacheable;
		return user_memcpy(user, &request, sizeof(request));
	}
	BufferMap request;
	status = ReadRequest(user, length, request);
	if (status != B_OK)
		return status;
	if (request.address != 0 || request.area != 0 || request.flags != 0
		|| request.bytes != 0 || request.reserved != 0) {
		return B_BAD_VALUE;
	}
	ClientBuffer* buffer = *FindBuffer(client, request.handle);
	if (buffer == NULL)
		return B_ENTRY_NOT_FOUND;
	void* address = NULL;
	area_id area = vm_clone_area(client->owner, "Mali CSF buffer mapping", &address,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0, buffer->area, true);
	if (area < B_OK)
		return area;
	request.address = (addr_t)address;
	request.area = area;
	request.bytes = buffer->bytes;
	request.flags = kBufferNormalNoncacheable;
	status = user_memcpy(user, &request, sizeof(request));
	if (status != B_OK)
		vm_delete_area(client->owner, area, true);
	return status;
}
