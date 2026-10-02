/* See nvdec_convert.h. */

#include "nvdec_convert.h"

#include <stdlib.h>
#include <string.h>

#include <OS.h>

/* Y is scaled from 16..235 and the differences from 16..240, so both are
 * stretched; the three coefficients are the ones the standard names. */
typedef struct {
	int	yMul;
	int	rV, gU, gV, bU;
} Coefficients;

static const Coefficients kBT601 = { 298, 409, -100, -208, 516 };
static const Coefficients kBT709 = { 298, 459, -55, -136, 541 };

static inline uint8_t
clamp(int value)
{
	return (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
}

void
nvdecFrameToRGB32(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine, NvdecColorRange range)
{
	const Coefficients *c = (range == NVDEC_RANGE_BT709) ? &kBT709 : &kBT601;
	const int width = frame->width;
	const int left = frame->cropLeft;
	const int top = frame->cropTop;

	/* Gathering a line at a time, on the stack: this runs on several threads
	 * at once and thirty times a second, so it allocates nothing. */
	uint8_t luma[NVDEC_MAX_WIDTH + 32];
	uint8_t chroma[NVDEC_MAX_WIDTH + 32];
	if (width > NVDEC_MAX_WIDTH)
		return;
	for (int y = fromLine; y < toLine; y++) {
		/* Sixteen columns are consecutive bytes in the card's arrangement,
		 * so gather a line of luma and a line of chroma in those runs. */
		for (int x = 0; x < width; x += 16) {
			int count = (width - x) < 16 ? (width - x) : 16;
			memcpy(luma + x, frame->luma
				+ nvdecTileOffset(left + x, top + y, frame->pitch), count);
		}
		for (int x = 0; x < width; x += 16) {
			int count = (width - x) < 16 ? (width - x) : 16;
			memcpy(chroma + x, frame->chroma
				+ nvdecTileOffset(left + x, (top + y) / 2, frame->pitch), count);
		}
		uint8_t *line = out + (size_t)y * outPitch;
		for (int x = 0; x < width; x++) {
			int yy = (luma[x] - 16) * c->yMul;
			int cb = chroma[(x & ~1)] - 128;
			int cr = chroma[(x & ~1) + 1] - 128;
			int r = (yy + c->rV * cr + 128) >> 8;
			int g = (yy + c->gU * cb + c->gV * cr + 128) >> 8;
			int b = (yy + c->bU * cb + 128) >> 8;
			line[4 * x + 0] = clamp(b);
			line[4 * x + 1] = clamp(g);
			line[4 * x + 2] = clamp(r);
			line[4 * x + 3] = 255;
		}
	}
}

void
nvdecFrameToYCbCr422(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine)
{
	const int width = frame->width;
	const int left = frame->cropLeft;
	const int top = frame->cropTop;
	uint8_t luma[NVDEC_MAX_WIDTH + 32];
	uint8_t chroma[NVDEC_MAX_WIDTH + 32];
	if (width > NVDEC_MAX_WIDTH)
		return;
	for (int y = fromLine; y < toLine; y++) {
		for (int x = 0; x < width; x += 16) {
			int count = (width - x) < 16 ? (width - x) : 16;
			memcpy(luma + x, frame->luma
				+ nvdecTileOffset(left + x, top + y, frame->pitch), count);
			memcpy(chroma + x, frame->chroma
				+ nvdecTileOffset(left + x, (top + y) / 2, frame->pitch), count);
		}
		uint8_t *line = out + (size_t)y * outPitch;
		for (int x = 0; x + 1 < width; x += 2) {
			line[2 * x + 0] = chroma[x];		/* Cb */
			line[2 * x + 1] = luma[x];
			line[2 * x + 2] = chroma[x + 1];	/* Cr */
			line[2 * x + 3] = luma[x + 1];
		}
	}
}

/* ------------------------------------------------------------- in bands */

typedef struct {
	const NvdecFrame	*frame;
	uint8_t			*out;
	size_t			outPitch;
	NvdecColorRange		range;
	bool			toRGB;
	int			bands;
	int32			next;
} Work;

static int32
convertBand(void *data)
{
	Work *work = data;
	for (;;) {
		int32 band = atomic_add(&work->next, 1);
		if (band >= work->bands)
			break;
		int height = work->frame->height;
		int from = (int)((int64)height * band / work->bands) & ~1;
		int to = (int)((int64)height * (band + 1) / work->bands) & ~1;
		if (band == work->bands - 1)
			to = height;
		if (work->toRGB) {
			nvdecFrameToRGB32(work->frame, work->out, work->outPitch,
				from, to, work->range);
		} else {
			nvdecFrameToYCbCr422(work->frame, work->out, work->outPitch, from, to);
		}
	}
	return 0;
}

static void
convertThreaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	NvdecColorRange range, bool toRGB)
{
	system_info info;
	get_system_info(&info);
	int threads = getenv("NVDEC_CONVERT_SINGLE") != NULL ? 1 : (int)info.cpu_count;
	if (threads > 8)
		threads = 8;
	if (threads < 1)
		threads = 1;

	Work work = {
		.frame = frame,
		.out = out,
		.outPitch = outPitch,
		.range = range,
		.toRGB = toRGB,
		.bands = threads * 2,
		.next = 0,
	};
	thread_id helpers[8];
	int started = 0;
	for (int i = 0; i < threads - 1 && i < 8; i++) {
		helpers[started] = spawn_thread(convertBand, "nvdec convert",
			B_DISPLAY_PRIORITY, &work);
		if (helpers[started] < 0)
			break;
		resume_thread(helpers[started]);
		started++;
	}
	convertBand(&work);
	for (int i = 0; i < started; i++) {
		status_t ignored;
		wait_for_thread(helpers[i], &ignored);
	}
}

void
nvdecFrameToRGB32Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	NvdecColorRange range)
{
	convertThreaded(frame, out, outPitch, range, true);
}

void
nvdecFrameToYCbCr422Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch)
{
	convertThreaded(frame, out, outPitch, NVDEC_RANGE_BT601, false);
}
