/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_vchiq_audio <destination> <seconds> [frequency] [volume dB]
// Plays a sine tone through the firmware's sound service, straight over
// /dev/misc/vchiq (no sound driver, no media server), and reports how the
// firmware took the samples. Destinations: 0 automatic, 1 headphone jack,
// 2 HDMI0, 3 HDMI1.


#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <OS.h>

#include <vchiq.h>


enum {
	AUDIO_RESULT, AUDIO_COMPLETE, AUDIO_CONFIG, AUDIO_CONTROL, AUDIO_OPEN,
	AUDIO_CLOSE, AUDIO_START, AUDIO_STOP, AUDIO_WRITE
};

struct audio_message {
	int32	type;
	union {
		struct { uint32 channels, samplerate, bps; } config;
		struct { uint32 volume, dest; } control;
		struct { uint32 count, cookie1, cookie2; uint16 silence, max_packet; }
			write;
		struct { int32 success; } result;
		struct { int32 count; uint32 cookie1, cookie2; } complete;
		uint32 dummy;
	};
};

static int sDevice;
static uint32 sHandle;


static status_t
send(const void* data, size_t size)
{
	vchiq_transfer_request request = {};
	request.handle = sHandle;
	request.data = (void*)data;
	request.size = size;
	return ioctl(sDevice, VCHIQ_QUEUE_MESSAGE, &request, sizeof(request)) == 0
		? B_OK : errno;
}


static status_t
receive(audio_message& message, bigtime_t timeout)
{
	vchiq_transfer_request request = {};
	request.handle = sHandle;
	request.data = &message;
	request.size = sizeof(message);
	request.timeout = timeout;
	return ioctl(sDevice, VCHIQ_DEQUEUE_MESSAGE, &request, sizeof(request))
		== 0 ? B_OK : errno;
}


static status_t
command(audio_message& message, bool wait)
{
	status_t status = send(&message, sizeof(message));
	if (status != B_OK || !wait)
		return status;

	audio_message reply;
	while ((status = receive(reply, 2000000)) == B_OK) {
		if (reply.type == AUDIO_RESULT)
			return reply.result.success == 0 ? B_OK : B_ERROR;
	}
	return status;
}


int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <destination> <seconds> [Hz] [dB]\n",
			argv[0]);
		return 1;
	}
	uint32 destination = atoi(argv[1]);
	double seconds = atof(argv[2]);
	double frequency = argc > 3 ? atof(argv[3]) : 440;
	double attenuation = argc > 4 ? -atof(argv[4]) : 10;

	sDevice = open(VCHIQ_DEVICE_PATH, O_RDWR);
	if (sDevice < 0) {
		fprintf(stderr, "%s: %s\n", VCHIQ_DEVICE_PATH, strerror(errno));
		return 1;
	}

	vchiq_open_request open = {};
	open.fourcc = VCHIQ_FOURCC('A', 'U', 'D', 'S');
	open.version = 2;
	open.min_version = 1;
	if (ioctl(sDevice, VCHIQ_OPEN_SERVICE, &open, sizeof(open)) != 0) {
		fprintf(stderr, "opening the sound service: %s\n", strerror(errno));
		return 1;
	}
	sHandle = open.handle;
	printf("sound service open, the firmware's version is %d\n",
		open.peer_version);

	audio_message message = {};
	message.type = AUDIO_OPEN;
	status_t status = command(message, false);
	printf("open: %s\n", strerror(status));

	message = {};
	message.type = AUDIO_CONTROL;
	message.control.dest = destination;
	message.control.volume = (uint32)(attenuation * 256);
	status = command(message, true);
	printf("control (destination %" B_PRIu32 ", -%.1f dB): %s\n", destination,
		attenuation, strerror(status));

	message = {};
	message.type = AUDIO_CONFIG;
	message.config.channels = 2;
	message.config.samplerate = 48000;
	message.config.bps = 16;
	status = command(message, true);
	printf("config (2 channels, 48000 Hz, 16 bit): %s\n", strerror(status));

	message = {};
	message.type = AUDIO_START;
	status = command(message, false);
	printf("start: %s\n", strerror(status));

	// 1000 frames = 4000 bytes, the most one message of samples may carry
	const uint32 kFrames = 1000;
	int16 samples[kFrames * 2];
	uint64 written = 0, completed = 0;
	uint64 total = (uint64)(seconds * 48000) * 4;
	double phase = 0;
	bigtime_t start = system_time();
	bigtime_t lastReport = start;
	uint32 underruns = 0;

	while (completed < total) {
		// keep about 80 ms with the firmware
		while (written < total && written - completed < 4 * sizeof(samples)) {
			for (uint32 i = 0; i < kFrames; i++) {
				int16 value = (int16)(sin(phase) * 12000);
				phase += 2 * M_PI * frequency / 48000;
				samples[i * 2] = samples[i * 2 + 1] = value;
			}
			message = {};
			message.type = AUDIO_WRITE;
			message.write.count = sizeof(samples);
			message.write.max_packet = 4000;
			message.write.cookie1 = VCHIQ_FOURCC('B', 'C', 'M', 'A');
			message.write.cookie2 = VCHIQ_FOURCC('D', 'A', 'T', 'A');
			status = send(&message, sizeof(message));
			if (status == B_OK)
				status = send(samples, sizeof(samples));
			if (status != B_OK) {
				printf("write: %s\n", strerror(status));
				return 1;
			}
			written += sizeof(samples);
		}

		// The firmware reports what it has played in pieces of its own
		// choosing (about 10 ms); the bits above the count flag an
		// underrun.
		audio_message reply;
		status = receive(reply, 2000000);
		if (status != B_OK) {
			printf("no completion from the firmware: %s\n", strerror(status));
			break;
		}
		if (reply.type == AUDIO_COMPLETE && reply.complete.count >= 0) {
			if ((reply.complete.count & ~0x3fffffff) != 0)
				underruns++;
			completed += reply.complete.count & 0x3fffffff;
		}

		bigtime_t now = system_time();
		if (now - lastReport >= 1000000) {
			printf("%.1f s: %" B_PRIu64 " frames played, %.0f per second\n",
				(now - start) / 1e6, completed / 4,
				completed / 4 / ((now - start) / 1e6));
			lastReport = now;
		}
	}

	bigtime_t elapsed = system_time() - start;
	printf("%" B_PRIu64 " frames in %.2f s: %.0f frames per second, %"
		B_PRIu32 " underruns\n", completed / 4, elapsed / 1e6,
		completed / 4 / (elapsed / 1e6), underruns);

	message = {};
	message.type = AUDIO_STOP;
	command(message, false);
	message = {};
	message.type = AUDIO_CLOSE;
	status = command(message, true);
	printf("close: %s\n", strerror(status));

	close(sDevice);
	return 0;
}
