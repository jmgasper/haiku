/* See h264_parse.h. Section numbers are from ITU-T H.264 (08/2021). */

#include "h264_parse.h"

#include <string.h>

void
h264BitsInit(H264Bits *br, const uint8_t *data, size_t size)
{
	br->data = data;
	br->size = size;
	br->bytePos = 0;
	br->bitPos = 0;
}

uint32_t
h264Bit(H264Bits *br)
{
	if (br->bytePos >= br->size)
		return 0;
	uint32_t value = (br->data[br->bytePos] >> (7 - br->bitPos)) & 1;
	if (++br->bitPos == 8) {
		br->bitPos = 0;
		br->bytePos++;
	}
	return value;
}

uint32_t
h264Bits(H264Bits *br, int count)
{
	uint32_t value = 0;
	while (count-- > 0)
		value = (value << 1) | h264Bit(br);
	return value;
}

uint32_t
h264UE(H264Bits *br)
{
	int zeros = 0;
	while (zeros < 32 && br->bytePos < br->size && h264Bit(br) == 0)
		zeros++;
	if (zeros == 0)
		return 0;
	return (1u << zeros) - 1 + h264Bits(br, zeros);
}

int32_t
h264SE(H264Bits *br)
{
	uint32_t k = h264UE(br);
	return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

bool
h264MoreData(const H264Bits *br)
{
	/* There is more data unless what is left is the stop bit and padding. */
	if (br->bytePos >= br->size)
		return false;
	size_t lastByte = br->size;
	while (lastByte > 0 && br->data[lastByte - 1] == 0)
		lastByte--;
	if (lastByte == 0)
		return false;
	int stopBit = 0;
	for (int i = 0; i < 8; i++) {
		if ((br->data[lastByte - 1] >> i) & 1) {
			stopBit = 7 - i;
			break;
		}
	}
	return br->bytePos * 8 + br->bitPos < (lastByte - 1) * 8 + (size_t)stopBit;
}

size_t
h264ToRbsp(const uint8_t *in, size_t size, uint8_t *out)
{
	size_t written = 0;
	for (size_t i = 0; i < size; i++) {
		if (i + 2 < size && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 3) {
			out[written++] = 0;
			out[written++] = 0;
			i += 2;
			continue;
		}
		out[written++] = in[i];
	}
	return written;
}

/* Tables 8-13 and 8-14: scaling lists are sent in zig-zag order and the
 * decoder wants them in raster order. */
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
parseScalingList(H264Bits *br, uint8_t *list, int size, const uint8_t *zigzag,
	const uint8_t *fallback)
{
	int lastScale = 8, nextScale = 8;
	bool useDefault = false;
	for (int j = 0; j < size; j++) {
		if (nextScale != 0) {
			int delta = h264SE(br);
			nextScale = (lastScale + delta + 256) % 256;
			useDefault = (j == 0 && nextScale == 0);
		}
		int value = (nextScale == 0) ? lastScale : nextScale;
		list[zigzag[j]] = (uint8_t)value;
		lastScale = value;
	}
	if (useDefault) {
		for (int j = 0; j < size; j++)
			list[zigzag[j]] = fallback[j];
	}
}

static void
parseScalingMatrices(H264Bits *br, int count, int chromaFormatIdc,
	uint8_t scaling4x4[6][16], uint8_t scaling8x8[2][64])
{
	for (int i = 0; i < count; i++) {
		if (h264Bit(br) == 0)
			continue;
		if (i < 6) {
			parseScalingList(br, scaling4x4[i], 16, kZigzag4x4,
				(i < 3) ? kDefault4x4Intra : kDefault4x4Inter);
		} else {
			int k = i - 6;
			if (chromaFormatIdc != 3 && k > 1)
				break;
			parseScalingList(br, scaling8x8[k & 1], 64, kZigzag8x8,
				((k & 1) == 0) ? kDefault8x8Intra : kDefault8x8Inter);
		}
	}
}

static void
setFlatScaling(uint8_t scaling4x4[6][16], uint8_t scaling8x8[2][64])
{
	memset(scaling4x4, 16, 6 * 16);
	memset(scaling8x8, 16, 2 * 64);
}

static void
skipHrd(H264Bits *br)
{
	int cpbCnt = h264UE(br) + 1;
	h264Bits(br, 4);		/* bit_rate_scale */
	h264Bits(br, 4);		/* cpb_size_scale */
	for (int i = 0; i < cpbCnt; i++) {
		h264UE(br);		/* bit_rate_value_minus1 */
		h264UE(br);		/* cpb_size_value_minus1 */
		h264Bit(br);		/* cbr_flag */
	}
	h264Bits(br, 5);		/* initial_cpb_removal_delay_length_minus1 */
	h264Bits(br, 5);		/* cpb_removal_delay_length_minus1 */
	h264Bits(br, 5);		/* dpb_output_delay_length_minus1 */
	h264Bits(br, 5);		/* time_offset_length */
}

/* Annex E. Only the bounds on reordering are wanted; the rest is skipped. */
static void
parseVui(H264Bits *br, H264Sps *sps)
{
	sps->hasVui = true;
	if (h264Bit(br)) {			/* aspect_ratio_info_present_flag */
		int idc = h264Bits(br, 8);
		if (idc == 255) {
			h264Bits(br, 16);
			h264Bits(br, 16);
		}
	}
	if (h264Bit(br))			/* overscan_info_present_flag */
		h264Bit(br);
	if (h264Bit(br)) {			/* video_signal_type_present_flag */
		h264Bits(br, 3);
		h264Bit(br);
		if (h264Bit(br)) {		/* colour_description_present_flag */
			h264Bits(br, 8);
			h264Bits(br, 8);
			h264Bits(br, 8);
		}
	}
	if (h264Bit(br)) {			/* chroma_loc_info_present_flag */
		h264UE(br);
		h264UE(br);
	}
	if (h264Bit(br)) {			/* timing_info_present_flag */
		h264Bits(br, 32);
		h264Bits(br, 32);
		h264Bit(br);
	}
	bool nalHrd = h264Bit(br) != 0;
	if (nalHrd)
		skipHrd(br);
	bool vclHrd = h264Bit(br) != 0;
	if (vclHrd)
		skipHrd(br);
	if (nalHrd || vclHrd)
		h264Bit(br);			/* low_delay_hrd_flag */
	h264Bit(br);				/* pic_struct_present_flag */
	if (h264Bit(br)) {			/* bitstream_restriction_flag */
		h264Bit(br);			/* motion_vectors_over_pic_boundaries */
		h264UE(br);			/* max_bytes_per_pic_denom */
		h264UE(br);			/* max_bits_per_mb_denom */
		h264UE(br);			/* log2_max_mv_length_horizontal */
		h264UE(br);			/* log2_max_mv_length_vertical */
		sps->maxNumReorderFrames = h264UE(br);
		sps->maxDecFrameBuffering = h264UE(br);
		sps->hasReorderFrames = true;
	}
}

bool
h264ParseSps(const uint8_t *rbsp, size_t size, H264Sps *sps)
{
	H264Bits br;
	h264BitsInit(&br, rbsp, size);
	memset(sps, 0, sizeof(*sps));
	setFlatScaling(sps->scaling4x4, sps->scaling8x8);

	sps->profileIdc = h264Bits(&br, 8);
	h264Bits(&br, 8);			/* constraint flags and reserved */
	sps->levelIdc = h264Bits(&br, 8);
	sps->id = h264UE(&br);
	if (sps->id >= H264_MAX_SPS)
		return false;
	sps->chromaFormatIdc = 1;
	sps->bitDepthLuma = 8;
	sps->bitDepthChroma = 8;
	switch (sps->profileIdc) {
	case 100: case 110: case 122: case 244: case 44:
	case 83: case 86: case 118: case 128: case 138: case 139:
	case 134: case 135:
		sps->chromaFormatIdc = h264UE(&br);
		if (sps->chromaFormatIdc == 3)
			sps->separateColourPlane = h264Bit(&br);
		sps->bitDepthLuma = 8 + h264UE(&br);
		sps->bitDepthChroma = 8 + h264UE(&br);
		sps->qpprimeYZeroTransformBypass = h264Bit(&br);
		if (h264Bit(&br)) {
			parseScalingMatrices(&br, (sps->chromaFormatIdc != 3) ? 8 : 12,
				sps->chromaFormatIdc, sps->scaling4x4, sps->scaling8x8);
		}
		break;
	}
	sps->log2MaxFrameNumMinus4 = h264UE(&br);
	sps->picOrderCntType = h264UE(&br);
	if (sps->picOrderCntType == 0) {
		sps->log2MaxPocLsbMinus4 = h264UE(&br);
	} else if (sps->picOrderCntType == 1) {
		sps->deltaPicOrderAlwaysZero = h264Bit(&br);
		sps->offsetForNonRefPic = h264SE(&br);
		sps->offsetForTopToBottomField = h264SE(&br);
		sps->numRefFramesInPocCycle = h264UE(&br);
		if (sps->numRefFramesInPocCycle > 255)
			return false;
		for (int i = 0; i < sps->numRefFramesInPocCycle; i++)
			sps->offsetForRefFrame[i] = h264SE(&br);
	}
	sps->maxNumRefFrames = h264UE(&br);
	h264Bit(&br);				/* gaps_in_frame_num_allowed_flag */
	sps->picWidthInMbs = h264UE(&br) + 1;
	sps->picHeightInMapUnits = h264UE(&br) + 1;
	sps->frameMbsOnly = h264Bit(&br);
	if (!sps->frameMbsOnly)
		sps->mbAdaptiveFrameField = h264Bit(&br);
	sps->direct8x8Inference = h264Bit(&br);
	if (h264Bit(&br)) {			/* frame_cropping_flag */
		sps->cropLeft = h264UE(&br);
		sps->cropRight = h264UE(&br);
		sps->cropTop = h264UE(&br);
		sps->cropBottom = h264UE(&br);
	}
	if (h264Bit(&br))			/* vui_parameters_present_flag */
		parseVui(&br, sps);
	sps->valid = true;
	return true;
}

bool
h264ParsePps(const uint8_t *rbsp, size_t size, const H264ParamSets *sets, H264Pps *pps)
{
	H264Bits br;
	h264BitsInit(&br, rbsp, size);
	memset(pps, 0, sizeof(*pps));
	setFlatScaling(pps->scaling4x4, pps->scaling8x8);

	pps->id = h264UE(&br);
	pps->spsId = h264UE(&br);
	if (pps->id >= H264_MAX_PPS || pps->spsId >= H264_MAX_SPS)
		return false;
	const H264Sps *sps = &sets->sps[pps->spsId];
	if (!sps->valid)
		return false;
	pps->entropyCodingMode = h264Bit(&br);
	pps->picOrderPresent = h264Bit(&br);
	if (h264UE(&br) != 0)			/* num_slice_groups_minus1 */
		return false;			/* slice groups are not supported */
	pps->numRefIdxL0Minus1 = h264UE(&br);
	pps->numRefIdxL1Minus1 = h264UE(&br);
	pps->weightedPred = h264Bit(&br);
	pps->weightedBipredIdc = h264Bits(&br, 2);
	pps->picInitQpMinus26 = h264SE(&br);
	h264SE(&br);				/* pic_init_qs_minus26 */
	pps->chromaQpIndexOffset = h264SE(&br);
	pps->deblockingFilterControlPresent = h264Bit(&br);
	pps->constrainedIntraPred = h264Bit(&br);
	pps->redundantPicCntPresent = h264Bit(&br);
	pps->secondChromaQpIndexOffset = pps->chromaQpIndexOffset;
	memcpy(pps->scaling4x4, sps->scaling4x4, sizeof(pps->scaling4x4));
	memcpy(pps->scaling8x8, sps->scaling8x8, sizeof(pps->scaling8x8));
	if (h264MoreData(&br)) {
		pps->transform8x8Mode = h264Bit(&br);
		if (h264Bit(&br)) {		/* pic_scaling_matrix_present_flag */
			pps->scalingPresent = true;
			int count = 6 + (sps->chromaFormatIdc != 3 ? 2 : 6) * pps->transform8x8Mode;
			parseScalingMatrices(&br, count, sps->chromaFormatIdc,
				pps->scaling4x4, pps->scaling8x8);
		}
		pps->secondChromaQpIndexOffset = h264SE(&br);
	}
	pps->valid = true;
	return true;
}

/* 7.3.3.1 */
static void
parseRefPicListModification(H264Bits *br, H264Slice *slice)
{
	for (int list = 0; list < 2; list++) {
		if (list == 0 && (slice->sliceType == 2 || slice->sliceType == 4))
			continue;
		if (list == 1 && slice->sliceType != 1)
			continue;
		if (!h264Bit(br))
			continue;
		for (;;) {
			int idc = h264UE(br);
			if (idc == 3)
				break;
			int value = h264UE(br);
			if (slice->listModCount[list] < H264_MAX_LIST_MODS) {
				H264ListMod *mod = &slice->listMod[list][slice->listModCount[list]++];
				mod->idc = idc;
				mod->value = value;
			}
		}
	}
}

/* 7.3.3.2 */
static void
skipPredWeightTable(H264Bits *br, const H264Slice *slice, int chromaArrayType)
{
	h264UE(br);				/* luma_log2_weight_denom */
	if (chromaArrayType != 0)
		h264UE(br);			/* chroma_log2_weight_denom */
	for (int list = 0; list < (slice->sliceType == 1 ? 2 : 1); list++) {
		int count = (list == 0 ? slice->numRefIdxL0Minus1 : slice->numRefIdxL1Minus1) + 1;
		for (int i = 0; i < count; i++) {
			if (h264Bit(br)) {
				h264SE(br);
				h264SE(br);
			}
			if (chromaArrayType != 0 && h264Bit(br)) {
				for (int j = 0; j < 2; j++) {
					h264SE(br);
					h264SE(br);
				}
			}
		}
	}
}

/* 7.3.3.3 */
static void
parseDecRefPicMarking(H264Bits *br, H264Slice *slice)
{
	if (slice->idr) {
		slice->noOutputOfPriorPics = h264Bit(br);
		slice->longTermReference = h264Bit(br);
		return;
	}
	slice->adaptiveRefPicMarking = h264Bit(br);
	if (!slice->adaptiveRefPicMarking)
		return;
	for (;;) {
		int op = h264UE(br);
		if (op == 0 || slice->mmcoCount >= H264_MAX_MMCO)
			break;
		H264Mmco *mmco = &slice->mmco[slice->mmcoCount++];
		memset(mmco, 0, sizeof(*mmco));
		mmco->op = op;
		if (op == 1 || op == 3)
			mmco->differenceOfPicNumsMinus1 = h264UE(br);
		if (op == 2)
			mmco->longTermPicNum = h264UE(br);
		if (op == 3 || op == 6)
			mmco->longTermFrameIdx = h264UE(br);
		if (op == 4)
			mmco->maxLongTermFrameIdxPlus1 = h264UE(br);
	}
}

bool
h264ParseSliceHeader(const uint8_t *rbsp, size_t size, const H264ParamSets *sets,
	int nalType, int nalRefIdc, H264Slice *slice)
{
	H264Bits br;
	h264BitsInit(&br, rbsp, size);
	memset(slice, 0, sizeof(*slice));
	slice->nalType = nalType;
	slice->nalRefIdc = nalRefIdc;
	slice->idr = (nalType == 5);

	slice->firstMbInSlice = h264UE(&br);
	slice->sliceType = h264UE(&br);
	if (slice->sliceType >= 5)
		slice->sliceType -= 5;
	if (slice->sliceType > 4)
		return false;
	slice->ppsId = h264UE(&br);
	if (slice->ppsId >= H264_MAX_PPS || !sets->pps[slice->ppsId].valid)
		return false;
	const H264Pps *pps = &sets->pps[slice->ppsId];
	const H264Sps *sps = &sets->sps[pps->spsId];
	if (!sps->valid)
		return false;
	int chromaArrayType = sps->separateColourPlane ? 0 : sps->chromaFormatIdc;

	if (sps->separateColourPlane)
		h264Bits(&br, 2);		/* colour_plane_id */
	slice->frameNum = h264Bits(&br, sps->log2MaxFrameNumMinus4 + 4);
	if (!sps->frameMbsOnly) {
		slice->fieldPic = h264Bit(&br);
		if (slice->fieldPic)
			slice->bottomField = h264Bit(&br);
	}
	if (slice->idr)
		slice->idrPicId = h264UE(&br);
	if (sps->picOrderCntType == 0) {
		slice->pocLsb = h264Bits(&br, sps->log2MaxPocLsbMinus4 + 4);
		if (pps->picOrderPresent && !slice->fieldPic)
			slice->deltaPocBottom = h264SE(&br);
	} else if (sps->picOrderCntType == 1 && !sps->deltaPicOrderAlwaysZero) {
		slice->deltaPoc[0] = h264SE(&br);
		if (pps->picOrderPresent && !slice->fieldPic)
			slice->deltaPoc[1] = h264SE(&br);
	}
	if (pps->redundantPicCntPresent)
		h264UE(&br);			/* redundant_pic_cnt */

	slice->numRefIdxL0Minus1 = pps->numRefIdxL0Minus1;
	slice->numRefIdxL1Minus1 = pps->numRefIdxL1Minus1;
	if (slice->sliceType == 1)
		h264Bit(&br);			/* direct_spatial_mv_pred_flag */
	if (slice->sliceType == 0 || slice->sliceType == 1 || slice->sliceType == 3) {
		if (h264Bit(&br)) {		/* num_ref_idx_active_override_flag */
			slice->numRefIdxL0Minus1 = h264UE(&br);
			if (slice->sliceType == 1)
				slice->numRefIdxL1Minus1 = h264UE(&br);
		}
	}
	parseRefPicListModification(&br, slice);
	if ((pps->weightedPred && (slice->sliceType == 0 || slice->sliceType == 3))
		|| (pps->weightedBipredIdc == 1 && slice->sliceType == 1)) {
		skipPredWeightTable(&br, slice, chromaArrayType);
	}
	if (nalRefIdc != 0)
		parseDecRefPicMarking(&br, slice);
	return true;
}

/* Table A-1, as MaxDpbMbs; the frame count follows from the picture size. */
int
h264MaxDpbFrames(const H264Sps *sps)
{
	static const struct { int level; int maxDpbMbs; } kLevels[] = {
		{ 10, 396 }, { 11, 900 }, { 12, 2376 }, { 13, 2376 },
		{ 20, 2376 }, { 21, 4752 }, { 22, 8100 },
		{ 30, 8100 }, { 31, 18000 }, { 32, 20480 },
		{ 40, 32768 }, { 41, 32768 }, { 42, 34816 },
		{ 50, 110400 }, { 51, 184320 }, { 52, 184320 },
		{ 60, 696320 }, { 61, 696320 }, { 62, 696320 },
	};
	int maxDpbMbs = 184320;
	for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); i++) {
		if (kLevels[i].level >= sps->levelIdc) {
			maxDpbMbs = kLevels[i].maxDpbMbs;
			break;
		}
	}
	int mbs = sps->picWidthInMbs * sps->picHeightInMapUnits
		* (sps->frameMbsOnly ? 1 : 2);
	int frames = mbs > 0 ? maxDpbMbs / mbs : 16;
	if (frames > 16)
		frames = 16;
	if (frames < 1)
		frames = 1;
	if (sps->hasReorderFrames && sps->maxDecFrameBuffering > 0
		&& sps->maxDecFrameBuffering < frames) {
		frames = sps->maxDecFrameBuffering;
	}
	if (frames < sps->maxNumRefFrames)
		frames = sps->maxNumRefFrames;
	return frames;
}
