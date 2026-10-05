/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	/dev/misc/vchiq: the services of the Raspberry Pi's VideoCore firmware
	for user programs (the H.264 decoder add-on, lab tools). All the work is
	the vchiq module's; this keeps the messages of a service until its owner
	reads them. On other boards the module finds no device and every open
	fails. */


#include <Drivers.h>
#include <KernelExport.h>

#include <new>
#include <stdlib.h>
#include <string.h>

#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include <vchiq.h>


#define MAX_USER_SERVICES	16
#define MAX_QUEUED_MESSAGES	512


struct queued_message : DoublyLinkedListLinkImpl<queued_message> {
	uint32	size;
	uint8	data[0];
};

struct user_service {
	vchiq_service*	service;
	spinlock		lock;
	DoublyLinkedList<queued_message> messages;
	int32			count;
	sem_id			sem;
	bool			closed;
};

struct user_client {
	mutex			lock;
	user_service*	services[MAX_USER_SERVICES];
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static vchiq_module_info* sVchiq;
static const char* sDeviceNames[] = { "misc/vchiq", NULL };


static void
service_hook(void* cookie, uint32 event, const void* data, size_t size)
{
	user_service* service = (user_service*)cookie;

	if (event == VCHIQ_EVENT_CLOSED) {
		service->closed = true;
		release_sem(service->sem);
		return;
	}

	if (atomic_get(&service->count) >= MAX_QUEUED_MESSAGES) {
		dprintf("vchiq: a message is lost, the owner of a service does not "
			"read\n");
		return;
	}

	queued_message* message
		= (queued_message*)malloc(sizeof(queued_message) + size);
	if (message == NULL)
		return;
	new(message) queued_message;
	message->size = size;
	memcpy(message->data, data, size);

	InterruptsSpinLocker locker(service->lock);
	service->messages.Add(message);
	locker.Unlock();

	atomic_add(&service->count, 1);
	release_sem(service->sem);
}


static void
delete_service(user_service* service)
{
	sVchiq->close_service(service->service);
		// no hook runs after this

	while (queued_message* message = service->messages.RemoveHead())
		free(message);
	delete_sem(service->sem);
	delete service;
}


static status_t
open_service(user_client* client, vchiq_open_request& request)
{
	MutexLocker locker(client->lock);

	uint32 slot = 0;
	while (slot < MAX_USER_SERVICES && client->services[slot] != NULL)
		slot++;
	if (slot == MAX_USER_SERVICES)
		return B_NO_MORE_PORTS;

	user_service* service = new(std::nothrow) user_service;
	if (service == NULL)
		return B_NO_MEMORY;
	service->lock = B_SPINLOCK_INITIALIZER;
	service->count = 0;
	service->closed = false;
	service->sem = create_sem(0, "vchiq messages");
	if (service->sem < 0) {
		delete service;
		return B_NO_MORE_SEMS;
	}

	status_t status = sVchiq->open_service(request.fourcc, request.version,
		request.min_version, service_hook, service, &service->service);
	if (status != B_OK) {
		delete_sem(service->sem);
		delete service;
		return status;
	}

	client->services[slot] = service;
	request.handle = slot + 1;
	request.peer_version = sVchiq->peer_version(service->service);
	return B_OK;
}


static status_t
dequeue_message(user_service* service, vchiq_transfer_request& request)
{
	while (true) {
		InterruptsSpinLocker locker(service->lock);
		queued_message* message = service->messages.Head();
		if (message != NULL) {
			request.actual = message->size;
			if (message->size > request.size)
				return B_BUFFER_OVERFLOW;
			service->messages.Remove(message);
			locker.Unlock();

			atomic_add(&service->count, -1);
			status_t status = user_memcpy(request.data, message->data,
				message->size);
			free(message);
			return status;
		}
		locker.Unlock();

		if (service->closed)
			return B_DEV_NOT_READY;

		status_t status = acquire_sem_etc(service->sem, 1,
			B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, request.timeout);
		if (status != B_OK)
			return status;
	}
}


//	#pragma mark - device


static status_t
vchiq_open(const char* name, uint32 flags, void** _cookie)
{
	user_client* client = new(std::nothrow) user_client;
	if (client == NULL)
		return B_NO_MEMORY;

	mutex_init(&client->lock, "vchiq client");
	memset(client->services, 0, sizeof(client->services));
	*_cookie = client;
	return B_OK;
}


static status_t
vchiq_close(void* cookie)
{
	return B_OK;
}


static status_t
vchiq_free(void* cookie)
{
	user_client* client = (user_client*)cookie;

	for (uint32 i = 0; i < MAX_USER_SERVICES; i++) {
		if (client->services[i] != NULL)
			delete_service(client->services[i]);
	}
	mutex_destroy(&client->lock);
	delete client;
	return B_OK;
}


static status_t
vchiq_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	user_client* client = (user_client*)cookie;

	if (op == VCHIQ_OPEN_SERVICE) {
		vchiq_open_request request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;

		status_t status = open_service(client, request);
		if (status != B_OK)
			return status;
		return user_memcpy(buffer, &request, sizeof(request));
	}

	if (op < VCHIQ_CLOSE_SERVICE || op > VCHIQ_BULK_RECEIVE)
		return B_DEV_INVALID_IOCTL;

	vchiq_transfer_request request;
	if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
		return B_BAD_ADDRESS;

	// Closing a service while another thread still uses it is the program's
	// mistake; nothing here waits for that thread.
	MutexLocker locker(client->lock);
	uint32 slot = request.handle - 1;
	if (slot >= MAX_USER_SERVICES || client->services[slot] == NULL)
		return B_BAD_VALUE;
	user_service* service = client->services[slot];

	if (op == VCHIQ_CLOSE_SERVICE) {
		client->services[slot] = NULL;
		locker.Unlock();
		delete_service(service);
		return B_OK;
	}
	locker.Unlock();

	if (request.size > 0 && !IS_USER_ADDRESS(request.data))
		return B_BAD_ADDRESS;

	status_t status;
	switch (op) {
		case VCHIQ_QUEUE_MESSAGE:
			return sVchiq->queue_message(service->service, request.data,
				request.size, true);

		case VCHIQ_BULK_TRANSMIT:
			return sVchiq->bulk_transmit(service->service, request.data,
				request.size, true);

		case VCHIQ_DEQUEUE_MESSAGE:
			status = dequeue_message(service, request);
			if (status != B_OK && status != B_BUFFER_OVERFLOW)
				return status;
			break;

		case VCHIQ_BULK_RECEIVE:
		{
			size_t actual = 0;
			status = sVchiq->bulk_receive(service->service, request.data,
				request.size, true, &actual);
			if (status != B_OK)
				return status;
			request.actual = actual;
			break;
		}

		default:
			return B_DEV_INVALID_IOCTL;
	}

	if (user_memcpy(buffer, &request, sizeof(request)) != B_OK)
		return B_BAD_ADDRESS;
	return status;
}


static status_t
vchiq_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
vchiq_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sHooks = {
	vchiq_open,
	vchiq_close,
	vchiq_free,
	vchiq_control,
	vchiq_read,
	vchiq_write
};


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(VCHIQ_MODULE_NAME, (module_info**)&sVchiq);
	if (status != B_OK)
		return status;

	// not a Raspberry Pi, or the firmware does not answer
	status = sVchiq->init_check();
	if (status != B_OK)
		put_module(VCHIQ_MODULE_NAME);
	return status;
}


void
uninit_driver()
{
	put_module(VCHIQ_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
