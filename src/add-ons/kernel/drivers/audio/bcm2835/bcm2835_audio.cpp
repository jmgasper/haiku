/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Sound output of the Raspberry Pi: the VideoCore firmware's sound service
	("AUDS") through VCHIQ. The firmware does the work (the HDMI outputs'
	audio packets, the PWM of the headphone jack, resampling, volume); this
	driver hands it the samples and takes its pace from the firmware's
	reports of what it has consumed.

	The messages of the service are those of Broadcom's
	vc_vchi_audioserv_defs.h. */


#include <Drivers.h>
#include <KernelExport.h>

#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <hmulti_audio.h>
#include <vchiq.h>


#define ERROR(x...)	dprintf("bcm2835_audio: " x)
//#define TRACE_PACE
	// reports the pace of the firmware every ten seconds

#define DEVICE_NAME			"audio/hmulti/bcm2835/0"

#define BUFFER_COUNT		2
#define BUFFER_FRAMES		960
	// 20 ms at 48 kHz, and less than the 4000 bytes a message of samples
	// may have
#define CHANNELS			2
#define FRAME_SIZE			(CHANNELS * 2)
#define BUFFER_BYTES		(BUFFER_FRAMES * FRAME_SIZE)

#define RESULT_TIMEOUT		2000000LL

// the service's messages
enum {
	AUDIO_RESULT,
	AUDIO_COMPLETE,
	AUDIO_CONFIG,
	AUDIO_CONTROL,
	AUDIO_OPEN,
	AUDIO_CLOSE,
	AUDIO_START,
	AUDIO_STOP,
	AUDIO_WRITE
};

#define AUDIO_WRITE_COOKIE1	VCHIQ_FOURCC('B', 'C', 'M', 'A')
#define AUDIO_WRITE_COOKIE2	VCHIQ_FOURCC('D', 'A', 'T', 'A')
#define AUDIO_MAX_PACKET	4000
#define AUDIO_COUNT_MASK	0x3fffffff
	// the bits above flag an underrun

struct audio_message {
	int32	type;
	union {
		struct {
			uint32	channels;
			uint32	samplerate;
			uint32	bps;
		} config;
		struct {
			uint32	volume;		// attenuation in 1/256 dB
			uint32	dest;
		} control;
		struct {
			uint32	count;
			uint32	cookie1;
			uint32	cookie2;
			uint16	silence;
			uint16	max_packet;
		} write;
		struct {
			int32	success;
		} result;
		struct {
			int32	count;
			uint32	cookie1;
			uint32	cookie2;
		} complete;
	};
};

// mixer controls
enum {
	CONTROL_GROUP = 1024,
	CONTROL_VOLUME_LEFT,
	CONTROL_VOLUME_RIGHT,
	CONTROL_DESTINATION,
	CONTROL_DESTINATION_FIRST
};

static const char* const kDestinations[] = {
	"Automatic", "Headphone jack", "HDMI 0", "HDMI 1"
};
#define DESTINATION_COUNT	B_COUNT_OF(kDestinations)

#define MIN_GAIN			-60.0f
#define MAX_GAIN			0.0f

struct audio_device {
	mutex			lock;
	int32			openCount;
	vchiq_service*	service;
	bool			serviceClosed;

	sem_id			resultSem;
	int32			result;
	sem_id			completeSem;
	int32			pending;
		// bytes the firmware has and did not play yet
	int32			underruns;

	area_id			bufferArea;
	uint8*			buffers[BUFFER_COUNT];
	sem_id			readySem;
	thread_id		thread;
	bool			running;

	spinlock		timeLock;
	int32			playingBuffer;
	bigtime_t		realTime;
	uint64			framesCount;

	uint32			rate;
	float			gain;
	uint32			destination;
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static vchiq_module_info* sVchiq;
static audio_device sDevice;
static const char* sDeviceNames[] = { DEVICE_NAME, NULL };


//	#pragma mark - the firmware's service


static void
service_hook(void* cookie, uint32 event, const void* data, size_t size)
{
	audio_device* device = (audio_device*)cookie;

	if (event == VCHIQ_EVENT_CLOSED) {
		device->serviceClosed = true;
		release_sem(device->completeSem);
		return;
	}

	const audio_message* message = (const audio_message*)data;
	if (size < 2 * sizeof(int32))
		return;

	if (message->type == AUDIO_RESULT) {
		device->result = message->result.success;
		release_sem(device->resultSem);
	} else if (message->type == AUDIO_COMPLETE) {
		int32 count = message->complete.count;
		if (count < 0)
			return;
		if ((count & ~AUDIO_COUNT_MASK) != 0)
			atomic_add(&device->underruns, 1);
		atomic_add(&device->pending, -(count & AUDIO_COUNT_MASK));
		release_sem(device->completeSem);
	}
}


static status_t
send_command(audio_device* device, audio_message& message, bool waitForResult)
{
	if (waitForResult) {
		// drop a late answer to an earlier command
		while (acquire_sem_etc(device->resultSem, 1, B_RELATIVE_TIMEOUT, 0)
				== B_OK) {
		}
	}

	status_t status = sVchiq->queue_message(device->service, &message,
		sizeof(message), false);
	if (status != B_OK || !waitForResult)
		return status;

	status = acquire_sem_etc(device->resultSem, 1, B_RELATIVE_TIMEOUT,
		RESULT_TIMEOUT);
	if (status != B_OK)
		return status;
	return device->result == 0 ? B_OK : B_ERROR;
}


static status_t
send_control(audio_device* device)
{
	audio_message message = {};
	message.type = AUDIO_CONTROL;
	message.control.dest = device->destination;
	message.control.volume = (uint32)(-device->gain * 256);
	return send_command(device, message, true);
}


static status_t
open_service(audio_device* device)
{
	device->serviceClosed = false;
	status_t status = sVchiq->open_service(VCHIQ_FOURCC('A', 'U', 'D', 'S'),
		2, 1, service_hook, device, &device->service);
	if (status != B_OK) {
		ERROR("the firmware's sound service: %s\n", strerror(status));
		return status;
	}

	if (sVchiq->peer_version(device->service) < 2) {
		// samples as messages need version 2
		ERROR("the firmware's sound service is too old\n");
		sVchiq->close_service(device->service);
		device->service = NULL;
		return B_NOT_SUPPORTED;
	}

	audio_message message = {};
	message.type = AUDIO_OPEN;
	status = send_command(device, message, false);
	if (status == B_OK)
		status = send_control(device);
	if (status != B_OK) {
		sVchiq->close_service(device->service);
		device->service = NULL;
	}
	return status;
}


static void
close_service(audio_device* device)
{
	if (device->service == NULL)
		return;

	audio_message message = {};
	message.type = AUDIO_CLOSE;
	send_command(device, message, true);

	sVchiq->close_service(device->service);
	device->service = NULL;
}


//	#pragma mark - playback


static status_t
write_samples(audio_device* device, const void* samples)
{
	audio_message message = {};
	message.type = AUDIO_WRITE;
	message.write.count = BUFFER_BYTES;
	message.write.cookie1 = AUDIO_WRITE_COOKIE1;
	message.write.cookie2 = AUDIO_WRITE_COOKIE2;
	message.write.max_packet = AUDIO_MAX_PACKET;
	status_t status = sVchiq->queue_message(device->service, &message,
		sizeof(message), false);
	if (status == B_OK) {
		status = sVchiq->queue_message(device->service, samples,
			BUFFER_BYTES, false);
	}
	if (status == B_OK)
		atomic_add(&device->pending, BUFFER_BYTES);
	return status;
}


static int32
playback_thread(void* data)
{
	audio_device* device = (audio_device*)data;
	int32 index = 0;

	// One buffer of silence ahead: from the first buffer on the firmware
	// then asks for one every 20 ms, and the time stamps for the node's
	// clock are that far apart (two at once make that clock race).
	static const uint8 kSilence[BUFFER_BYTES] = {};
	write_samples(device, kSilence);
#ifdef TRACE_PACE
	bigtime_t traceStart = system_time();
	uint64 traceFrames = 0;
	int32 traceCount = 0;
#endif

	while (device->running && !device->serviceClosed) {
		// Wait until the firmware is down to one buffer: with the one that
		// follows it has 20 to 40 ms to play.
		while (device->running && !device->serviceClosed
			&& atomic_get(&device->pending) > BUFFER_BYTES) {
			status_t waitStatus = acquire_sem_etc(device->completeSem, 1,
				B_RELATIVE_TIMEOUT, 100000);
#ifdef TRACE_PACE
			if (waitStatus != B_OK) {
				dprintf("bcm2835_audio: waiting: %s, %" B_PRId32 " pending\n",
					strerror(waitStatus), atomic_get(&device->pending));
			}
#else
			(void)waitStatus;
#endif
		}
		if (!device->running || device->serviceClosed)
			break;

		status_t status = write_samples(device, device->buffers[index]);
		if (status != B_OK) {
			ERROR("writing samples: %s\n", strerror(status));
			snooze(20000);
			continue;
		}

		// this buffer is on its way; the other one is to be filled
		InterruptsSpinLocker locker(device->timeLock);
		device->playingBuffer = index;
		device->realTime = system_time();
		device->framesCount += BUFFER_FRAMES;
		locker.Unlock();
		release_sem_etc(device->readySem, 1, B_DO_NOT_RESCHEDULE);

		index = (index + 1) % BUFFER_COUNT;

#ifdef TRACE_PACE
		traceFrames += BUFFER_FRAMES;
		if (++traceCount == 500) {
			bigtime_t now = system_time();
			dprintf("bcm2835_audio: %" B_PRIu64 " frames in %" B_PRIdBIGTIME
				" us, %" B_PRId32 " bytes pending, %" B_PRId32
				" underruns\n", traceFrames, now - traceStart,
				atomic_get(&device->pending),
				atomic_get(&device->underruns));
			traceStart = now;
			traceFrames = 0;
			traceCount = 0;
		}
#endif
	}

	return 0;
}


static status_t
start_playback(audio_device* device)
{
	MutexLocker locker(device->lock);
	if (device->running)
		return B_OK;
	if (device->service == NULL || device->serviceClosed)
		return B_DEV_NOT_READY;

	audio_message message = {};
	message.type = AUDIO_CONFIG;
	message.config.channels = CHANNELS;
	message.config.samplerate = device->rate;
	message.config.bps = 16;
	status_t status = send_command(device, message, true);
	if (status == B_OK)
		status = send_control(device);
	if (status == B_OK) {
		message = {};
		message.type = AUDIO_START;
		status = send_command(device, message, false);
	}
	if (status != B_OK) {
		ERROR("starting the firmware's output: %s\n", strerror(status));
		return status;
	}

	// a fresh count for the node's clock, and no buffer of the last run
	while (acquire_sem_etc(device->readySem, 1, B_RELATIVE_TIMEOUT, 0)
			== B_OK) {
	}
	atomic_set(&device->pending, 0);
	device->framesCount = 0;
	device->realTime = 0;
	device->running = true;
	device->thread = spawn_kernel_thread(playback_thread, "bcm2835 audio",
		B_REAL_TIME_PRIORITY, device);
	if (device->thread < 0) {
		device->running = false;
		return device->thread;
	}
	resume_thread(device->thread);
	return B_OK;
}


static void
stop_playback(audio_device* device)
{
	MutexLocker locker(device->lock);
	if (!device->running)
		return;

	device->running = false;
	release_sem(device->completeSem);
	status_t result;
	wait_for_thread(device->thread, &result);

	if (!device->serviceClosed) {
		audio_message message = {};
		message.type = AUDIO_STOP;
		send_command(device, message, false);
	}
}


static void
free_buffers(audio_device* device)
{
	if (device->bufferArea >= 0) {
		delete_area(device->bufferArea);
		device->bufferArea = -1;
	}
}


//	#pragma mark - multi audio


static const multi_channel_info kChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO }
};


static status_t
get_description(multi_description* userData)
{
	multi_description description;
	if (user_memcpy(&description, userData, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;

	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	strlcpy(description.friendly_name, "Raspberry Pi HDMI and headphones",
		sizeof(description.friendly_name));
	strlcpy(description.vendor_info, "Broadcom VideoCore",
		sizeof(description.vendor_info));
	description.output_channel_count = CHANNELS;
	description.input_channel_count = 0;
	description.output_bus_channel_count = CHANNELS;
	description.input_bus_channel_count = 0;
	description.aux_bus_channel_count = 0;
	description.output_rates = B_SR_44100 | B_SR_48000;
	description.input_rates = 0;
	description.max_cvsr_rate = 0;
	description.min_cvsr_rate = 0;
	description.output_formats = B_FMT_16BIT;
	description.input_formats = 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	description.start_latency = 40000;
	description.control_panel[0] = 0;

	if (user_memcpy(userData, &description, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	if ((size_t)description.request_channel_count >= B_COUNT_OF(kChannels)
		&& user_memcpy(description.channels, kChannels, sizeof(kChannels))
			!= B_OK) {
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
get_enabled_channels(multi_channel_enable* userData)
{
	multi_channel_enable data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;

	uint8 bits = 0x3;
	if (!IS_USER_ADDRESS(data.enable_bits)
		|| user_memcpy(data.enable_bits, &bits, 1) != B_OK) {
		return B_BAD_ADDRESS;
	}
	data.lock_source = B_MULTI_LOCK_INTERNAL;
	return user_memcpy(userData, &data, sizeof(data));
}


static status_t
get_global_format(audio_device* device, multi_format_info* userData)
{
	multi_format_info data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;

	data.output_latency = 0;
	data.input_latency = 0;
	data.timecode_kind = 0;
	data.input.rate = 0;
	data.input.cvsr = 0;
	data.input.format = 0;
	data.output.rate = device->rate == 44100 ? B_SR_44100 : B_SR_48000;
	data.output.cvsr = 0;
	data.output.format = B_FMT_16BIT;
	return user_memcpy(userData, &data, sizeof(data));
}


static status_t
set_global_format(audio_device* device, multi_format_info* userData)
{
	multi_format_info data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;

	uint32 rate;
	if (data.output.rate == B_SR_48000)
		rate = 48000;
	else if (data.output.rate == B_SR_44100)
		rate = 44100;
	else
		return B_BAD_VALUE;
	if (data.output.format != B_FMT_16BIT)
		return B_BAD_VALUE;

	if (rate != device->rate) {
		stop_playback(device);
		device->rate = rate;
	}
	return B_OK;
}


static status_t
get_buffers(audio_device* device, multi_buffer_list* userData)
{
	multi_buffer_list data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;
	if (data.request_playback_buffers < BUFFER_COUNT
		|| data.request_playback_channels < CHANNELS
		|| !IS_USER_ADDRESS(data.playback_buffers)) {
		return B_BAD_VALUE;
	}

	buffer_desc* userBuffers[BUFFER_COUNT];
	if (user_memcpy(userBuffers, data.playback_buffers, sizeof(userBuffers))
			!= B_OK) {
		return B_BAD_ADDRESS;
	}

	stop_playback(device);
	free_buffers(device);

	void* address;
	device->bufferArea = create_area("bcm2835 audio buffers", &address,
		B_ANY_KERNEL_ADDRESS, ROUNDUP(BUFFER_COUNT * BUFFER_BYTES, B_PAGE_SIZE),
		B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA
			| B_KERNEL_WRITE_AREA);
	if (device->bufferArea < 0)
		return device->bufferArea;
	memset(address, 0, BUFFER_COUNT * BUFFER_BYTES);

	for (uint32 i = 0; i < BUFFER_COUNT; i++) {
		device->buffers[i] = (uint8*)address + i * BUFFER_BYTES;

		buffer_desc descriptors[CHANNELS];
		for (uint32 channel = 0; channel < CHANNELS; channel++) {
			descriptors[channel].base = (char*)device->buffers[i]
				+ channel * 2;
			descriptors[channel].stride = FRAME_SIZE;
		}
		if (!IS_USER_ADDRESS(userBuffers[i])
			|| user_memcpy(userBuffers[i], descriptors, sizeof(descriptors))
				!= B_OK) {
			free_buffers(device);
			return B_BAD_ADDRESS;
		}
	}

	data.flags = B_MULTI_BUFFER_PLAYBACK;
	data.return_playback_buffers = BUFFER_COUNT;
	data.return_playback_channels = CHANNELS;
	data.return_playback_buffer_size = BUFFER_FRAMES;
	data.return_record_buffers = 0;
	data.return_record_channels = 0;
	data.return_record_buffer_size = 0;
	if (user_memcpy(userData, &data, sizeof(data)) != B_OK) {
		free_buffers(device);
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
buffer_exchange(audio_device* device, multi_buffer_info* userData)
{
	if (device->bufferArea < 0)
		return B_NO_INIT;

	status_t status = start_playback(device);
	if (status != B_OK)
		return status;

	status = acquire_sem_etc(device->readySem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 1000000);
	if (status != B_OK)
		return status;

	multi_buffer_info info;
	if (user_memcpy(&info, userData, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;

	InterruptsSpinLocker locker(device->timeLock);
	info.playback_buffer_cycle = device->playingBuffer;
	info.played_real_time = device->realTime;
	info.played_frames_count = device->framesCount;
	locker.Unlock();
	info.flags = B_MULTI_BUFFER_PLAYBACK;

	return user_memcpy(userData, &info, sizeof(info));
}


static status_t
list_mix_controls(multi_mix_control_info* userData)
{
	multi_mix_control_info info;
	if (user_memcpy(&info, userData, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;

	multi_mix_control controls[4 + DESTINATION_COUNT];
	memset(controls, 0, sizeof(controls));
	uint32 count = 0;

	multi_mix_control* control = &controls[count++];
	control->id = CONTROL_GROUP;
	control->flags = B_MULTI_MIX_GROUP;
	control->string = S_OUTPUT;

	control = &controls[count++];
	control->id = CONTROL_VOLUME_LEFT;
	control->parent = CONTROL_GROUP;
	control->flags = B_MULTI_MIX_GAIN;
	control->gain.min_gain = MIN_GAIN;
	control->gain.max_gain = MAX_GAIN;
	control->gain.granularity = 1.0f;
	control->string = S_VOLUME;

	// the firmware has one volume: the right channel follows the left
	control = &controls[count++];
	*control = controls[count - 2];
	control->id = CONTROL_VOLUME_RIGHT;
	control->master = CONTROL_VOLUME_LEFT;

	control = &controls[count++];
	control->id = CONTROL_DESTINATION;
	control->parent = CONTROL_GROUP;
	control->flags = B_MULTI_MIX_MUX;
	strlcpy(control->name, "Output", sizeof(control->name));

	for (uint32 i = 0; i < DESTINATION_COUNT; i++) {
		control = &controls[count++];
		control->id = CONTROL_DESTINATION_FIRST + i;
		control->parent = CONTROL_DESTINATION;
		control->flags = B_MULTI_MIX_MUX_VALUE;
		strlcpy(control->name, kDestinations[i], sizeof(control->name));
	}

	if ((uint32)info.control_count < count)
		count = info.control_count;
	if (count > 0 && (!IS_USER_ADDRESS(info.controls)
			|| user_memcpy(info.controls, controls,
				count * sizeof(multi_mix_control)) != B_OK)) {
		return B_BAD_ADDRESS;
	}
	info.control_count = count;
	return user_memcpy(userData, &info, sizeof(info));
}


static status_t
get_or_set_mix(audio_device* device, multi_mix_value_info* userData, bool set)
{
	multi_mix_value_info info;
	if (user_memcpy(&info, userData, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;
	if (info.item_count < 0 || info.item_count > 32
		|| !IS_USER_ADDRESS(info.values)) {
		return B_BAD_VALUE;
	}

	MutexLocker locker(device->lock);
	bool changed = false;

	for (int32 i = 0; i < info.item_count; i++) {
		multi_mix_value value;
		if (user_memcpy(&value, &info.values[i], sizeof(value)) != B_OK)
			return B_BAD_ADDRESS;

		switch (value.id) {
			case CONTROL_VOLUME_LEFT:
			case CONTROL_VOLUME_RIGHT:
				if (set) {
					float gain = value.gain;
					if (gain < MIN_GAIN)
						gain = MIN_GAIN;
					if (gain > MAX_GAIN)
						gain = MAX_GAIN;
					changed |= gain != device->gain;
					device->gain = gain;
				} else
					value.gain = device->gain;
				break;

			case CONTROL_DESTINATION:
				if (set) {
					if (value.mux >= DESTINATION_COUNT)
						return B_BAD_VALUE;
					changed |= value.mux != device->destination;
					device->destination = value.mux;
				} else
					value.mux = device->destination;
				break;

			default:
				continue;
		}

		if (!set && user_memcpy(&info.values[i], &value, sizeof(value))
				!= B_OK) {
			return B_BAD_ADDRESS;
		}
	}

	if (changed && device->service != NULL && !device->serviceClosed) {
		status_t status = send_control(device);
		if (status != B_OK)
			ERROR("volume and output: %s\n", strerror(status));
	}
	return B_OK;
}


//	#pragma mark - device


static status_t
audio_open(const char* name, uint32 flags, void** _cookie)
{
	audio_device* device = &sDevice;

	MutexLocker locker(device->lock);
	if (device->openCount > 0)
		return B_BUSY;

	status_t status = open_service(device);
	if (status != B_OK)
		return status;

	device->openCount++;
	*_cookie = device;
	return B_OK;
}


static status_t
audio_close(void* cookie)
{
	stop_playback((audio_device*)cookie);
	return B_OK;
}


static status_t
audio_free(void* cookie)
{
	audio_device* device = (audio_device*)cookie;

	stop_playback(device);

	MutexLocker locker(device->lock);
	close_service(device);
	free_buffers(device);
	device->openCount--;
	return B_OK;
}


static status_t
audio_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	audio_device* device = (audio_device*)cookie;

	switch (op) {
		case B_MULTI_GET_DESCRIPTION:
			return get_description((multi_description*)buffer);
		case B_MULTI_GET_ENABLED_CHANNELS:
			return get_enabled_channels((multi_channel_enable*)buffer);
		case B_MULTI_SET_ENABLED_CHANNELS:
			return B_OK;
		case B_MULTI_GET_GLOBAL_FORMAT:
			return get_global_format(device, (multi_format_info*)buffer);
		case B_MULTI_SET_GLOBAL_FORMAT:
			return set_global_format(device, (multi_format_info*)buffer);
		case B_MULTI_GET_BUFFERS:
			return get_buffers(device, (multi_buffer_list*)buffer);
		case B_MULTI_BUFFER_EXCHANGE:
			return buffer_exchange(device, (multi_buffer_info*)buffer);
		case B_MULTI_BUFFER_FORCE_STOP:
			stop_playback(device);
			return B_OK;
		case B_MULTI_LIST_MIX_CONTROLS:
			return list_mix_controls((multi_mix_control_info*)buffer);
		case B_MULTI_GET_MIX:
			return get_or_set_mix(device, (multi_mix_value_info*)buffer,
				false);
		case B_MULTI_SET_MIX:
			return get_or_set_mix(device, (multi_mix_value_info*)buffer,
				true);

		case B_MULTI_GET_EVENT_INFO:
		case B_MULTI_SET_EVENT_INFO:
		case B_MULTI_GET_EVENT:
		case B_MULTI_GET_CHANNEL_FORMATS:
		case B_MULTI_SET_CHANNEL_FORMATS:
		case B_MULTI_LIST_MIX_CHANNELS:
		case B_MULTI_LIST_MIX_CONNECTIONS:
		case B_MULTI_SET_BUFFERS:
		case B_MULTI_SET_START_TIME:
			return B_NOT_SUPPORTED;
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
audio_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_IO_ERROR;
}


static status_t
audio_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_IO_ERROR;
}


static device_hooks sHooks = {
	audio_open,
	audio_close,
	audio_free,
	audio_control,
	audio_read,
	audio_write
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
	if (status != B_OK) {
		put_module(VCHIQ_MODULE_NAME);
		return status;
	}

	audio_device* device = &sDevice;
	memset(device, 0, sizeof(*device));
	mutex_init(&device->lock, "bcm2835 audio");
	device->timeLock = B_SPINLOCK_INITIALIZER;
	device->bufferArea = -1;
	device->rate = 48000;
	device->gain = 0;
	device->destination = 0;
	device->resultSem = create_sem(0, "bcm2835 audio result");
	device->completeSem = create_sem(0, "bcm2835 audio complete");
	device->readySem = create_sem(0, "bcm2835 audio buffer");
	if (device->resultSem < 0 || device->completeSem < 0
		|| device->readySem < 0) {
		delete_sem(device->resultSem);
		delete_sem(device->completeSem);
		delete_sem(device->readySem);
		mutex_destroy(&device->lock);
		put_module(VCHIQ_MODULE_NAME);
		return B_NO_MORE_SEMS;
	}

	return B_OK;
}


void
uninit_driver()
{
	audio_device* device = &sDevice;
	delete_sem(device->resultSem);
	delete_sem(device->completeSem);
	delete_sem(device->readySem);
	mutex_destroy(&device->lock);
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
