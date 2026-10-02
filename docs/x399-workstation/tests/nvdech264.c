/* Decode one H.264 picture on the card's video engine and write it out.
 *
 * The engine parses slice headers and every entropy-coded bit itself; what it
 * cannot do is know what the sequence and picture parameter sets said, so that
 * much is parsed here and handed over as a picture setup structure. This
 * decodes the first picture of an Annex B stream, which must be an IDR, and
 * writes the result as NV12.
 *
 * Usage: nvdech264 <stream.h264> <out.nv12>
 */
#include "nvdecrm.h"
#include "video/nvdec_drv.h"

#define MAX_SLICES 1024

/* ------------------------------------------------------------------ bits */

typedef struct {
	const uint8_t	*data;
	size_t		size;
	size_t		bytePos;
	int		bitPos;
} BitReader;

static void brInit(BitReader *br, const uint8_t *data, size_t size)
{
	br->data = data; br->size = size; br->bytePos = 0; br->bitPos = 0;
}

static uint32_t brBit(BitReader *br)
{
	if (br->bytePos >= br->size) return 0;
	uint32_t v = (br->data[br->bytePos] >> (7 - br->bitPos)) & 1;
	if (++br->bitPos == 8) { br->bitPos = 0; br->bytePos++; }
	return v;
}

static uint32_t brBits(BitReader *br, int n)
{
	uint32_t v = 0;
	while (n-- > 0) v = (v << 1) | brBit(br);
	return v;
}

static uint32_t brUE(BitReader *br)
{
	int zeros = 0;
	while (zeros < 32 && brBit(br) == 0 && br->bytePos < br->size) zeros++;
	if (zeros == 0) return 0;
	return (1u << zeros) - 1 + brBits(br, zeros);
}

static int32_t brSE(BitReader *br)
{
	uint32_t k = brUE(br);
	return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

static bool brMoreData(BitReader *br)
{
	/* More RBSP data unless what is left is the stop bit and padding. */
	if (br->bytePos >= br->size) return false;
	size_t lastByte = br->size;
	while (lastByte > 0 && br->data[lastByte - 1] == 0) lastByte--;
	if (lastByte == 0) return false;
	int lastBit = 0;
	for (int i = 0; i < 8; i++)
		if ((br->data[lastByte - 1] >> i) & 1) { lastBit = 7 - i; break; }
	size_t pos = br->bytePos * 8 + br->bitPos;
	size_t end = (lastByte - 1) * 8 + lastBit;
	return pos < end;
}

/* --------------------------------------------------------------- headers */

typedef struct {
	bool		valid;
	int		profileIdc, levelIdc;
	int		chromaFormatIdc;
	int		bitDepthLuma, bitDepthChroma;
	int		qpprimeYZeroTransformBypass;
	int		log2MaxFrameNumMinus4;
	int		picOrderCntType;
	int		log2MaxPocLsbMinus4;
	int		deltaPicOrderAlwaysZero;
	int		offsetForNonRefPic, offsetForTopToBottomField;
	int		numRefFramesInPocCycle;
	int		offsetForRefFrame[256];
	int		maxNumRefFrames;
	int		picWidthInMbs, picHeightInMapUnits;
	int		frameMbsOnly, mbAdaptiveFrameField;
	int		direct8x8Inference;
	int		cropLeft, cropRight, cropTop, cropBottom;
	uint8_t		scaling4x4[6][16];
	uint8_t		scaling8x8[2][64];
	bool		scalingPresent;
} Sps;

typedef struct {
	bool		valid;
	int		spsId;
	int		entropyCodingMode;
	int		picOrderPresent;
	int		numRefIdxL0Minus1, numRefIdxL1Minus1;
	int		weightedPred, weightedBipredIdc;
	int		picInitQpMinus26;
	int		chromaQpIndexOffset, secondChromaQpIndexOffset;
	int		deblockingFilterControlPresent;
	int		constrainedIntraPred;
	int		redundantPicCntPresent;
	int		transform8x8Mode;
	uint8_t		scaling4x4[6][16];
	uint8_t		scaling8x8[2][64];
	bool		scalingPresent;
} Pps;

typedef struct {
	int		firstMbInSlice;
	int		sliceType;
	int		ppsId;
	int		frameNum;
	int		fieldPic, bottomField;
	int		idrPicId;
	int		pocLsb;
	int		deltaPocBottom;
	int		deltaPoc[2];
	int		numRefIdxL0Minus1, numRefIdxL1Minus1;
	int		nalRefIdc, nalType;
	int		longTermReference;
} SliceHeader;

/* Table 8-13/8-14: zig-zag order, used to put scaling lists back in raster. */
static const uint8_t kZigzag4x4[16] = {
	0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};
static const uint8_t kZigzag8x8[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};
static const uint8_t kDefault4x4Intra[16] = {
	 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42
};
static const uint8_t kDefault4x4Inter[16] = {
	10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34
};
static const uint8_t kDefault8x8Intra[64] = {
	 6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
	23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
	27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42
};
static const uint8_t kDefault8x8Inter[64] = {
	 9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
	21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35
};

/* 7.3.2.1.1.1, written straight into raster order. */
static void
parseScalingList(BitReader *br, uint8_t *list, int size, const uint8_t *zigzag,
	const uint8_t *fallback, const uint8_t *deflt)
{
	int lastScale = 8, nextScale = 8;
	bool useDefault = false;
	for (int j = 0; j < size; j++) {
		if (nextScale != 0) {
			int delta = brSE(br);
			nextScale = (lastScale + delta + 256) % 256;
			useDefault = (j == 0 && nextScale == 0);
		}
		int value = (nextScale == 0) ? lastScale : nextScale;
		list[zigzag[j]] = (uint8_t)value;
		lastScale = value;
	}
	if (useDefault)
		for (int j = 0; j < size; j++) list[zigzag[j]] = deflt[j];
	(void)fallback;
}

static void
parseScalingMatrices(BitReader *br, int count, int chromaFormatIdc,
	uint8_t scaling4x4[6][16], uint8_t scaling8x8[2][64])
{
	for (int i = 0; i < count; i++) {
		if (brBit(br) == 0)
			continue;
		if (i < 6) {
			parseScalingList(br, scaling4x4[i], 16, kZigzag4x4, NULL,
				(i < 3) ? kDefault4x4Intra : kDefault4x4Inter);
		} else {
			int k = i - 6;
			if (chromaFormatIdc != 3 && k > 1)
				break;
			parseScalingList(br, scaling8x8[k & 1], 64, kZigzag8x8, NULL,
				((k & 1) == 0) ? kDefault8x8Intra : kDefault8x8Inter);
		}
	}
}

static void setFlat(uint8_t s4[6][16], uint8_t s8[2][64])
{
	memset(s4, 16, 6 * 16);
	memset(s8, 16, 2 * 64);
}

static bool
parseSps(const uint8_t *rbsp, size_t size, Sps *sps)
{
	BitReader br;
	brInit(&br, rbsp, size);
	memset(sps, 0, sizeof(*sps));
	setFlat(sps->scaling4x4, sps->scaling8x8);

	sps->profileIdc = brBits(&br, 8);
	brBits(&br, 8);			/* constraint flags and reserved */
	sps->levelIdc = brBits(&br, 8);
	brUE(&br);			/* seq_parameter_set_id */
	sps->chromaFormatIdc = 1;
	sps->bitDepthLuma = sps->bitDepthChroma = 8;
	switch (sps->profileIdc) {
	case 100: case 110: case 122: case 244: case 44:
	case 83: case 86: case 118: case 128: case 138: case 139:
	case 134: case 135:
		sps->chromaFormatIdc = brUE(&br);
		if (sps->chromaFormatIdc == 3) brBit(&br);
		sps->bitDepthLuma = 8 + brUE(&br);
		sps->bitDepthChroma = 8 + brUE(&br);
		sps->qpprimeYZeroTransformBypass = brBit(&br);
		if (brBit(&br)) {
			sps->scalingPresent = true;
			parseScalingMatrices(&br, (sps->chromaFormatIdc != 3) ? 8 : 12,
				sps->chromaFormatIdc, sps->scaling4x4, sps->scaling8x8);
		}
		break;
	}
	sps->log2MaxFrameNumMinus4 = brUE(&br);
	sps->picOrderCntType = brUE(&br);
	if (sps->picOrderCntType == 0) {
		sps->log2MaxPocLsbMinus4 = brUE(&br);
	} else if (sps->picOrderCntType == 1) {
		sps->deltaPicOrderAlwaysZero = brBit(&br);
		sps->offsetForNonRefPic = brSE(&br);
		sps->offsetForTopToBottomField = brSE(&br);
		sps->numRefFramesInPocCycle = brUE(&br);
		for (int i = 0; i < sps->numRefFramesInPocCycle && i < 256; i++)
			sps->offsetForRefFrame[i] = brSE(&br);
	}
	sps->maxNumRefFrames = brUE(&br);
	brBit(&br);			/* gaps_in_frame_num_value_allowed_flag */
	sps->picWidthInMbs = brUE(&br) + 1;
	sps->picHeightInMapUnits = brUE(&br) + 1;
	sps->frameMbsOnly = brBit(&br);
	if (!sps->frameMbsOnly) sps->mbAdaptiveFrameField = brBit(&br);
	sps->direct8x8Inference = brBit(&br);
	if (brBit(&br)) {
		sps->cropLeft = brUE(&br);
		sps->cropRight = brUE(&br);
		sps->cropTop = brUE(&br);
		sps->cropBottom = brUE(&br);
	}
	sps->valid = true;
	return true;
}

static bool
parsePps(const uint8_t *rbsp, size_t size, const Sps *sps, Pps *pps)
{
	BitReader br;
	brInit(&br, rbsp, size);
	memset(pps, 0, sizeof(*pps));
	setFlat(pps->scaling4x4, pps->scaling8x8);

	brUE(&br);			/* pic_parameter_set_id */
	pps->spsId = brUE(&br);
	pps->entropyCodingMode = brBit(&br);
	pps->picOrderPresent = brBit(&br);
	int numSliceGroupsMinus1 = brUE(&br);
	if (numSliceGroupsMinus1 > 0) {
		printf("slice groups are not supported\n");
		return false;
	}
	pps->numRefIdxL0Minus1 = brUE(&br);
	pps->numRefIdxL1Minus1 = brUE(&br);
	pps->weightedPred = brBit(&br);
	pps->weightedBipredIdc = brBits(&br, 2);
	pps->picInitQpMinus26 = brSE(&br);
	brSE(&br);			/* pic_init_qs_minus26 */
	pps->chromaQpIndexOffset = brSE(&br);
	pps->deblockingFilterControlPresent = brBit(&br);
	pps->constrainedIntraPred = brBit(&br);
	pps->redundantPicCntPresent = brBit(&br);
	pps->secondChromaQpIndexOffset = pps->chromaQpIndexOffset;
	if (brMoreData(&br)) {
		pps->transform8x8Mode = brBit(&br);
		if (brBit(&br)) {
			pps->scalingPresent = true;
			/* The picture's lists start from the sequence's. */
			memcpy(pps->scaling4x4, sps->scaling4x4, sizeof(pps->scaling4x4));
			memcpy(pps->scaling8x8, sps->scaling8x8, sizeof(pps->scaling8x8));
			int count = 6 + (sps->chromaFormatIdc != 3 ? 2 : 6) * pps->transform8x8Mode;
			parseScalingMatrices(&br, count, sps->chromaFormatIdc,
				pps->scaling4x4, pps->scaling8x8);
		}
		pps->secondChromaQpIndexOffset = brSE(&br);
	}
	pps->valid = true;
	return true;
}

static bool
parseSliceHeader(const uint8_t *rbsp, size_t size, const Sps *sps, const Pps *pps,
	int nalType, int nalRefIdc, SliceHeader *sh)
{
	BitReader br;
	brInit(&br, rbsp, size);
	memset(sh, 0, sizeof(*sh));
	sh->nalType = nalType;
	sh->nalRefIdc = nalRefIdc;

	sh->firstMbInSlice = brUE(&br);
	sh->sliceType = brUE(&br);
	if (sh->sliceType >= 5) sh->sliceType -= 5;
	sh->ppsId = brUE(&br);
	sh->frameNum = brBits(&br, sps->log2MaxFrameNumMinus4 + 4);
	if (!sps->frameMbsOnly) {
		sh->fieldPic = brBit(&br);
		if (sh->fieldPic) sh->bottomField = brBit(&br);
	}
	if (nalType == 5) sh->idrPicId = brUE(&br);
	if (sps->picOrderCntType == 0) {
		sh->pocLsb = brBits(&br, sps->log2MaxPocLsbMinus4 + 4);
		if (pps->picOrderPresent && !sh->fieldPic) sh->deltaPocBottom = brSE(&br);
	} else if (sps->picOrderCntType == 1 && !sps->deltaPicOrderAlwaysZero) {
		sh->deltaPoc[0] = brSE(&br);
		if (pps->picOrderPresent && !sh->fieldPic) sh->deltaPoc[1] = brSE(&br);
	}
	if (pps->redundantPicCntPresent) brUE(&br);

	sh->numRefIdxL0Minus1 = pps->numRefIdxL0Minus1;
	sh->numRefIdxL1Minus1 = pps->numRefIdxL1Minus1;
	if (sh->sliceType == 1) brBit(&br);	/* direct_spatial_mv_pred_flag */
	if (sh->sliceType == 0 || sh->sliceType == 1 || sh->sliceType == 3) {
		if (brBit(&br)) {
			sh->numRefIdxL0Minus1 = brUE(&br);
			if (sh->sliceType == 1) sh->numRefIdxL1Minus1 = brUE(&br);
		}
	}
	/* Everything past here the engine parses for itself; the one thing still
	 * wanted from it is how an IDR asks to be kept. */
	if (nalType == 5) {
		/* ref_pic_list_modification is absent for I slices of an IDR. */
		if (sh->sliceType != 2 && sh->sliceType != 4) {
			if (brBit(&br)) {
				for (;;) {
					int op = brUE(&br);
					if (op == 3) break;
					brUE(&br);
				}
			}
		}
		if (nalRefIdc != 0) {
			brBit(&br);		/* no_output_of_prior_pics_flag */
			sh->longTermReference = brBit(&br);
		}
	}
	return true;
}

/* ----------------------------------------------------------------- stream */

typedef struct {
	size_t	offset;		/* into the file, at the start code */
	size_t	length;		/* including the start code */
	int	type;
	int	refIdc;
	size_t	payloadOffset;	/* first byte after the header byte */
} Nal;

static size_t
findNals(const uint8_t *data, size_t size, Nal *nals, size_t maxNals)
{
	size_t count = 0;
	size_t i = 0;
	while (i + 3 <= size && count < maxNals) {
		if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) { i++; continue; }
		size_t start = i;
		size_t payload = i + 3;
		size_t j = payload;
		while (j + 3 <= size && !(data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1)) j++;
		size_t end = (j + 3 <= size) ? j : size;
		/* A start code may have a leading zero belonging to the next one. */
		while (end > payload && data[end - 1] == 0) end--;
		nals[count].offset = start;
		nals[count].length = end - start;
		nals[count].type = data[payload] & 0x1f;
		nals[count].refIdc = (data[payload] >> 5) & 3;
		nals[count].payloadOffset = payload + 1;
		count++;
		i = (j + 3 <= size) ? j : size;
	}
	return count;
}

/* Strip emulation prevention bytes so the header parsers see plain RBSP. */
static size_t
toRbsp(const uint8_t *in, size_t size, uint8_t *out)
{
	size_t n = 0;
	for (size_t i = 0; i < size; i++) {
		if (i + 2 < size && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 3) {
			out[n++] = 0; out[n++] = 0; i += 2;
			continue;
		}
		out[n++] = in[i];
	}
	return n;
}

/* ------------------------------------------------------------------ main */

static int envInt(const char *name, int deflt)
{
	const char *s = getenv(name);
	return s != NULL ? atoi(s) : deflt;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		printf("usage: %s <stream.h264> <out.nv12>\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *file = malloc(fileSize);
	if (fread(file, 1, fileSize, f) != (size_t)fileSize) { printf("short read\n"); return 1; }
	fclose(f);

	static Nal nals[4096];
	size_t nalCount = findNals(file, fileSize, nals, 4096);
	printf("%zu NAL units in %ld bytes\n", nalCount, fileSize);

	Sps sps = {0};
	Pps pps = {0};
	SliceHeader sh = {0};
	uint8_t *rbsp = malloc(fileSize);
	bool haveSlice = false;
	size_t sliceNal = 0;

	for (size_t i = 0; i < nalCount; i++) {
		size_t payloadLen = nals[i].offset + nals[i].length - nals[i].payloadOffset;
		size_t n = toRbsp(file + nals[i].payloadOffset, payloadLen, rbsp);
		switch (nals[i].type) {
		case 7:
			if (!parseSps(rbsp, n, &sps)) return 1;
			printf("sequence: %dx%d macroblocks, profile %d level %d, "
				"chroma %d, %s, poc type %d\n",
				sps.picWidthInMbs, sps.picHeightInMapUnits,
				sps.profileIdc, sps.levelIdc, sps.chromaFormatIdc,
				sps.frameMbsOnly ? "frames only" : "fields",
				sps.picOrderCntType);
			break;
		case 8:
			if (!sps.valid) { printf("picture parameters before sequence\n"); return 1; }
			if (!parsePps(rbsp, n, &sps, &pps)) return 1;
			printf("picture: %s, transform8x8 %d, init qp %d\n",
				pps.entropyCodingMode ? "cabac" : "cavlc",
				pps.transform8x8Mode, 26 + pps.picInitQpMinus26);
			break;
		case 1: case 5:
			if (!haveSlice) {
				if (!sps.valid || !pps.valid) { printf("slice before parameters\n"); return 1; }
				parseSliceHeader(rbsp, n, &sps, &pps, nals[i].type,
					nals[i].refIdc, &sh);
				printf("slice: type %d, frame_num %d, poc lsb %d, %s\n",
					sh.sliceType, sh.frameNum, sh.pocLsb,
					nals[i].type == 5 ? "IDR" : "non-IDR");
				haveSlice = true;
				sliceNal = i;
			}
			break;
		}
		if (haveSlice) break;
	}
	if (!haveSlice) { printf("no slice found\n"); return 1; }
	if (nals[sliceNal].type != 5) { printf("the first picture is not an IDR\n"); return 1; }

	const int mbWidth = sps.picWidthInMbs;
	const int mbHeight = sps.picHeightInMapUnits * (sps.frameMbsOnly ? 1 : 2);
	const int width = mbWidth * 16;
	const int height = mbHeight * 16;
	const int tileFormat = envInt("NVDEC_TILE_FORMAT", 0);
	const int gobHeight = envInt("NVDEC_GOB_HEIGHT", 0);
	const int pitch = envInt("NVDEC_PITCH", (width + 255) & ~255);
	/* The picture setup's pitch is in units of 256 bytes unless told otherwise. */
	const int pitchUnits = envInt("NVDEC_PITCH_UNITS", 256);
	printf("picture %dx%d, %d bytes a line, tile format %d\n", width, height, pitch, tileFormat);

	NvDecGpu gpu;
	if (nvdecOpen(&gpu) != NV_OK) return 1;
	printf("decoder channel ready\n");

	const size_t mbCount = (size_t)mbWidth * mbHeight;
	/* The decoder writes blocks of sixteen lines, so a plane is that tall
	 * whether the picture is or not. */
	const int blockRows = 16;
	const size_t lumaSize = (size_t)pitch * ((height + blockRows - 1) / blockRows * blockRows);
	const size_t chromaSize = (size_t)pitch
		* (((height + 1) / 2 + blockRows - 1) / blockRows * blockRows);

	NvBuffer picSetup, bitstream, sliceOffsets, coloc, history, mbhist, status, scratch, surface;
	NvBuffer *toAlloc[] = { &picSetup, &bitstream, &sliceOffsets, &coloc, &history, &mbhist, &status, &scratch, &surface };
	uint64_t sizes[] = {
		sizeof(nvdec_h264_pic_s) + 0x100,
		(size_t)fileSize + 0x1000,
		MAX_SLICES * 4,
		mbCount * 256 + 0x1000,
		(size_t)envInt("NVDEC_HISTORY_KB", 1024) * 1024,
		mbCount * 512 + 0x1000,
		0x1000,
		0x10000,
		lumaSize + chromaSize + 0x1000,
	};
	const bool surfaceInSysmem = envInt("NVDEC_SURFACE_SYSMEM", 0) != 0;
	bool sysmem[] = { true, true, true, false, false, false, true, false, surfaceInSysmem };
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		if (nvdecAllocBuffer(&gpu, toAlloc[i], sizes[i], sysmem[i]) != NV_OK) {
			printf("allocating buffer %zu of %llu bytes failed\n", i,
				(unsigned long long)sizes[i]);
			return 1;
		}
	}

	/* The engine wants every slice of the picture back to back, without start
	 * codes removed - it looks for them itself - and a list of where they are. */
	uint8_t *bits = bitstream.map;
	uint32_t *offsets = sliceOffsets.map;
	size_t streamLen = 0;
	uint32_t sliceCount = 0;
	const bool keepStartCodes = envInt("NVDEC_START_CODES", 1) != 0;
	/* The engine reads ahead of the bits it needs, and stops with "out of
	 * data" partway through the picture if the length it is given ends
	 * exactly at the last byte of the last slice. Sixteen bytes of slack is
	 * enough; this leaves a whole block, and the buffer is already zeroed. */
	const int streamPad = envInt("NVDEC_STREAM_PAD", 256);
	const bool padCounts = envInt("NVDEC_PAD_COUNTS", 1) != 0;
	for (size_t i = sliceNal; i < nalCount; i++) {
		if (nals[i].type != 1 && nals[i].type != 5) continue;
		size_t from = keepStartCodes ? nals[i].offset : nals[i].payloadOffset - 1;
		size_t len = nals[i].offset + nals[i].length - from;
		offsets[sliceCount++] = (uint32_t)streamLen;
		memcpy(bits + streamLen, file + from, len);
		streamLen += len;
		if (sliceCount >= MAX_SLICES) break;
	}
	size_t reportedLen = streamLen + (padCounts ? streamPad : 0);
	printf("%u slice(s), %zu bytes of bitstream, %zu reported\n",
		sliceCount, streamLen, reportedLen);

	nvdec_h264_pic_s *ps = picSetup.map;
	memset(ps, 0, sizeof(*ps));
	ps->stream_len = (unsigned)reportedLen;
	ps->slice_count = sliceCount;
	ps->mbhist_buffer_size = (unsigned)mbhist.size;
	ps->gptimer_timeout_value = 0;

	ps->log2_max_pic_order_cnt_lsb_minus4 = sps.log2MaxPocLsbMinus4;
	ps->delta_pic_order_always_zero_flag = sps.deltaPicOrderAlwaysZero;
	ps->frame_mbs_only_flag = sps.frameMbsOnly;
	ps->PicWidthInMbs = mbWidth;
	ps->FrameHeightInMbs = mbHeight;
	ps->tileFormat = tileFormat;
	ps->gob_height = gobHeight;

	ps->entropy_coding_mode_flag = pps.entropyCodingMode;
	ps->pic_order_present_flag = pps.picOrderPresent;
	ps->num_ref_idx_l0_active_minus1 = sh.numRefIdxL0Minus1;
	ps->num_ref_idx_l1_active_minus1 = sh.numRefIdxL1Minus1;
	ps->deblocking_filter_control_present_flag = pps.deblockingFilterControlPresent;
	ps->redundant_pic_cnt_present_flag = pps.redundantPicCntPresent;
	ps->transform_8x8_mode_flag = pps.transform8x8Mode;

	ps->pitch_luma = pitch / pitchUnits;
	ps->pitch_chroma = pitch / pitchUnits;
	ps->luma_top_offset = 0;
	ps->luma_bot_offset = 0;
	ps->luma_frame_offset = 0;
	ps->chroma_top_offset = 0;
	ps->chroma_bot_offset = 0;
	ps->chroma_frame_offset = 0;
	ps->HistBufferSize = (unsigned)(history.size / 256);

	ps->MbaffFrameFlag = sps.mbAdaptiveFrameField && !sh.fieldPic;
	ps->direct_8x8_inference_flag = sps.direct8x8Inference;
	ps->weighted_pred_flag = pps.weightedPred;
	ps->constrained_intra_pred_flag = pps.constrainedIntraPred;
	ps->ref_pic_flag = sh.nalRefIdc != 0;
	ps->field_pic_flag = sh.fieldPic;
	ps->bottom_field_flag = sh.bottomField;
	ps->second_field = 0;
	ps->log2_max_frame_num_minus4 = sps.log2MaxFrameNumMinus4;
	ps->chroma_format_idc = sps.chromaFormatIdc;
	ps->pic_order_cnt_type = sps.picOrderCntType;
	ps->pic_init_qp_minus26 = pps.picInitQpMinus26;
	ps->chroma_qp_index_offset = pps.chromaQpIndexOffset;
	ps->second_chroma_qp_index_offset = pps.secondChromaQpIndexOffset;
	ps->weighted_bipred_idc = pps.weightedBipredIdc;
	ps->CurrPicIdx = 0;
	ps->CurrColIdx = 0;
	ps->frame_num = sh.frameNum;
	ps->frame_surfaces = 1;
	ps->output_memory_layout = 0;		/* NV12 */
	ps->CurrFieldOrderCnt[0] = 0;
	ps->CurrFieldOrderCnt[1] = 0;
	ps->lossless_ipred8x8_filter_enable = 1;
	ps->qpprime_y_zero_transform_bypass_flag = sps.qpprimeYZeroTransformBypass;

	/* An IDR has no references, so every slot is empty. */
	for (int i = 0; i < 16; i++) {
		ps->dpb[i].index = 0x7f;
		ps->dpb[i].col_idx = 0x1f;
		ps->dpb[i].state = 0;
		ps->dpb[i].not_existing = 1;
	}

	const uint8_t (*s4)[16] = pps.scalingPresent ? pps.scaling4x4 : sps.scaling4x4;
	const uint8_t (*s8)[64] = pps.scalingPresent ? pps.scaling8x8 : sps.scaling8x8;
	for (int i = 0; i < 6; i++)
		memcpy(&ps->WeightScale[i][0][0], s4[i], 16);
	for (int i = 0; i < 2; i++)
		memcpy(&ps->WeightScale8x8[i][0][0], s8[i], 64);

	uint64_t lumaAddr = surface.gpuAddr;
	uint64_t chromaAddr = surface.gpuAddr + lumaSize;

	NvPush push;
	nvpBegin(&push, &gpu);
	nvpMethod(&push, NVC2B0_SET_OBJECT, NVC2B0_VIDEO_DECODER);
	nvpMethod(&push, NVC2B0_SET_APPLICATION_ID, NVDEC_APP_ID_H264);
	nvpMethod(&push, NVC2B0_SET_WATCHDOG_TIMER, 0);
	nvpMethod(&push, NVC2B0_SET_CONTROL_PARAMS,
		(uint32_t)envInt("NVDEC_CONTROL",
			NVDEC_CODEC_H264		/* CODEC_TYPE 3:0 */
			| (1 << 4)			/* GPTIMER_ON */
			| (1 << 5)));			/* RET_ERROR */
	nvpOffset(&push, NVC2B0_SET_DRV_PIC_SETUP_OFFSET, picSetup.gpuAddr);
	nvpOffset(&push, NVC2B0_SET_IN_BUF_BASE_OFFSET, bitstream.gpuAddr);
	nvpMethod(&push, NVC2B0_SET_PICTURE_INDEX, 0);
	nvpOffset(&push, NVC2B0_SET_SLICE_OFFSETS_BUF_OFFSET, sliceOffsets.gpuAddr);
	nvpOffset(&push, NVC2B0_SET_COLOC_DATA_OFFSET, coloc.gpuAddr);
	nvpOffset(&push, NVC2B0_SET_HISTORY_OFFSET, history.gpuAddr);
	nvpOffset(&push, NVC2B0_SET_NVDEC_STATUS_OFFSET, status.gpuAddr);
	nvpOffset(&push, NVC2B0_SET_PIC_SCRATCH_BUF_OFFSET, scratch.gpuAddr);
	nvpOffset(&push, NVC2B0_H264_SET_MBHIST_BUF_OFFSET, mbhist.gpuAddr);
	nvpOffset(&push, NVC2B0_SET_PICTURE_LUMA_OFFSET0, lumaAddr);
	nvpOffset(&push, NVC2B0_SET_PICTURE_CHROMA_OFFSET0, chromaAddr);
	nvpMethod(&push, NVC2B0_EXECUTE, 1u << 0);	/* NOTIFY */

	nvdec_status_s *st = status.map;
	memset(st, 0, sizeof(*st));

	bool answered = nvdecSubmit(&gpu, &push, 3000);
	NvNotification *notes = gpu.notifier.map;
	printf("engine answered: %s\n", answered ? "yes" : "no");
	printf("status: %u macroblocks decoded, %u in error, error %#x, "
		"slice header error %#x, %u cycles\n",
		st->mbs_correctly_decoded, st->mbs_in_error, st->error_status,
		st->slice_header_error_code, st->cycle_count);
	printf("error notifier: info32 %#x info16 %#x status %#x\n",
		notes[0].info32, notes[0].info16, notes[0].status);

	if (!answered) return 1;

	int settleMs = envInt("NVDEC_SETTLE_MS", 0);
	if (settleMs > 0) usleep(settleMs * 1000);

	FILE *out = fopen(argv[2], "wb");
	if (out == NULL) { perror(argv[2]); return 1; }
	if (envInt("NVDEC_RAW_SURFACE", 0) != 0) {
		fwrite(surface.map, 1, lumaSize + chromaSize, out);
	} else {
		uint8_t *plane = malloc((size_t)width * height);
		nvdecUntile(plane, surface.map, width, height, pitch);
		fwrite(plane, 1, (size_t)width * height, out);
		nvdecUntile(plane, (uint8_t *)surface.map + lumaSize, width, height / 2, pitch);
		fwrite(plane, 1, (size_t)width * (height / 2), out);
		free(plane);
	}
	fclose(out);
	printf("wrote %s (%d wide, %d high) to %s\n",
		envInt("NVDEC_RAW_SURFACE", 0) != 0 ? "the surface as the card holds it" : "NV12",
		width, height, argv[2]);
	return st->error_status == 0 ? 0 : 1;
}
