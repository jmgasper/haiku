/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MmalDecoder.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <Autolock.h>

#include <vchiq.h>


#define REQUEST_TIMEOUT		5000000LL
#define FIRST_REQUEST		0x10000
	// request contexts are above those of the buffers

//#define TRACE_MMAL
#ifdef TRACE_MMAL
#	define TRACE(x...)	fprintf(stderr, "mmal: " x)
#else
#	define TRACE(x...)	do {} while (false)
#endif


static inline uint32
align_up(uint32 value, uint32 alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}


MmalDecoder::MmalDecoder()
	:
	fToken(-1),
	fDevice(-1),
	fService(0),
	fComponent(0),
	fComponentEnabled(false),
	fInputSize(0),
	fEncoding(0),
	fWidth(0),
	fHeight(0),
	fStride(0),
	fSliceHeight(0),
	fReader(-1),
	fQuit(false),
	fWakeSem(-1),
	fRequestLock("mmal request"),
	fReplySem(-1),
	fNextContext(FIRST_REQUEST),
	fRequestContext(0),
	fReply(NULL),
	fReplySize(0),
	fLock("mmal decoder"),
	fBufferCount(0),
	fBufferSize(0),
	fReadyCount(0),
	fOutputEnabled(false),
	fInputOutstanding(0),
	fEndOfStream(false),
	fFormatChanged(false),
	fStatus(B_OK),
	fScratch(NULL),
	fInputCopy(NULL)
{
	memset(fBuffers, 0, sizeof(fBuffers));
	memset(&fControl, 0, sizeof(fControl));
	memset(&fInput, 0, sizeof(fInput));
	memset(&fOutput, 0, sizeof(fOutput));
}


MmalDecoder::~MmalDecoder()
{
	Close();
}


void
MmalDecoder::_Fail(const char* format, ...)
{
	char text[256];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);

	BAutolock locker(fLock);
	if (fStatus == B_OK) {
		fStatus = B_ERROR;
		fError = text;
	}
	release_sem(fWakeSem);
}


//	#pragma mark - messages


status_t
MmalDecoder::_Send(const void* message, size_t size)
{
	vchiq_transfer_request request = {};
	request.handle = fService;
	request.data = (void*)message;
	request.size = size;
	if (ioctl(fDevice, VCHIQ_QUEUE_MESSAGE, &request, sizeof(request)) != 0)
		return errno;
	return B_OK;
}


/*!	Sends \a message and replaces it with the firmware's answer. Every
	answer starts with a status.
*/
status_t
MmalDecoder::_Request(mmal_message& message, size_t size, size_t replySize)
{
	BAutolock locker(fRequestLock);

	message.h.magic = MMAL_MAGIC;
	message.h.context = fNextContext++;
	message.h.status = 0;
	if (fNextContext == 0)
		fNextContext = FIRST_REQUEST;

	fReply = &message;
	fReplySize = sizeof(mmal_header) + replySize;
	fRequestContext = message.h.context;

	status_t status = _Send(&message, sizeof(mmal_header) + size);
	if (status == B_OK) {
		status = acquire_sem_etc(fReplySem, 1, B_RELATIVE_TIMEOUT,
			REQUEST_TIMEOUT);
	}
	if (status != B_OK) {
		BAutolock bufferLocker(fLock);
		fRequestContext = 0;
		fReply = NULL;
		// the answer may have come in between
		while (acquire_sem_etc(fReplySem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK) {
		}
		return status;
	}

	if (message.u.status.status != 0) {
		TRACE("request %" B_PRIu32 ": status %" B_PRIu32 "\n",
			message.h.type, message.u.status.status);
		return B_ERROR;
	}
	return B_OK;
}


status_t
MmalDecoder::_GetPort(Port& port)
{
	mmal_message message = {};
	message.h.type = MMAL_MSG_PORT_INFO_GET;
	message.u.port_info_get.component_handle = fComponent;
	message.u.port_info_get.port_type = port.type;
	message.u.port_info_get.index = 0;
	status_t status = _Request(message, sizeof(mmal_port_info_get),
		sizeof(mmal_port_info_get_reply));
	if (status != B_OK)
		return status;

	const mmal_port_info_get_reply& reply = message.u.port_info_get_reply;
	port.handle = reply.port_handle;
	port.port = reply.port;
	port.format = reply.format;
	port.es = reply.es;
	return B_OK;
}


status_t
MmalDecoder::_SetPort(Port& port)
{
	mmal_message message = {};
	message.h.type = MMAL_MSG_PORT_INFO_SET;
	mmal_port_info_set& set = message.u.port_info_set;
	set.component_handle = fComponent;
	set.port_type = port.type;
	set.port_index = 0;
	set.port = port.port;
	set.format = port.format;
	set.format.extradata_size = 0;
	set.es = port.es;
	return _Request(message, sizeof(mmal_port_info_set),
		sizeof(mmal_port_info_get_reply));
}


status_t
MmalDecoder::_PortAction(Port& port, uint32 action)
{
	mmal_message message = {};
	message.h.type = MMAL_MSG_PORT_ACTION;
	message.u.port_action.component_handle = fComponent;
	message.u.port_action.port_handle = port.handle;
	message.u.port_action.action = action;
	message.u.port_action.port = port.port;
	return _Request(message, sizeof(mmal_port_action),
		sizeof(mmal_status_reply));
}


status_t
MmalDecoder::_SetParameter(Port& port, uint32 id, uint32 value)
{
	mmal_message message = {};
	message.h.type = MMAL_MSG_PORT_PARAMETER_SET;
	message.u.port_parameter_set.component_handle = fComponent;
	message.u.port_parameter_set.port_handle = port.handle;
	message.u.port_parameter_set.id = id;
	message.u.port_parameter_set.size = 2 * sizeof(uint32) + sizeof(uint32);
	message.u.port_parameter_set.value[0] = value;
	return _Request(message, 5 * sizeof(uint32), sizeof(mmal_status_reply));
}


//	#pragma mark - the firmware's messages


int32
MmalDecoder::_ReaderEntry(void* self)
{
	((MmalDecoder*)self)->_Reader();
	return 0;
}


void
MmalDecoder::_Reader()
{
	mmal_message message;

	while (!fQuit) {
		vchiq_transfer_request request = {};
		request.handle = fService;
		request.data = &message;
		request.size = sizeof(message);
		request.timeout = 100000;
		if (ioctl(fDevice, VCHIQ_DEQUEUE_MESSAGE, &request, sizeof(request))
				!= 0) {
			if (errno == B_TIMED_OUT || errno == B_INTERRUPTED)
				continue;
			if (!fQuit)
				_Fail("the firmware's service is gone: %s", strerror(errno));
			return;
		}
		if (request.actual < sizeof(mmal_header)
			|| message.h.magic != MMAL_MAGIC) {
			continue;
		}

		switch (message.h.type) {
			case MMAL_MSG_BUFFER_TO_HOST:
				_BufferToHost(message);
				break;

			case MMAL_MSG_EVENT_TO_HOST:
				_EventToHost(message);
				break;

			default:
			{
				BAutolock locker(fLock);
				if (fReply != NULL && message.h.context == fRequestContext) {
					memcpy(fReply, &message, min_c(fReplySize,
						(size_t)request.actual));
					fReply = NULL;
					fRequestContext = 0;
					release_sem(fReplySem);
				}
				break;
			}
		}
	}
}


void
MmalDecoder::_BufferToHost(const mmal_message& message)
{
	const mmal_buffer_from_host& buffer = message.u.buffer_from_host;
	const mmal_buffer_header& header = buffer.buffer_header;
	uint32 context = buffer.drvbuf.client_context;

	if ((context & kInputContext) != 0) {
		// the firmware is through with a piece of input
		TRACE("input returned, length %" B_PRIu32 " flags %#" B_PRIx32 "\n",
			header.length, header.flags);
		BAutolock locker(fLock);
		if (fInputOutstanding > 0)
			fInputOutstanding--;
		release_sem(fWakeSem);
		return;
	}

	uint32 index = context & 0xff;
	if ((context & kOutputContext) == 0 || index >= fBufferCount)
		return;
	OutputBuffer& output = fBuffers[index];

	uint32 length = header.length;
	TRACE("output %" B_PRIu32 ": %" B_PRIu32 " bytes, flags %#" B_PRIx32
		", pts %" B_PRId64 "\n", index, length, header.flags, header.pts);

	if (length > fBufferSize) {
		_Fail("a picture of %" B_PRIu32 " bytes for a buffer of %" B_PRIu32,
			length, fBufferSize);
		length = 0;
	}

	vchiq_transfer_request request = {};
	request.handle = fService;
	if (length > 0 && buffer.payload_in_message > 0
		&& buffer.payload_in_message <= MMAL_SHORT_DATA) {
		memcpy(output.memory, buffer.short_data, buffer.payload_in_message);
	} else if (length > 0) {
		// the firmware has a bulk transfer waiting for us
		request.data = output.memory;
		request.size = align_up(length, 4);
	} else if ((header.flags & MMAL_BUFFER_FLAG_EOS) != 0) {
		// ... as it has with the empty buffer that ends the stream
		request.data = fScratch;
		request.size = 8;
	}
	if (request.size > 0 && ioctl(fDevice, VCHIQ_BULK_RECEIVE, &request,
			sizeof(request)) != 0) {
		_Fail("receiving a picture: %s", strerror(errno));
		length = 0;
	}

	BAutolock locker(fLock);
	if (length > 0) {
		output.state = BUFFER_READY;
		output.length = length;
		output.flags = header.flags;
		output.pts = header.pts;
		fReady[fReadyCount++] = index;
	} else {
		output.state = BUFFER_FREE;
		if ((header.flags & MMAL_BUFFER_FLAG_EOS) != 0)
			fEndOfStream = true;
	}
	release_sem(fWakeSem);
}


void
MmalDecoder::_EventToHost(const mmal_message& message)
{
	const mmal_event_to_host& event = message.u.event_to_host;
	TRACE("event %.4s on port type %" B_PRIu32 ", %" B_PRIu32 " bytes\n",
		(const char*)&event.cmd, event.port_type, event.length);

	if (event.cmd == MMAL_EVENT_FORMAT_CHANGED
		&& event.length >= sizeof(mmal_event_format_changed)) {
		BAutolock locker(fLock);
		memcpy(&fNewFormat, event.data, sizeof(fNewFormat));
		fFormatChanged = true;
		release_sem(fWakeSem);
	} else if (event.cmd == MMAL_EVENT_ERROR) {
		uint32 status = 0;
		memcpy(&status, event.data, sizeof(status));
		_Fail("the firmware's decoder reports error %" B_PRIu32, status);
	}
}


//	#pragma mark - output


void
MmalDecoder::_UpdateGeometry()
{
	const mmal_video_format& video = fOutput.es;
	fEncoding = fOutput.format.encoding;
	fStride = video.width;
	fSliceHeight = video.height;
	fWidth = video.crop.width > 0 ? (uint32)video.crop.width : video.width;
	fHeight = video.crop.height > 0 ? (uint32)video.crop.height
		: video.height;
}


void
MmalDecoder::_FreeOutputBuffers()
{
	for (uint32 i = 0; i < kMaxOutputBuffers; i++) {
		free(fBuffers[i].memory);
		fBuffers[i].memory = NULL;
		fBuffers[i].state = BUFFER_FREE;
	}
	fBufferCount = 0;
	fBufferSize = 0;
	fReadyCount = 0;
}


status_t
MmalDecoder::_AllocateOutputBuffers()
{
	uint32 count = fOutput.port.buffer_num;
	uint32 size = align_up(fOutput.port.buffer_size, 64);

	BAutolock locker(fLock);
	if (count == fBufferCount && size <= fBufferSize)
		return B_OK;

	_FreeOutputBuffers();
	for (uint32 i = 0; i < count; i++) {
		fBuffers[i].memory = (uint8*)malloc(size);
		if (fBuffers[i].memory == NULL) {
			_FreeOutputBuffers();
			return B_NO_MEMORY;
		}
	}
	fBufferCount = count;
	fBufferSize = size;
	return B_OK;
}


status_t
MmalDecoder::_SendOutputBuffer(int32 index)
{
	mmal_message message = {};
	message.h.magic = MMAL_MAGIC;
	message.h.type = MMAL_MSG_BUFFER_FROM_HOST;
	message.h.context = kOutputContext | index;

	mmal_buffer_from_host& buffer = message.u.buffer_from_host;
	buffer.drvbuf.magic = MMAL_MAGIC;
	buffer.drvbuf.component_handle = fComponent;
	buffer.drvbuf.port_handle = fOutput.handle;
	buffer.drvbuf.client_context = kOutputContext | index;
	buffer.buffer_header.data = kOutputContext | index;
	buffer.buffer_header.alloc_size = fOutput.port.buffer_size;
	buffer.buffer_header.pts = MMAL_TIME_UNKNOWN;
	buffer.buffer_header.dts = MMAL_TIME_UNKNOWN;
	return _Send(&message, sizeof(mmal_header) + sizeof(buffer));
}


void
MmalDecoder::_SendFreeOutputBuffers()
{
	for (uint32 i = 0; i < fBufferCount; i++) {
		BAutolock locker(fLock);
		if (!fOutputEnabled || fBuffers[i].state != BUFFER_FREE)
			continue;
		fBuffers[i].state = BUFFER_SENT;
		locker.Unlock();

		status_t status = _SendOutputBuffer(i);
		if (status != B_OK) {
			_Fail("sending an output buffer: %s", strerror(status));
			return;
		}
	}
}


/*!	Sets the output port up for the pictures of \a event, or for those the
	input was announced with: its format, its buffers, and on.
*/
status_t
MmalDecoder::_ConfigureOutput(const mmal_event_format_changed* event)
{
	bool wasEnabled;
	{
		BAutolock locker(fLock);
		wasEnabled = fOutputEnabled;
		fOutputEnabled = false;
	}

	if (wasEnabled) {
		status_t status = _PortAction(fOutput, MMAL_PORT_ACTION_DISABLE);
		if (status != B_OK) {
			_Fail("stopping the output port: %s", strerror(status));
			return status;
		}

		// the firmware hands the buffers back
		for (int32 tries = 0; tries < 200; tries++) {
			bool sent = false;
			BAutolock locker(fLock);
			for (uint32 i = 0; i < fBufferCount; i++) {
				if (fBuffers[i].state == BUFFER_READY)
					fBuffers[i].state = BUFFER_FREE;
				sent |= fBuffers[i].state == BUFFER_SENT;
			}
			fReadyCount = 0;
			locker.Unlock();
			if (!sent)
				break;
			snooze(5000);
		}
	}

	uint32 count = 0, size = 0;
	if (event != NULL) {
		fOutput.format = event->es_format;
		fOutput.es = event->es;
		count = max_c(event->buffer_num_min, event->buffer_num_recommended);
		size = max_c(event->buffer_size_min, event->buffer_size_recommended);
	} else {
		fOutput.format.type = MMAL_ES_TYPE_VIDEO;
		fOutput.es = fInput.es;
	}

	// three planes, as the decoder has them
	static const uint32 kEncodings[] = {MMAL_ENCODING_I420, MMAL_ENCODING_NV12};
	status_t status = B_ERROR;
	for (size_t i = 0; i < B_COUNT_OF(kEncodings); i++) {
		fOutput.format.encoding = kEncodings[i];
		fOutput.format.encoding_variant = 0;
		status = _SetPort(fOutput);
		if (status == B_OK)
			break;
	}
	if (status == B_OK)
		status = _GetPort(fOutput);
	if (status != B_OK) {
		_Fail("setting the output format: %s", strerror(status));
		return status;
	}

	count = max_c(count, max_c(fOutput.port.buffer_num_min,
		fOutput.port.buffer_num_recommended));
	fOutput.port.buffer_num = min_c(max_c(count, 3u),
		(uint32)kMaxOutputBuffers);
	fOutput.port.buffer_size = max_c(size, max_c(fOutput.port.buffer_size_min,
		fOutput.port.buffer_size_recommended));
	_UpdateGeometry();

	TRACE("output: %" B_PRIu32 "x%" B_PRIu32 " in %" B_PRIu32 "x%" B_PRIu32
		" %.4s, %" B_PRIu32 " buffers of %" B_PRIu32 " bytes\n", fWidth,
		fHeight, fStride, fSliceHeight, (const char*)&fEncoding,
		fOutput.port.buffer_num, fOutput.port.buffer_size);

	status = _AllocateOutputBuffers();
	if (status == B_OK)
		status = _PortAction(fOutput, MMAL_PORT_ACTION_ENABLE);
	if (status != B_OK) {
		_Fail("starting the output port: %s", strerror(status));
		return status;
	}

	{
		BAutolock locker(fLock);
		fOutputEnabled = true;
	}
	_SendFreeOutputBuffers();
	return B_OK;
}


//	#pragma mark - public


status_t
MmalDecoder::Open(uint32 width, uint32 height, bool framed,
	BString* _error)
{
	status_t status = B_OK;
	mmal_message message;
	vchiq_open_request service = {};

	fWakeSem = create_sem(0, "mmal wake");
	fReplySem = create_sem(0, "mmal reply");
	fScratch = (uint8*)malloc(64);
	if (fWakeSem < 0 || fReplySem < 0 || fScratch == NULL) {
		status = B_NO_MEMORY;
		goto failed;
	}

	// The firmware has memory for a few decoders (gpu_mem in config.txt)
	// and stops answering for good when it runs out: no more than two at a
	// time in the whole system. A port's name is the token; it goes away
	// with a team that dies.
	for (int32 i = 0; i < kMaxDecoders && fToken < 0; i++) {
		char name[B_OS_NAME_LENGTH];
		snprintf(name, sizeof(name), "rpi_mmal decoder %" B_PRId32, i);
		if (find_port(name) < 0)
			fToken = create_port(1, name);
	}
	if (fToken < 0) {
		status = B_BUSY;
		fError = "the hardware decoder is in use";
		goto failed;
	}

	fDevice = open(VCHIQ_DEVICE_PATH, O_RDWR);
	if (fDevice < 0) {
		status = errno;
		fError.SetToFormat("%s: %s", VCHIQ_DEVICE_PATH, strerror(status));
		goto failed;
	}

	service.fourcc = VCHIQ_FOURCC('m', 'm', 'a', 'l');
	service.version = MMAL_SERVICE_VERSION;
	service.min_version = MMAL_SERVICE_VERSION_MIN;
	if (ioctl(fDevice, VCHIQ_OPEN_SERVICE, &service, sizeof(service)) != 0) {
		status = errno;
		fError.SetToFormat("the firmware's mmal service: %s",
			strerror(status));
		goto failed;
	}
	fService = service.handle;

	fReader = spawn_thread(_ReaderEntry, "mmal reader", B_DISPLAY_PRIORITY,
		this);
	if (fReader < 0) {
		status = fReader;
		goto failed;
	}
	resume_thread(fReader);

	message = {};
	message.h.type = MMAL_MSG_COMPONENT_CREATE;
	message.u.component_create.client_component = 1;
	message.u.component_create.pid = getpid();
	strlcpy(message.u.component_create.name, "ril.video_decode",
		sizeof(message.u.component_create.name));
	status = _Request(message, sizeof(mmal_component_create),
		sizeof(mmal_component_create_reply));
	if (status != B_OK) {
		fError.SetToFormat("the firmware made no video decoder (%s); it "
			"needs the start4.elf with the codecs and enough gpu_mem",
			strerror(status));
		goto failed;
	}
	fComponent = message.u.component_create_reply.component_handle;

	fControl.type = MMAL_PORT_TYPE_CONTROL;
	fInput.type = MMAL_PORT_TYPE_INPUT;
	fOutput.type = MMAL_PORT_TYPE_OUTPUT;
	status = _GetPort(fControl);
	if (status == B_OK)
		status = _GetPort(fInput);
	if (status == B_OK)
		status = _GetPort(fOutput);
	if (status != B_OK) {
		fError.SetToFormat("the decoder's ports: %s", strerror(status));
		goto failed;
	}

	// the input: H.264, in whole access units or as it comes
	fInput.format.type = MMAL_ES_TYPE_VIDEO;
	fInput.format.encoding = MMAL_ENCODING_H264;
	fInput.format.encoding_variant = 0;
	fInput.format.bitrate = 0;
	fInput.format.flags = framed ? MMAL_ES_FORMAT_FLAG_FRAMED : 0;
	memset(&fInput.es, 0, sizeof(fInput.es));
	fInput.es.width = align_up(width, 32);
	fInput.es.height = align_up(height, 16);
	fInput.es.crop.width = width;
	fInput.es.crop.height = height;
	fInput.es.frame_rate.numerator = 0;
	fInput.es.frame_rate.denominator = 1;
	fInput.es.par.numerator = 1;
	fInput.es.par.denominator = 1;
	status = _SetPort(fInput);
	if (status == B_OK)
		status = _GetPort(fInput);
	if (status != B_OK) {
		fError.SetToFormat("the firmware refuses H.264 of %" B_PRIu32 "x%"
			B_PRIu32 ": %s", width, height, strerror(status));
		goto failed;
	}
	fInput.port.buffer_num = max_c(fInput.port.buffer_num_min,
		(uint32)kInputBuffers + kInputSpare);
	fInput.port.buffer_size = max_c(fInput.port.buffer_size_min,
		fInput.port.buffer_size_recommended);
	fInputSize = fInput.port.buffer_size;
	fInputCopy = (uint8*)malloc(fInputSize + 4);
	if (fInputCopy == NULL) {
		status = B_NO_MEMORY;
		goto failed;
	}
	TRACE("input: %" B_PRIu32 " buffers of %" B_PRIu32 " bytes (the firmware "
		"asks for %" B_PRIu32 " to %" B_PRIu32 " of %" B_PRIu32 " to %"
		B_PRIu32 ")\n", fInput.port.buffer_num, fInput.port.buffer_size,
		fInput.port.buffer_num_min, fInput.port.buffer_num_recommended,
		fInput.port.buffer_size_min, fInput.port.buffer_size_recommended);

	// time stamps as they went in, or none
	_SetParameter(fOutput, MMAL_PARAMETER_VIDEO_INTERPOLATE_TIMESTAMPS, 0);

	status = _PortAction(fControl, MMAL_PORT_ACTION_ENABLE);
	if (status == B_OK)
		status = _PortAction(fInput, MMAL_PORT_ACTION_ENABLE);
	if (status != B_OK) {
		fError.SetToFormat("starting the decoder's input: %s",
			strerror(status));
		goto failed;
	}

	status = _ConfigureOutput(NULL);
	if (status != B_OK)
		goto failed;

	message = {};
	message.h.type = MMAL_MSG_COMPONENT_ENABLE;
	message.u.component.component_handle = fComponent;
	status = _Request(message, sizeof(mmal_component_request),
		sizeof(mmal_status_reply));
	if (status != B_OK) {
		fError.SetToFormat("starting the decoder: %s", strerror(status));
		goto failed;
	}
	fComponentEnabled = true;
	return B_OK;

failed:
	if (_error != NULL)
		*_error = fError.Length() > 0 ? fError.String() : strerror(status);
	Close();
	return status;
}


void
MmalDecoder::Close()
{
	if (fComponent != 0 && fReader >= 0) {
		{
			BAutolock locker(fLock);
			fOutputEnabled = false;
		}
		_PortAction(fOutput, MMAL_PORT_ACTION_DISABLE);
		_PortAction(fInput, MMAL_PORT_ACTION_DISABLE);
		_PortAction(fControl, MMAL_PORT_ACTION_DISABLE);

		mmal_message message = {};
		if (fComponentEnabled) {
			message.h.type = MMAL_MSG_COMPONENT_DISABLE;
			message.u.component.component_handle = fComponent;
			_Request(message, sizeof(mmal_component_request),
				sizeof(mmal_status_reply));
		}
		message = {};
		message.h.type = MMAL_MSG_COMPONENT_DESTROY;
		message.u.component.component_handle = fComponent;
		_Request(message, sizeof(mmal_component_request),
			sizeof(mmal_status_reply));
	}
	fComponent = 0;
	fComponentEnabled = false;

	fQuit = true;
	if (fReader >= 0) {
		status_t result;
		wait_for_thread(fReader, &result);
		fReader = -1;
	}
	if (fDevice >= 0) {
		// closes the service with it
		close(fDevice);
		fDevice = -1;
	}

	if (fToken >= 0)
		delete_port(fToken);
	fToken = -1;

	_FreeOutputBuffers();
	free(fScratch);
	fScratch = NULL;
	free(fInputCopy);
	fInputCopy = NULL;
	if (fWakeSem >= 0)
		delete_sem(fWakeSem);
	if (fReplySem >= 0)
		delete_sem(fReplySem);
	fWakeSem = fReplySem = -1;
}


bool
MmalDecoder::CanSend() const
{
	return fInputOutstanding < kInputBuffers;
}


status_t
MmalDecoder::Send(const void* data, size_t size, int64 pts, uint32 flags)
{
	if (size > fInputSize)
		return B_BAD_VALUE;

	{
		BAutolock locker(fLock);
		if (fStatus != B_OK)
			return fStatus;
		if (fInputOutstanding >= kInputBuffers)
			return B_WOULD_BLOCK;
		fInputOutstanding++;
	}
	TRACE("input: %" B_PRIuSIZE " bytes, flags %#" B_PRIx32 ", %" B_PRId32
		" with the firmware\n", size, flags, fInputOutstanding);

	mmal_message message = {};
	message.h.magic = MMAL_MAGIC;
	message.h.type = MMAL_MSG_BUFFER_FROM_HOST;
	message.h.context = kInputContext;

	mmal_buffer_from_host& buffer = message.u.buffer_from_host;
	buffer.drvbuf.magic = MMAL_MAGIC;
	buffer.drvbuf.component_handle = fComponent;
	buffer.drvbuf.port_handle = fInput.handle;
	buffer.drvbuf.client_context = kInputContext;
	buffer.buffer_header.data = kInputContext;
	buffer.buffer_header.alloc_size = fInputSize;
	buffer.buffer_header.length = size;
	buffer.buffer_header.flags = flags;
	buffer.buffer_header.pts = pts;
	buffer.buffer_header.dts = MMAL_TIME_UNKNOWN;
	if (size <= MMAL_SHORT_DATA) {
		buffer.payload_in_message = size;
		memcpy(buffer.short_data, data, size);
	}

	status_t status = _Send(&message, sizeof(mmal_header) + sizeof(buffer));
	if (status == B_OK && size > MMAL_SHORT_DATA) {
		// The firmware takes the data in a bulk transfer of a multiple of
		// four bytes (and cancels one of another size).
		vchiq_transfer_request request = {};
		request.handle = fService;
		request.data = (void*)data;
		request.size = align_up(size, 4);
		if (request.size != size) {
			memcpy(fInputCopy, data, size);
			memset(fInputCopy + size, 0, request.size - size);
			request.data = fInputCopy;
		}
		if (ioctl(fDevice, VCHIQ_BULK_TRANSMIT, &request, sizeof(request))
				!= 0) {
			status = errno;
		}
	}
	if (status != B_OK)
		_Fail("sending input: %s", strerror(status));
	return status;
}


status_t
MmalDecoder::SendEndOfStream()
{
	return Send(NULL, 0, MMAL_TIME_UNKNOWN, MMAL_BUFFER_FLAG_EOS);
}


status_t
MmalDecoder::NextFrame(Frame& frame, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		BAutolock locker(fLock);
		if (fStatus != B_OK)
			return fStatus;

		if (fReadyCount > 0) {
			int32 index = fReady[0];
			fReadyCount--;
			memmove(&fReady[0], &fReady[1], fReadyCount * sizeof(fReady[0]));

			OutputBuffer& buffer = fBuffers[index];
			buffer.state = BUFFER_HELD;
			frame.data = buffer.memory;
			frame.length = buffer.length;
			frame.pts = buffer.pts;
			frame.flags = buffer.flags;
			frame.index = index;
			if ((buffer.flags & MMAL_BUFFER_FLAG_EOS) != 0)
				fEndOfStream = true;
			return B_OK;
		}

		if (fFormatChanged) {
			// only now: pictures of the old format came first
			mmal_event_format_changed event = fNewFormat;
			fFormatChanged = false;
			locker.Unlock();

			status_t status = _ConfigureOutput(&event);
			if (status != B_OK)
				return status;
			continue;
		}

		if (fEndOfStream)
			return B_LAST_BUFFER_ERROR;
		locker.Unlock();

		_SendFreeOutputBuffers();

		bigtime_t now = system_time();
		if (now >= deadline)
			return B_TIMED_OUT;
		acquire_sem_etc(fWakeSem, 1, B_RELATIVE_TIMEOUT, deadline - now);
	}
}


void
MmalDecoder::ReleaseFrame(const Frame& frame)
{
	{
		BAutolock locker(fLock);
		if (frame.index < 0 || (uint32)frame.index >= fBufferCount
			|| fBuffers[frame.index].state != BUFFER_HELD) {
			return;
		}
		fBuffers[frame.index].state = BUFFER_FREE;
	}
	_SendFreeOutputBuffers();
}


void
MmalDecoder::Wait(bigtime_t timeout)
{
	acquire_sem_etc(fWakeSem, 1, B_RELATIVE_TIMEOUT, timeout);
}


status_t
MmalDecoder::Flush()
{
	{
		BAutolock locker(fLock);
		if (fStatus != B_OK)
			return fStatus;
		// buffers that come back stay with us until the ports are clear
		fOutputEnabled = false;
	}

	status_t status = _PortAction(fInput, MMAL_PORT_ACTION_FLUSH);
	if (status == B_OK)
		status = _PortAction(fOutput, MMAL_PORT_ACTION_FLUSH);
	if (status != B_OK) {
		_Fail("flushing the decoder: %s", strerror(status));
		return status;
	}

	for (int32 tries = 0; tries < 200; tries++) {
		BAutolock locker(fLock);
		bool sent = fInputOutstanding > 0;
		for (uint32 i = 0; i < fBufferCount; i++) {
			if (fBuffers[i].state == BUFFER_READY)
				fBuffers[i].state = BUFFER_FREE;
			sent |= fBuffers[i].state == BUFFER_SENT;
		}
		fReadyCount = 0;
		locker.Unlock();
		if (!sent)
			break;
		snooze(5000);
	}

	{
		BAutolock locker(fLock);
		fInputOutstanding = 0;
		fEndOfStream = false;
		fOutputEnabled = true;
	}
	_SendFreeOutputBuffers();
	return B_OK;
}
