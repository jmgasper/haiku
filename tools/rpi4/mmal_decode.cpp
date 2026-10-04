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

	// RPI4_MMAL_DECODEONLY: ask the decoder for no pictures at all
	uint32 flags = getenv("RPI4_MMAL_DECODEONLY") != NULL
		? MMAL_BUFFER_FLAG_DECODEONLY : 0;

	// RPI4_MMAL_FRAMED: the stream goes in a NAL unit at a time, each slice
	// as a whole picture (right for streams of one slice a picture), the way
	// a player's demuxer hands them over
	bool framed = getenv("RPI4_MMAL_FRAMED") != NULL;

	MmalDecoder decoder;
	BString error;
	status_t status = decoder.Open(atoi(argv[1]), atoi(argv[2]), framed,
		&error);
	if (status != B_OK) {
		fprintf(stderr, "%s\n", error.String());
		return 1;
	}

	uint32 chunkSize = decoder.InputSize();
	uint8* chunk = (uint8*)malloc(chunkSize);

	// the whole stream, for the framed mode
	uint8* stream = NULL;
	size_t streamSize = 0;
	size_t position = 0;
	int64 units = 0;
	int32 repeat = getenv("RPI4_MMAL_REPEAT") != NULL
		? atoi(getenv("RPI4_MMAL_REPEAT")) : 1;
	bigtime_t endWait = 0;
	if (framed) {
		fseek(input, 0, SEEK_END);
		streamSize = ftell(input);
		fseek(input, 0, SEEK_SET);
		stream = (uint8*)malloc(streamSize + 4);
		if (stream == NULL || fread(stream, 1, streamSize, input)
				!= streamSize) {
			fprintf(stderr, "cannot read the stream\n");
			return 1;
		}
	}
	bool endOfFile = false;
	uint32 frames = 0;
	bigtime_t start = system_time();
	bigtime_t lastProgress = start;
	bigtime_t firstFrame = 0;

	while (true) {
		while (framed && !endOfFile && decoder.CanSend()) {
			if (position >= streamSize && repeat > 1) {
				// RPI4_MMAL_REPEAT: the stream again, that many times
				repeat--;
				position = 0;
			}
			if (position >= streamSize) {
				// The end of the stream overtakes pictures the decoder
				// has not got to yet: let it finish first.
				if (endWait == 0)
					endWait = system_time();
				if (system_time() - endWait < 1000000)
					break;
				status = decoder.SendEndOfStream();
				endOfFile = true;
				break;
			}

			// an access unit: up to the NAL unit that starts the next
			// picture (a parameter set or the like after a slice, or a
			// slice that begins with the first macroblock)
			size_t end = position;
			bool haveSlice = false;
			while (end < streamSize) {
				size_t unit = end;
				while (unit < streamSize && stream[unit] == 0)
					unit++;
				if (unit + 2 >= streamSize) {
					end = streamSize;
					break;
				}
				uint8 type = stream[unit + 1] & 0x1f;
				bool slice = type == 1 || type == 5;
				bool first = slice && (stream[unit + 2] & 0x80) != 0;
				if (haveSlice && (!slice || first))
					break;
				haveSlice |= slice;

				// the next start code
				end = unit + 1;
				while (end + 3 <= streamSize && !(stream[end] == 0
						&& stream[end + 1] == 0 && (stream[end + 2] == 1
							|| (stream[end + 2] == 0 && end + 3 < streamSize
								&& stream[end + 3] == 1)))) {
					end++;
				}
				if (end + 3 > streamSize)
					end = streamSize;
			}

			size_t sent = position;
			status = B_OK;
			while (sent < end && status == B_OK) {
				size_t bytes = min_c(end - sent, (size_t)chunkSize);
				uint32 pieceFlags = flags;
				if (sent == position)
					pieceFlags |= MMAL_BUFFER_FLAG_FRAME_START;
				if (sent + bytes == end)
					pieceFlags |= MMAL_BUFFER_FLAG_FRAME_END;
				while (!decoder.CanSend())
					decoder.Wait(5000);
				// a time for each picture, as a demuxer has one
				status = decoder.Send(stream + sent, bytes,
					haveSlice ? units * 33333 : MMAL_TIME_UNKNOWN,
					pieceFlags);
				sent += bytes;
			}
			if (haveSlice)
				units++;
			// RPI4_MMAL_PACE: so many ms between pictures going in
			if (getenv("RPI4_MMAL_PACE") != NULL)
				snooze(atoi(getenv("RPI4_MMAL_PACE")) * 1000LL);
			position = end;
			lastProgress = system_time();
		}

		while (!framed && !endOfFile && decoder.CanSend()) {
			size_t bytes = fread(chunk, 1, chunkSize, input);
			if (bytes > 0)
				status = decoder.Send(chunk, bytes, MMAL_TIME_UNKNOWN, flags);
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
