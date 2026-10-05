/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_tone <seconds> [frequency]
// rpi4_tone controls [<parameter id> <value>]
// Plays a sine tone through the Media Kit (BSoundPlayer, system mixer, the
// default output) and names the output node. "controls" lists the output
// node's controls (the sound driver's mixer: volume, output) and sets one:
// a selector takes the number of the choice, a slider dB.


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>
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


static int
controls(int argc, char** argv)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	status_t status = roster->GetAudioOutput(&node);
	if (status != B_OK) {
		printf("no audio output: %s\n", strerror(status));
		return 1;
	}

	BParameterWeb* web = NULL;
	status = roster->GetParameterWebFor(node, &web);
	if (status != B_OK || web == NULL) {
		printf("no controls: %s\n", strerror(status));
		return 1;
	}

	int32 wanted = argc > 3 ? atoi(argv[2]) : -1;
	for (int32 i = 0; i < web->CountParameters(); i++) {
		BParameter* parameter = web->ParameterAt(i);
		bigtime_t when;
		if (parameter->Type() == BParameter::B_DISCRETE_PARAMETER) {
			BDiscreteParameter* discrete = (BDiscreteParameter*)parameter;
			if (parameter->ID() == wanted) {
				int32 value = discrete->ItemValueAt(atoi(argv[3]));
				status = parameter->SetValue(&value, sizeof(value), -1);
				printf("set %" B_PRId32 ": %s\n", wanted, strerror(status));
				snooze(200000);
			}
			int32 value = 0;
			size_t size = sizeof(value);
			parameter->GetValue(&value, &size, &when);
			printf("%" B_PRId32 " \"%s\" (%s): %" B_PRId32 " of", parameter->ID(),
				parameter->Name(), parameter->Kind(), value);
			for (int32 item = 0; item < discrete->CountItems(); item++) {
				printf(" %" B_PRId32 "=\"%s\"", discrete->ItemValueAt(item),
					discrete->ItemNameAt(item));
			}
			printf("\n");
		} else if (parameter->Type() == BParameter::B_CONTINUOUS_PARAMETER) {
			BContinuousParameter* continuous = (BContinuousParameter*)parameter;
			float values[8];
			size_t size = sizeof(values);
			if (parameter->ID() == wanted) {
				parameter->GetValue(values, &size, &when);
				for (size_t channel = 0; channel < size / sizeof(float);
						channel++) {
					values[channel] = atof(argv[3]);
				}
				status = parameter->SetValue(values, size, -1);
				printf("set %" B_PRId32 ": %s\n", wanted, strerror(status));
				snooze(200000);
				size = sizeof(values);
			}
			parameter->GetValue(values, &size, &when);
			printf("%" B_PRId32 " \"%s\" (%s): %g (%g to %g %s, %d channels)\n",
				parameter->ID(), parameter->Name(), parameter->Kind(),
				values[0], continuous->MinValue(), continuous->MaxValue(),
				continuous->Unit(), (int)(size / sizeof(float)));
		}
	}

	roster->ReleaseNode(node);
	return 0;
}


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.airOS-rpi4-tone");

	if (argc > 1 && strcmp(argv[1], "controls") == 0)
		return controls(argc, argv);

	double seconds = argc > 1 ? atof(argv[1]) : 3;
	if (argc > 2)
		sFrequency = atof(argv[2]);

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
