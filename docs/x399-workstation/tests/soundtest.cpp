/* Check that sound really comes out of the machine.
 *
 * Plays a tone through the media kit and counts the frames the driver asks
 * for. Nobody here can listen to the speakers, so the proof is arithmetic: if
 * the hardware is clocking the stream, the number of frames it takes in a
 * second matches the sample rate. A driver that never starts asks for nothing;
 * one that free-runs on a software timer drifts.
 *
 * usage: soundtest [seconds] [hz]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <MediaDefs.h>
#include <SoundPlayer.h>
#include <OS.h>


struct Tone {
	double phase;
	double step;
	int64 frames;
	bigtime_t firstBuffer;
	bigtime_t lastBuffer;
	int32 buffers;
};


static void
FillBuffer(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format)
{
	Tone *tone = (Tone *)cookie;
	const size_t frames = size / (sizeof(float) * format.channel_count);
	float *out = (float *)buffer;

	for (size_t i = 0; i < frames; i++) {
		float v = 0.2f * sinf(tone->phase);
		tone->phase += tone->step;
		if (tone->phase > 2 * M_PI)
			tone->phase -= 2 * M_PI;
		for (uint32 c = 0; c < format.channel_count; c++)
			*out++ = v;
	}

	bigtime_t now = system_time();
	if (tone->buffers == 0)
		tone->firstBuffer = now;
	tone->lastBuffer = now;
	tone->buffers++;
	tone->frames += frames;
}


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	double seconds = argc > 1 ? atof(argv[1]) : 3.0;
	double hz = argc > 2 ? atof(argv[2]) : 440.0;

	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.channel_count = 2;
	format.frame_rate = 48000;
	format.byte_order = B_MEDIA_HOST_ENDIAN;

	Tone tone = {};
	tone.step = 2 * M_PI * hz / format.frame_rate;

	BSoundPlayer player(&format, "soundtest", FillBuffer, NULL, &tone);
	if (player.InitCheck() != B_OK) {
		fprintf(stderr, "[!] the media kit would not give us a player: %s\n",
			strerror(player.InitCheck()));
		return 1;
	}

	const media_raw_audio_format &got = player.Format();
	printf("playing %.0f Hz: %" B_PRIu32 " channels at %.0f frames a second,"
		" %" B_PRIu32 " byte buffers\n",
		hz, got.channel_count, got.frame_rate, got.buffer_size);

	player.Start();
	player.SetHasData(true);
	snooze((bigtime_t)(seconds * 1000000));
	player.Stop();

	if (tone.buffers < 2) {
		printf("the driver asked for %" B_PRId32 " buffers: nothing is playing\n",
			tone.buffers);
		return 1;
	}

	double elapsed = (tone.lastBuffer - tone.firstBuffer) / 1000000.0;
	double rate = elapsed > 0 ? tone.frames / elapsed : 0;
	printf("%" B_PRId64 " frames in %" B_PRId32 " buffers over %.2f s:"
		" %.0f frames a second\n", tone.frames, tone.buffers, elapsed, rate);
	printf("that is %.2f%% off the %.0f the stream asked for\n",
		100.0 * (rate - got.frame_rate) / got.frame_rate, got.frame_rate);
	return 0;
}
