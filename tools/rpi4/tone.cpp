/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_tone <seconds> [frequency]
// Plays a sine tone through the Media Kit (BSoundPlayer, system mixer, the
// default output) and names the output node.


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <MediaRoster.h>
#include <SoundPlayer.h>


static double sPhase;
static double sFrequency = 440;
static int64 sFrames;


static void
fill(void* cookie, void* buffer, size_t size, const media_raw_audio_format& format)
{
	if (format.format != media_raw_audio_format::B_AUDIO_FLOAT) {
		memset(buffer, 0, size);
		return;
	}

	float* samples = (float*)buffer;
	size_t frames = size / (sizeof(float) * format.channel_count);
	for (size_t i = 0; i < frames; i++) {
		float value = sin(sPhase) * 0.3f;
		sPhase += 2 * M_PI * sFrequency / format.frame_rate;
		for (uint32 channel = 0; channel < format.channel_count; channel++)
			*samples++ = value;
	}
	sFrames += frames;
}


int
main(int argc, char** argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 3;
	if (argc > 2)
		sFrequency = atof(argv[2]);

	BApplication app("application/x-vnd.airOS-rpi4-tone");

	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	status_t status = roster->GetAudioOutput(&node);
	if (status == B_OK) {
		live_node_info info;
		if (roster->GetLiveNodeInfo(node, &info) == B_OK)
			printf("audio output: %s\n", info.name);
		roster->ReleaseNode(node);
	} else
		printf("no audio output: %s\n", strerror(status));

	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.channel_count = 2;
	format.frame_rate = 48000;
	BSoundPlayer player(&format, "tone", fill);
	status = player.InitCheck();
	if (status != B_OK) {
		printf("BSoundPlayer: %s\n", strerror(status));
		return 1;
	}
	player.SetVolume(1.0f);
	player.Start();
	player.SetHasData(true);

	bigtime_t start = system_time();
	snooze((bigtime_t)(seconds * 1e6));
	double elapsed = (system_time() - start) / 1e6;
	player.Stop();

	printf("%lld frames asked for in %.2f s: %.0f per second\n",
		(long long)sFrames, elapsed, sFrames / elapsed);
	return 0;
}
