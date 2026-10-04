/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_mmal_decode <width> <height> <file.h264> [output | -]
// Decodes an H.264 Annex B byte stream with the firmware's decoder and
// writes the pictures (the visible part, planes as the firmware gives them:
// I420 or NV12) to the output file or, with "-", to the standard output, to
// compare with another decoder's:
//   rpi4_mmal_decode 1920 1080 film.h264 - | md5sum
//   ffmpeg -i film.h264 -pix_fmt yuv420p -f rawvideo - | md5sum


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include "MmalDecoder.h"


int
main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <width> <height> <file.h264> [output]\n",
			argv[0]);
		return 1;
	}

	FILE* input = fopen(argv[3], "rb");
	if (input == NULL) {
		perror(argv[3]);
		return 1;
	}
	FILE* output = NULL;
	if (argc > 4)
		output = strcmp(argv[4], "-") == 0 ? stdout : fopen(argv[4], "wb");
	// what is said goes where the pictures do not
	FILE* report = output == stdout ? stderr : stdout;

	// RPI4_MMAL_DELAY: a pause after every picture, in ms (a slow player)
	bigtime_t delay = getenv("RPI4_MMAL_DELAY") != NULL
		? atoi(getenv("RPI4_MMAL_DELAY")) * 1000LL : 0;

	MmalDecoder decoder;
	BString error;
	status_t status = decoder.Open(atoi(argv[1]), atoi(argv[2]), false,
		&error);
	if (status != B_OK) {
		fprintf(stderr, "%s\n", error.String());
		return 1;
	}

	uint32 chunkSize = decoder.InputSize();
	uint8* chunk = (uint8*)malloc(chunkSize);
	bool endOfFile = false;
	uint32 frames = 0;
	bigtime_t start = system_time();
	bigtime_t lastProgress = start;
	bigtime_t firstFrame = 0;

	while (true) {
		while (!endOfFile && decoder.CanSend()) {
			size_t bytes = fread(chunk, 1, chunkSize, input);
			if (bytes > 0)
				status = decoder.Send(chunk, bytes, MMAL_TIME_UNKNOWN, 0);
			else {
				status = decoder.SendEndOfStream();
				endOfFile = true;
			}
			if (status != B_OK && status != B_WOULD_BLOCK)
				break;
			lastProgress = system_time();
		}

		MmalDecoder::Frame frame;
		status = decoder.NextFrame(frame, 50000);
		if (status == B_OK) {
			if (frames == 0) {
				firstFrame = system_time();
				fprintf(report, "%" B_PRIu32 "x%" B_PRIu32 " in rows of %" B_PRIu32
					", %" B_PRIu32 " rows of luma\n", decoder.Width(),
					decoder.Height(), decoder.Stride(), decoder.SliceHeight());
				uint32 encoding = decoder.Encoding();
				fprintf(report, "encoding %.4s, %" B_PRIu32 " bytes a picture\n",
					(const char*)&encoding, frame.length);
			}
			if (output != NULL) {
				uint32 stride = decoder.Stride();
				uint32 width = decoder.Width();
				uint32 height = decoder.Height();
				const uint8* luma = frame.data;
				const uint8* chroma = luma + stride * decoder.SliceHeight();
				for (uint32 y = 0; y < height; y++)
					fwrite(luma + y * stride, 1, width, output);
				if (decoder.Encoding() == MMAL_ENCODING_NV12) {
					for (uint32 y = 0; y < height / 2; y++)
						fwrite(chroma + y * stride, 1, width, output);
				} else {
					// I420: two planes of half the size
					uint32 half = stride / 2;
					for (int plane = 0; plane < 2; plane++) {
						const uint8* data = chroma
							+ plane * half * (decoder.SliceHeight() / 2);
						for (uint32 y = 0; y < height / 2; y++)
							fwrite(data + y * half, 1, width / 2, output);
					}
				}
			}
			frames++;
			if (delay > 0)
				snooze(delay);
			decoder.ReleaseFrame(frame);
			lastProgress = system_time();
		} else if (status == B_LAST_BUFFER_ERROR) {
			break;
		} else if (status != B_TIMED_OUT) {
			fprintf(stderr, "decoding stopped: %s (%s)\n", strerror(status),
				decoder.Error());
			break;
		} else if (system_time() - lastProgress > 5000000) {
			fprintf(stderr, "nothing from the decoder for 5 s\n");
			break;
		}
	}

	bigtime_t end = system_time();
	fprintf(report, "%" B_PRIu32 " pictures in %.2f s", frames,
		(end - start) / 1e6);
	if (frames > 1) {
		fprintf(report, ": %.1f per second after the first",
			(frames - 1) / ((end - firstFrame) / 1e6));
	}
	fprintf(report, "\n");

	if (output != NULL && output != stdout)
		fclose(output);
	fflush(stdout);
	fclose(input);
	decoder.Close();
	return 0;
}
