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

using namespace MaliCSF;

static mutex sClientLock = MUTEX_INITIALIZER("Mali CSF clients");
static const uint32 kMaxClients = 64;
static const uint64 kMaxGlobalBufferBytes = UINT64_C(512) << 20;
static uint32 sClients, sBuffers, sNextHandle = 1;
static uint64 sBufferBytes;

struct ClientBuffer {
	ClientBuffer* next;
	uint32 handle;
	area_id area;
	void* address;
	size_t bytes;
	uint64* pages;
};

struct Client {
	team_id owner;
	bool writable;
	bool closed;
	uint32 buffers;
	uint64 bytes;
	ClientBuffer* first;
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

status_t
MaliCSF::OpenClient(bool writable, void** cookie)
{
	Client* client = (Client*)calloc(1, sizeof(Client));
	if (client == NULL)
		return B_NO_MEMORY;
	MutexLocker locker(sClientLock);
	if (sClients == kMaxClients) {
		free(client);
		return B_BUSY;
	}
	client->owner = team_get_current_team_id();
	client->writable = writable;
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
DeleteBuffer(Client* client, ClientBuffer** link)
{
	ClientBuffer* buffer = *link;
	*link = buffer->next;
	// No GPU submission references exist in this ABI yet. The VM cache keeps
	// mapped RAM alive after this kernel area is deleted, including user clones.
	delete_area(buffer->area);
	client->bytes -= buffer->bytes;
	client->buffers--;
	sBufferBytes -= buffer->bytes;
	sBuffers--;
	free(buffer->pages);
	free(buffer);
}

static void
CloseLocked(Client* client)
{
	client->closed = true;
	while (client->first != NULL)
		DeleteBuffer(client, &client->first);
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
	if (client->buffers == kMaxClientBuffers || sNextHandle == 0
		|| bytes > kMaxClientBufferBytes - client->bytes
		|| bytes > kMaxGlobalBufferBytes - sBufferBytes) {
		return B_NO_MEMORY;
	}
	ClientBuffer* buffer = (ClientBuffer*)calloc(1, sizeof(ClientBuffer));
	if (buffer == NULL)
		return B_NO_MEMORY;
	buffer->pages = (uint64*)malloc(bytes / B_PAGE_SIZE * sizeof(uint64));
	if (buffer->pages == NULL) {
		free(buffer);
		return B_NO_MEMORY;
	}
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	buffer->area = create_area_etc(B_SYSTEM_TEAM, "Mali CSF client buffer", bytes,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &buffer->address);
	status_t status = buffer->area < B_OK ? buffer->area : B_OK;
	for (size_t offset = 0; status == B_OK && offset < bytes; offset += B_PAGE_SIZE) {
		physical_entry entry;
		status = get_memory_map((uint8*)buffer->address + offset, B_PAGE_SIZE, &entry, 1);
		if (status == B_OK && (entry.size < B_PAGE_SIZE
			|| (entry.address & (B_PAGE_SIZE - 1)) != 0
			|| entry.address >= (UINT64_C(1) << 40))) {
			status = B_BAD_VALUE;
		}
		if (status == B_OK)
			buffer->pages[offset / B_PAGE_SIZE] = entry.address;
	}
	if (status == B_OK)
		status = MakeClientRamNoncacheable(buffer->area, buffer->address, bytes);
	if (status != B_OK) {
		if (buffer->area >= B_OK)
			delete_area(buffer->area);
		free(buffer->pages);
		free(buffer);
		return status;
	}
	buffer->handle = sNextHandle++;
	buffer->bytes = bytes;
	buffer->next = client->first;
	client->first = buffer;
	client->buffers++;
	client->bytes += bytes;
	sBuffers++;
	sBufferBytes += bytes;
	request.bytes = bytes;
	request.handle = buffer->handle;
	return B_OK;
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
		info.capabilities = client->writable ? kClientCpuBuffers : 0;
		info.maxBuffers = kMaxClientBuffers;
		info.maxBufferBytes = kMaxBufferBytes;
		info.maxClientBytes = kMaxClientBufferBytes;
		info.bufferCount = client->buffers;
		info.bufferBytes = client->bytes;
		info.globalBufferBytes = sBufferBytes;
		info.globalBuffers = sBuffers;
		info.globalClients = sClients;
		return user_memcpy(user, &info, sizeof(info));
	}
	if (op < kCreateBuffer || op > kGetBufferInfo)
		return B_DEV_INVALID_IOCTL;
	if (!client->writable)
		return B_NOT_ALLOWED;
	if (op == kCreateBuffer) {
		BufferCreate request;
		status = ReadRequest(user, length, request);
		if (status == B_OK)
			status = CreateBuffer(client, request);
		if (status != B_OK)
			return status;
		status = user_memcpy(user, &request, sizeof(request));
		if (status != B_OK)
			DeleteBuffer(client, FindBuffer(client, request.handle));
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
		DeleteBuffer(client, link);
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
