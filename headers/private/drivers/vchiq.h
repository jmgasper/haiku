/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _VCHIQ_H
#define _VCHIQ_H


#include <Drivers.h>
#include <module.h>


/*	VCHIQ is the message channel to the services of the Raspberry Pi's
	VideoCore firmware: sound output ("AUDS"), the multimedia components
	("mmal": H.264 decoding, camera), and more. Kernel drivers use the
	module; user programs /dev/misc/vchiq. */

#define VCHIQ_MODULE_NAME	"generic/vchiq/v1"
#define VCHIQ_DEVICE_PATH	"/dev/misc/vchiq"

#define VCHIQ_FOURCC(a, b, c, d) \
	(((uint32)(a) << 24) | ((uint32)(b) << 16) | ((uint32)(c) << 8) \
		| (uint32)(d))

// the most a message can carry, and a bulk transfer
#define VCHIQ_MAX_MESSAGE_SIZE	(4096 - 8)
#define VCHIQ_MAX_BULK_SIZE		(16 * 1024 * 1024)


#ifdef _KERNEL_MODE

struct vchiq_service;

enum {
	VCHIQ_EVENT_MESSAGE,
		// data and size are the message; they are gone when the hook returns
	VCHIQ_EVENT_CLOSED
		// the firmware closed the service
};

/*!	Called in the module's receiving thread: be short, and do not wait for
	another message or close the service in here. Sending is fine.
*/
typedef void (*vchiq_service_hook)(void* cookie, uint32 event,
	const void* data, size_t size);


typedef struct vchiq_module_info {
	module_info	info;

	/*!	B_OK when the firmware is connected; the module loads on any board. */
	status_t	(*init_check)();

	status_t	(*open_service)(uint32 fourcc, int16 version,
					int16 minVersion, vchiq_service_hook hook, void* cookie,
					struct vchiq_service** _service);
	void		(*close_service)(struct vchiq_service* service);
	int16		(*peer_version)(struct vchiq_service* service);

	/*!	Sends one message of at most VCHIQ_MAX_MESSAGE_SIZE bytes. */
	status_t	(*queue_message)(struct vchiq_service* service,
					const void* data, size_t size, bool userData);

	/*!	Bulk transfers: the firmware copies from or to memory of ours.
		Both wait until the firmware has done its part; one transfer per
		direction and service at a time.
	*/
	status_t	(*bulk_transmit)(struct vchiq_service* service,
					const void* data, size_t size, bool userData);
	status_t	(*bulk_receive)(struct vchiq_service* service, void* data,
					size_t size, bool userData, size_t* _actual);
} vchiq_module_info;

#endif	// _KERNEL_MODE


// /dev/misc/vchiq: the services opened through a file descriptor are closed
// with it.

enum {
	VCHIQ_OPEN_SERVICE = B_DEVICE_OP_CODES_END + 0x5600,
	VCHIQ_CLOSE_SERVICE,
	VCHIQ_QUEUE_MESSAGE,
	VCHIQ_DEQUEUE_MESSAGE,
	VCHIQ_BULK_TRANSMIT,
	VCHIQ_BULK_RECEIVE
};

typedef struct vchiq_open_request {
	uint32		fourcc;
	int16		version;
	int16		min_version;
	uint32		handle;			// out
	int16		peer_version;	// out
} vchiq_open_request;

typedef struct vchiq_transfer_request {
	uint32		handle;
	uint32		size;			// of data; of the buffer when receiving
	void*		data;
	bigtime_t	timeout;		// VCHIQ_DEQUEUE_MESSAGE, relative
	uint32		actual;			// out, when receiving
} vchiq_transfer_request;

/*	VCHIQ_DEQUEUE_MESSAGE returns B_TIMED_OUT when nothing arrived,
	B_BUFFER_OVERFLOW when the next message is larger than the buffer (it
	stays queued, "actual" is its size), and B_DEV_NOT_READY once the
	firmware has closed the service and its messages are read. */


#endif	/* _VCHIQ_H */
