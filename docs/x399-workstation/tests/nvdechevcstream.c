/* Decode a whole H.265 stream on the card and write what comes out.
 *
 * Reads an Annex B file, hands the decoder one access unit at a time, and
 * writes the pictures in the order they should be shown: eight-bit pictures
 * as yuv420p and ten-bit ones as yuv420p10le, which is what ffmpeg writes with
 * those -pix_fmt values, so the two can be compared byte for byte.
 *
 * Usage: nvdechevcstream <stream.h265> [out.yuv|-] [frame limit]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include "nvdec_convert.h"
#include "nvdec_hevc.h"

/* NVDEC_BENCH_CONVERT=1: time turning each picture into what a player is
 * given (P010 for ten bits, YCbCr422 for eight) instead of writing it. */
static bigtime_t sConvertTime;
static int sConverted;

static void
benchConvert(const NvdecFrame *frame)
{
	static uint8_t *buffer;
	size_t rowBytes = (size_t)frame->width * 2;
	if (buffer == NULL)
		buffer = malloc(rowBytes * frame->height * 2);
	bigtime_t at = system_time();
	if (frame->bitDepth > 8)
		nvdecFrameToP010Threaded(frame, buffer, rowBytes);
	else
		nvdecFrameToYCbCr422Threaded(frame, buffer, rowBytes);
	sConvertTime += system_time() - at;
	sConverted++;
}

static void
writeFrame(FILE *out, const NvdecFrame *frame)
{
	if (getenv("NVDEC_BENCH_CONVERT") != NULL)
		benchConvert(frame);
	if (out == NULL)
		return;
	int width = frame->width, height = frame->height;
	int bytes = frame->bitDepth > 8 ? 2 : 1;
	size_t lineBytes = (size_t)width * bytes;
	uint8_t *line = malloc(lineBytes);
	uint8_t *cb = malloc((size_t)(width / 2) * (height / 2) * bytes);
	uint8_t *cr = malloc((size_t)(width / 2) * (height / 2) * bytes);

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < (int)lineBytes; x++) {
			line[x] = frame->luma[nvdecTileOffset(frame->cropLeft * bytes + x,
				frame->cropTop + y, frame->pitch)];
		}
		if (bytes == 2) {
			uint16_t *samples = (uint16_t *)line;
			for (int x = 0; x < width; x++)
				samples[x] >>= 6;
		}
		fwrite(line, 1, lineBytes, out);
	}
	for (int y = 0; y < height / 2; y++) {
		for (int x = 0; x < (int)lineBytes; x++) {
			line[x] = frame->chroma[nvdecTileOffset(frame->cropLeft * bytes + x,
				frame->cropTop / 2 + y, frame->pitch)];
		}
		for (int x = 0; x < width / 2; x++) {
			size_t at = ((size_t)y * (width / 2) + x) * bytes;
			if (bytes == 2) {
				uint16_t *samples = (uint16_t *)line;
				((uint16_t *)cb)[at / 2] = samples[2 * x] >> 6;
				((uint16_t *)cr)[at / 2] = samples[2 * x + 1] >> 6;
			} else {
				cb[at] = line[2 * x];
				cr[at] = line[2 * x + 1];
			}
		}
	}
	fwrite(cb, 1, (size_t)(width / 2) * (height / 2) * bytes, out);
	fwrite(cr, 1, (size_t)(width / 2) * (height / 2) * bytes, out);
	free(line);
	free(cb);
	free(cr);
}

typedef struct {
	size_t	offset;
	size_t	length;
	int	type;
	bool	firstSlice;
} Nal;

static size_t
findNals(const uint8_t *data, size_t size, Nal *nals, size_t maxNals)
{
	size_t count = 0;
	size_t i = 0;
	while (i + 3 <= size && count < maxNals) {
		if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
			i++;
			continue;
		}
		size_t payload = i + 3;
		size_t j = payload;
		while (j + 3 <= size && !(data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1))
			j++;
		size_t end = (j + 3 <= size) ? j : size;
		while (end > payload && data[end - 1] == 0)
			end--;
		if (end >= payload + 3) {
			nals[count].offset = i;
			nals[count].length = end - i;
			nals[count].type = (data[payload] >> 1) & 0x3f;
			nals[count].firstSlice = nals[count].type < 32
				&& (data[payload + 2] & 0x80) != 0;
			count++;
		}
		i = (j + 3 <= size) ? j : size;
	}
	return count;
}

/* 7.4.2.4.4: a new access unit begins at the first of these that follows a
 * picture - parameter sets, a delimiter, a prefix SEI - or at the first slice
 * of the next picture. */
static bool
beginsAccessUnit(const Nal *nal)
{
	if (nal->type < 32)
		return nal->firstSlice;
	return (nal->type >= 32 && nal->type <= 35) || nal->type == 39
		|| (nal->type >= 41 && nal->type <= 44);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: %s <stream.h265> [out.yuv|-] [frame limit]\n", argv[0]);
		return 2;
	}
	int limit = (argc > 3) ? atoi(argv[3]) : 0;

	FILE *file = fopen(argv[1], "rb");
	if (file == NULL) { perror(argv[1]); return 1; }
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	uint8_t *data = malloc(size);
	if (data == NULL || fread(data, 1, size, file) != (size_t)size) {
		printf("cannot read %s\n", argv[1]);
		return 1;
	}
	fclose(file);

	FILE *out = NULL;
	if (argc > 2 && strcmp(argv[2], "-") != 0) {
		out = fopen(argv[2], "wb");
		if (out == NULL) { perror(argv[2]); return 1; }
	}

	size_t maxNals = size / 4 + 16;
	Nal *nals = malloc(maxNals * sizeof(Nal));
	size_t nalCount = findNals(data, size, nals, maxNals);

	char reason[256] = "";
	NvdecEngine *engine = nvdecOpen(reason, sizeof(reason));
	if (engine == NULL) { printf("opening the decoder: %s\n", reason); return 1; }
	NvdecHevc *decoder = nvdecHevcCreate(engine, reason, sizeof(reason));
	if (decoder == NULL) { printf("creating the decoder failed\n"); return 1; }

	int decoded = 0, written = 0, failures = 0;
	bigtime_t began = system_time();
	bigtime_t decodeTime = 0;
	size_t auStart = 0;
	bool sawPicture = false;
	NvdecFrame frame;

	for (size_t i = 0; i <= nalCount; i++) {
		bool last = i == nalCount;
		if ((last || beginsAccessUnit(&nals[i])) && sawPicture) {
			size_t auEnd = last ? (size_t)size : nals[i].offset;
			bigtime_t at = system_time();
			if (!nvdecHevcDecode(decoder, data + auStart, auEnd - auStart,
					decoded)) {
				printf("picture %d: %s\n", decoded, nvdecHevcLastError(decoder));
				failures++;
			}
			decodeTime += system_time() - at;
			decoded++;
			while (nvdecHevcNextFrame(decoder, &frame)) {
				writeFrame(out, &frame);
				nvdecHevcReleaseFrame(decoder, &frame);
				written++;
			}
			auStart = auEnd;
			sawPicture = false;
			if (limit > 0 && decoded >= limit)
				break;
		}
		if (last)
			break;
		if (nals[i].type < 32)
			sawPicture = true;
	}
	nvdecHevcDrainAll(decoder);
	while (nvdecHevcNextFrame(decoder, &frame)) {
		writeFrame(out, &frame);
		nvdecHevcReleaseFrame(decoder, &frame);
		written++;
	}
	bigtime_t elapsed = system_time() - began;
	int width = 0, height = 0, depth = 0;
	nvdecHevcHasFormat(decoder, &width, &height, &depth);
	printf("%d pictures decoded, %d written, %d failed, %dx%d %d-bit; "
		"%.2f ms a picture in the engine, %.1f s in all\n", decoded, written,
		failures, width, height, depth,
		decoded > 0 ? decodeTime / 1000.0 / decoded : 0.0, elapsed / 1e6);
	if (sConverted > 0) {
		printf("conversion: %.2f ms a picture over %d\n",
			sConvertTime / 1000.0 / sConverted, sConverted);
	}
	if (out != NULL)
		fclose(out);
	nvdecHevcDestroy(decoder);
	nvdecClose(engine);
	return failures > 0 ? 1 : 0;
}
