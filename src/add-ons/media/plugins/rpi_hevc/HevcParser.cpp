/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "HevcParser.h"

#include <algorithm>
#include <string.h>


namespace hevc {


static int
ceil_log2(uint32_t value)
{
	int bits = 0;
	while ((1u << bits) < value)
		bits++;
	return bits;
}


//	#pragma mark - BitReader


BitReader::BitReader()
	:
	fPosition(0),
	fFailed(false)
{
}


/*!	Takes the payload of \a nal (what follows its two header bytes), at most
	\a maxBytes of it.
*/
void
BitReader::SetTo(const uint8_t* nal, size_t size, size_t maxBytes)
{
	fData.clear();
	fRemoved.clear();
	fPosition = 0;
	fFailed = size < 2;

	int zeros = 0;
	for (size_t i = 2; i < size && fData.size() < maxBytes; i++) {
		uint8_t byte = nal[i];
		if (zeros >= 2 && byte == 3) {
			fRemoved.push_back((uint32_t)fData.size());
			zeros = 0;
			continue;
		}
		zeros = byte == 0 ? zeros + 1 : 0;
		fData.push_back(byte);
	}
}


uint32_t
BitReader::Bits(int count)
{
	uint32_t value = 0;
	while (count > 0) {
		size_t byte = fPosition >> 3;
		if (byte >= fData.size()) {
			fFailed = true;
			return value << count;
		}
		int available = 8 - (int)(fPosition & 7);
		int take = std::min(available, count);
		uint32_t bits = (fData[byte] >> (available - take)) & ((1u << take) - 1);
		value = (value << take) | bits;
		fPosition += take;
		count -= take;
	}
	return value;
}


uint32_t
BitReader::UE()
{
	int zeros = 0;
	while (!Flag()) {
		if (fFailed || ++zeros > 32) {
			fFailed = true;
			return 0;
		}
	}
	if (zeros == 0)
		return 0;
	if (zeros == 32)
		return 0xffffffffu;
	return (1u << zeros) - 1 + Bits(zeros);
}


int32_t
BitReader::SE()
{
	uint32_t value = UE();
	if ((value & 1) != 0)
		return (int32_t)((value + 1) >> 1);
	return -(int32_t)(value >> 1);
}


void
BitReader::Skip(size_t count)
{
	fPosition += count;
	if (fPosition > fData.size() * 8)
		fFailed = true;
}


size_t
BitReader::OffsetInNal(size_t index) const
{
	size_t removed = std::upper_bound(fRemoved.begin(), fRemoved.end(),
		(uint32_t)index) - fRemoved.begin();
	return 2 + index + removed;
}


//	#pragma mark - scaling lists


static const uint8_t kDefaultIntra[64] = {
	16, 16, 16, 16, 17, 18, 21, 24,
	16, 16, 16, 16, 17, 19, 22, 25,
	16, 16, 17, 18, 20, 22, 25, 29,
	16, 16, 18, 21, 24, 27, 31, 36,
	17, 17, 20, 24, 30, 35, 41, 47,
	18, 19, 22, 27, 35, 44, 54, 65,
	21, 22, 25, 31, 41, 54, 70, 88,
	24, 25, 29, 36, 47, 65, 88, 115
};

static const uint8_t kDefaultInter[64] = {
	16, 16, 16, 16, 17, 18, 20, 24,
	16, 16, 16, 17, 18, 20, 24, 25,
	16, 16, 17, 18, 20, 24, 25, 28,
	16, 17, 18, 20, 24, 25, 28, 33,
	17, 18, 20, 24, 25, 28, 33, 41,
	18, 20, 24, 25, 28, 33, 41, 54,
	20, 24, 25, 28, 33, 41, 54, 71,
	24, 25, 28, 33, 41, 54, 71, 91
};


/*!	The up-right diagonal scan of a block (6.5.3): the raster position of
	each coefficient in coding order.
*/
static void
diagonal_scan(int size, uint8_t* positions)
{
	int i = 0;
	int x = 0;
	int y = 0;
	while (i < size * size) {
		while (y >= 0) {
			if (x < size && y < size)
				positions[i++] = (uint8_t)(y * size + x);
			y--;
			x++;
		}
		y = x;
		x = 0;
	}
}


void
ScalingList::SetDefault()
{
	memset(list4x4, 16, sizeof(list4x4));
	for (int matrix = 0; matrix < 6; matrix++) {
		const uint8_t* source = matrix < 3 ? kDefaultIntra : kDefaultInter;
		memcpy(list8x8[matrix], source, 64);
		memcpy(list16x16[matrix], source, 64);
		dc16x16[matrix] = 16;
	}
	memcpy(list32x32[0], kDefaultIntra, 64);
	memcpy(list32x32[1], kDefaultInter, 64);
	dc32x32[0] = dc32x32[1] = 16;
}


bool
ScalingList::Parse(BitReader& reader)
{
	uint8_t scan4x4[16];
	uint8_t scan8x8[64];
	diagonal_scan(4, scan4x4);
	diagonal_scan(8, scan8x8);

	SetDefault();

	for (int size = 0; size < 4; size++) {
		int matrices = size == 3 ? 2 : 6;
		for (int matrix = 0; matrix < matrices; matrix++) {
			uint8_t* list = size == 0 ? list4x4[matrix]
				: size == 1 ? list8x8[matrix]
				: size == 2 ? list16x16[matrix] : list32x32[matrix];
			uint8_t* dc = size == 2 ? &dc16x16[matrix]
				: size == 3 ? &dc32x32[matrix] : NULL;
			int count = size == 0 ? 16 : 64;

			if (!reader.Flag()) {
				// as another matrix of the size, or the default
				uint32_t delta = reader.UE();
				if (delta > (uint32_t)matrix)
					return false;
				if (delta > 0) {
					memcpy(list, list - delta * count, count);
					if (dc != NULL)
						*dc = *(dc - delta);
				}
				continue;
			}

			int next = 8;
			if (dc != NULL) {
				next = reader.SE() + 8;
				*dc = (uint8_t)next;
			}
			for (int i = 0; i < count; i++) {
				int position = size == 0 ? scan4x4[i] : scan8x8[i];
				next = (next + reader.SE() + 256) & 255;
				list[position] = (uint8_t)next;
			}
		}
	}
	return !reader.Failed();
}


//	#pragma mark - parameter sets


static bool
parse_short_term_set(BitReader& reader, ShortTermSet& set, int index,
	int count, const ShortTermSet* sets)
{
	memset(&set, 0, sizeof(set));

	bool predicted = index != 0 && reader.Flag();
	if (!predicted) {
		uint32_t negative = reader.UE();
		uint32_t positive = reader.UE();
		if (negative > (uint32_t)kMaxReferences
			|| positive > (uint32_t)kMaxReferences
			|| negative + positive > (uint32_t)kMaxReferences) {
			return false;
		}
		set.numNegative = (uint8_t)negative;
		set.numPositive = (uint8_t)positive;
		int32_t poc = 0;
		for (uint32_t i = 0; i < negative; i++) {
			poc -= (int32_t)reader.UE() + 1;
			set.deltaPocS0[i] = poc;
			set.usedS0[i] = reader.Flag();
		}
		poc = 0;
		for (uint32_t i = 0; i < positive; i++) {
			poc += (int32_t)reader.UE() + 1;
			set.deltaPocS1[i] = poc;
			set.usedS1[i] = reader.Flag();
		}
		return !reader.Failed();
	}

	// from another set, each of its pictures moved by the same distance
	uint32_t deltaIndex = 1;
	if (index == count)
		deltaIndex = reader.UE() + 1;
	if (deltaIndex > (uint32_t)index)
		return false;
	const ShortTermSet& reference = sets[index - deltaIndex];

	bool sign = reader.Flag();
	int32_t deltaRps = (int32_t)reader.UE() + 1;
	if (sign)
		deltaRps = -deltaRps;

	int total = reference.NumDeltaPocs();
	bool used[2 * kMaxReferences + 1];
	bool useDelta[2 * kMaxReferences + 1];
	for (int j = 0; j <= total; j++) {
		used[j] = reader.Flag();
		useDelta[j] = used[j] || reader.Flag();
	}

	int i = 0;
	for (int j = reference.numPositive - 1; j >= 0; j--) {
		int32_t poc = reference.deltaPocS1[j] + deltaRps;
		if (poc < 0 && useDelta[reference.numNegative + j]
			&& i < kMaxReferences) {
			set.deltaPocS0[i] = poc;
			set.usedS0[i++] = used[reference.numNegative + j];
		}
	}
	if (deltaRps < 0 && useDelta[total] && i < kMaxReferences) {
		set.deltaPocS0[i] = deltaRps;
		set.usedS0[i++] = used[total];
	}
	for (int j = 0; j < reference.numNegative; j++) {
		int32_t poc = reference.deltaPocS0[j] + deltaRps;
		if (poc < 0 && useDelta[j] && i < kMaxReferences) {
			set.deltaPocS0[i] = poc;
			set.usedS0[i++] = used[j];
		}
	}
	set.numNegative = (uint8_t)i;

	i = 0;
	for (int j = reference.numNegative - 1; j >= 0; j--) {
		int32_t poc = reference.deltaPocS0[j] + deltaRps;
		if (poc > 0 && useDelta[j] && i < kMaxReferences) {
			set.deltaPocS1[i] = poc;
			set.usedS1[i++] = used[j];
		}
	}
	if (deltaRps > 0 && useDelta[total] && i < kMaxReferences) {
		set.deltaPocS1[i] = deltaRps;
		set.usedS1[i++] = used[total];
	}
	for (int j = 0; j < reference.numPositive; j++) {
		int32_t poc = reference.deltaPocS1[j] + deltaRps;
		if (poc > 0 && useDelta[reference.numNegative + j]
			&& i < kMaxReferences) {
			set.deltaPocS1[i] = poc;
			set.usedS1[i++] = used[reference.numNegative + j];
		}
	}
	set.numPositive = (uint8_t)i;

	return !reader.Failed()
		&& set.numNegative + set.numPositive <= kMaxReferences;
}


static void
skip_profile_tier_level(BitReader& reader, int maxSubLayersMinus1,
	uint8_t* _profile, uint32_t* _compatibility, uint8_t* _level)
{
	reader.Bits(3);		// profile space, tier
	*_profile = (uint8_t)reader.Bits(5);
	*_compatibility = reader.Bits(32);
	reader.Skip(48);	// source flags, reserved
	*_level = (uint8_t)reader.Bits(8);

	bool profilePresent[8];
	bool levelPresent[8];
	for (int i = 0; i < maxSubLayersMinus1; i++) {
		profilePresent[i] = reader.Flag();
		levelPresent[i] = reader.Flag();
	}
	if (maxSubLayersMinus1 > 0)
		reader.Skip(2 * (8 - maxSubLayersMinus1));
	for (int i = 0; i < maxSubLayersMinus1; i++) {
		if (profilePresent[i])
			reader.Skip(88);
		if (levelPresent[i])
			reader.Skip(8);
	}
}


bool
SPS::Parse(const uint8_t* nal, size_t size)
{
	BitReader reader;
	reader.SetTo(nal, size);

	memset(this, 0, sizeof(*this));

	reader.Bits(4);		// video parameter set
	maxSubLayersMinus1 = (uint8_t)reader.Bits(3);
	reader.Flag();		// temporal id nesting
	if (maxSubLayersMinus1 > 6)
		return false;
	skip_profile_tier_level(reader, maxSubLayersMinus1, &profile,
		&profileCompatibility, &level);

	uint32_t value = reader.UE();
	if (value >= (uint32_t)kMaxSpsCount)
		return false;
	id = (uint8_t)value;

	chromaFormat = (uint8_t)reader.UE();
	if (chromaFormat == 3)
		separateColourPlane = reader.Flag();
	width = reader.UE();
	height = reader.UE();
	if (reader.Flag()) {
		// in chroma samples of 4:2:0
		cropLeft = reader.UE() * 2;
		cropRight = reader.UE() * 2;
		cropTop = reader.UE() * 2;
		cropBottom = reader.UE() * 2;
	}
	bitDepthLuma = (uint8_t)(reader.UE() + 8);
	bitDepthChroma = (uint8_t)(reader.UE() + 8);
	log2MaxPocLsb = (uint8_t)(reader.UE() + 4);
	if (log2MaxPocLsb > 16)
		return false;

	bool orderingPresent = reader.Flag();
	for (int i = orderingPresent ? 0 : maxSubLayersMinus1;
			i <= maxSubLayersMinus1; i++) {
		maxDecPicBuffering = (uint8_t)std::min(reader.UE() + 1, 255u);
		maxNumReorder = (uint8_t)std::min(reader.UE(), 255u);
		maxLatencyIncreasePlus1 = reader.UE();
	}

	log2MinCbSize = (uint8_t)(reader.UE() + 3);
	log2DiffMaxMinCbSize = (uint8_t)reader.UE();
	log2MinTbSize = (uint8_t)(reader.UE() + 2);
	log2DiffMaxMinTbSize = (uint8_t)reader.UE();
	maxTransformHierarchyDepthInter = (uint8_t)reader.UE();
	maxTransformHierarchyDepthIntra = (uint8_t)reader.UE();

	scalingListEnabled = reader.Flag();
	scalingList.SetDefault();
	if (scalingListEnabled) {
		scalingListPresent = reader.Flag();
		if (scalingListPresent && !scalingList.Parse(reader))
			return false;
	}

	ampEnabled = reader.Flag();
	saoEnabled = reader.Flag();
	pcmEnabled = reader.Flag();
	if (pcmEnabled) {
		pcmBitDepthLuma = (uint8_t)(reader.Bits(4) + 1);
		pcmBitDepthChroma = (uint8_t)(reader.Bits(4) + 1);
		log2MinPcmCbSize = (uint8_t)(reader.UE() + 3);
		log2DiffMaxMinPcmCbSize = (uint8_t)reader.UE();
		pcmLoopFilterDisabled = reader.Flag();
	}

	value = reader.UE();
	if (value > 64)
		return false;
	numShortTermSets = (uint8_t)value;
	for (int i = 0; i < numShortTermSets; i++) {
		if (!parse_short_term_set(reader, shortTermSets[i], i,
				numShortTermSets, shortTermSets)) {
			return false;
		}
	}

	longTermPresent = reader.Flag();
	if (longTermPresent) {
		value = reader.UE();
		if (value > 32)
			return false;
		numLongTermSps = (uint8_t)value;
		for (int i = 0; i < numLongTermSps; i++) {
			longTermPocLsb[i] = (uint16_t)reader.Bits(log2MaxPocLsb);
			longTermUsed[i] = reader.Flag();
		}
	}

	temporalMvpEnabled = reader.Flag();
	strongIntraSmoothing = reader.Flag();
	// The rest (how to show the pictures, extensions) is not needed here.

	if (reader.Failed())
		return false;

	log2CtbSize = log2MinCbSize + log2DiffMaxMinCbSize;
	if (log2CtbSize < 4 || log2CtbSize > 6 || width == 0 || height == 0
		|| width > 8192 || height > 8192 || log2MinTbSize > 5
		|| log2MinTbSize + log2DiffMaxMinTbSize > 5) {
		return false;
	}
	ctbWidth = (width + (1u << log2CtbSize) - 1) >> log2CtbSize;
	ctbHeight = (height + (1u << log2CtbSize) - 1) >> log2CtbSize;
	if (cropLeft + cropRight >= width || cropTop + cropBottom >= height)
		cropLeft = cropRight = cropTop = cropBottom = 0;

	valid = true;
	return true;
}


bool
PPS::Parse(const uint8_t* nal, size_t size)
{
	BitReader reader;
	reader.SetTo(nal, size);

	memset(this, 0, sizeof(*this));

	uint32_t value = reader.UE();
	if (value >= (uint32_t)kMaxPpsCount)
		return false;
	id = (uint8_t)value;
	value = reader.UE();
	if (value >= (uint32_t)kMaxSpsCount)
		return false;
	spsId = (uint8_t)value;

	dependentSliceSegmentsEnabled = reader.Flag();
	outputFlagPresent = reader.Flag();
	numExtraSliceHeaderBits = (uint8_t)reader.Bits(3);
	signDataHiding = reader.Flag();
	cabacInitPresent = reader.Flag();
	numRefIdxDefault[0] = (uint8_t)std::min(reader.UE() + 1, 16u);
	numRefIdxDefault[1] = (uint8_t)std::min(reader.UE() + 1, 16u);
	initQpMinus26 = (int8_t)reader.SE();
	constrainedIntraPred = reader.Flag();
	transformSkipEnabled = reader.Flag();
	cuQpDeltaEnabled = reader.Flag();
	if (cuQpDeltaEnabled)
		diffCuQpDeltaDepth = (uint8_t)reader.UE();
	cbQpOffset = (int8_t)reader.SE();
	crQpOffset = (int8_t)reader.SE();
	sliceChromaQpOffsetsPresent = reader.Flag();
	weightedPred = reader.Flag();
	weightedBipred = reader.Flag();
	transquantBypassEnabled = reader.Flag();
	tilesEnabled = reader.Flag();
	entropyCodingSyncEnabled = reader.Flag();

	numTileColumns = numTileRows = 1;
	uniformSpacing = true;
	loopFilterAcrossTiles = true;
	if (tilesEnabled) {
		uint32_t columns = reader.UE() + 1;
		uint32_t rows = reader.UE() + 1;
		if (columns > (uint32_t)kMaxTileColumns
			|| rows > (uint32_t)kMaxTileRows) {
			return false;
		}
		numTileColumns = (uint8_t)columns;
		numTileRows = (uint8_t)rows;
		uniformSpacing = reader.Flag();
		if (!uniformSpacing) {
			for (uint32_t i = 0; i + 1 < columns; i++)
				columnWidth[i] = (uint16_t)(reader.UE() + 1);
			for (uint32_t i = 0; i + 1 < rows; i++)
				rowHeight[i] = (uint16_t)(reader.UE() + 1);
		}
		loopFilterAcrossTiles = reader.Flag();
	}

	loopFilterAcrossSlices = reader.Flag();
	if (reader.Flag()) {
		deblockingOverrideEnabled = reader.Flag();
		deblockingDisabled = reader.Flag();
		if (!deblockingDisabled) {
			betaOffsetDiv2 = (int8_t)reader.SE();
			tcOffsetDiv2 = (int8_t)reader.SE();
		}
	}

	scalingListPresent = reader.Flag();
	if (scalingListPresent && !scalingList.Parse(reader))
		return false;

	listsModificationPresent = reader.Flag();
	log2ParallelMergeLevel = (uint8_t)(reader.UE() + 2);
	sliceHeaderExtensionPresent = reader.Flag();

	if (reader.Flag()) {
		// extensions: the range extension changes the slice header
		if (reader.Flag())
			return false;
	}

	if (reader.Failed())
		return false;
	valid = true;
	return true;
}


//	#pragma mark - slice segment header


int
SliceHeader::NumPicTotalCurr() const
{
	int count = 0;
	for (int i = 0; i < shortTermSet.numNegative; i++)
		count += shortTermSet.usedS0[i] ? 1 : 0;
	for (int i = 0; i < shortTermSet.numPositive; i++)
		count += shortTermSet.usedS1[i] ? 1 : 0;
	for (int i = 0; i < numLongTerm; i++)
		count += longTermUsed[i] ? 1 : 0;
	return count;
}


static bool
parse_pred_weight_table(BitReader& reader, SliceHeader& header)
{
	PredWeightTable& table = header.weights;
	memset(&table, 0, sizeof(table));

	uint32_t lumaDenom = reader.UE();
	int32_t chromaDenom = (int32_t)lumaDenom + reader.SE();
	if (lumaDenom > 7 || chromaDenom < 0 || chromaDenom > 7)
		return false;
	table.lumaLog2Denom = (uint8_t)lumaDenom;
	table.chromaLog2Denom = (uint8_t)chromaDenom;

	int lists = header.type == SLICE_B ? 2 : 1;
	for (int list = 0; list < lists; list++) {
		bool lumaFlag[kMaxReferences];
		bool chromaFlag[kMaxReferences];
		for (int i = 0; i < header.numRefIdx[list]; i++)
			lumaFlag[i] = reader.Flag();
		for (int i = 0; i < header.numRefIdx[list]; i++)
			chromaFlag[i] = reader.Flag();

		for (int i = 0; i < header.numRefIdx[list]; i++) {
			table.lumaWeight[list][i] = (int16_t)(1 << lumaDenom);
			if (lumaFlag[i]) {
				table.lumaWeight[list][i] += (int16_t)reader.SE();
				table.lumaOffset[list][i] = (int16_t)reader.SE();
			}
			for (int j = 0; j < 2; j++) {
				table.chromaWeight[list][i][j] = (int16_t)(1 << chromaDenom);
				if (!chromaFlag[i])
					continue;
				table.chromaWeight[list][i][j] += (int16_t)reader.SE();
				int32_t offset = reader.SE() + 128
					- ((128 * table.chromaWeight[list][i][j]) >> chromaDenom);
				table.chromaOffset[list][i][j]
					= (int16_t)std::max(-128, std::min(127, offset));
			}
		}
	}
	return !reader.Failed();
}


bool
parse_slice_header(const uint8_t* nal, size_t size, const SPS* spsList,
	const PPS* ppsList, const SliceHeader* previous, SliceHeader& header)
{
	if (size < 3)
		return false;

	BitReader reader;
	reader.SetTo(nal, size);

	uint8_t nalType = (nal[0] >> 1) & 0x3f;
	uint8_t temporalId = (nal[1] & 7) - 1;

	bool first = reader.Flag();
	bool noOutputOfPriorPics = false;
	if (nalType >= NAL_BLA_W_LP && nalType <= NAL_IRAP_LAST)
		noOutputOfPriorPics = reader.Flag();
	uint32_t ppsId = reader.UE();
	if (ppsId >= (uint32_t)kMaxPpsCount || !ppsList[ppsId].valid)
		return false;
	const PPS& pps = ppsList[ppsId];
	if (!spsList[pps.spsId].valid)
		return false;
	const SPS& sps = spsList[pps.spsId];

	bool dependent = false;
	uint32_t address = 0;
	if (!first) {
		if (pps.dependentSliceSegmentsEnabled)
			dependent = reader.Flag();
		address = reader.Bits(ceil_log2(sps.ctbWidth * sps.ctbHeight));
		if (address >= sps.ctbWidth * sps.ctbHeight)
			return false;
	}

	if (dependent) {
		if (previous == NULL)
			return false;
		header = *previous;
	} else
		memset(&header, 0, sizeof(header));

	header.nalType = nalType;
	header.temporalId = temporalId;
	header.firstSliceSegmentInPic = first;
	header.noOutputOfPriorPics = noOutputOfPriorPics;
	header.ppsId = (uint8_t)ppsId;
	header.dependentSliceSegment = dependent;
	header.segmentAddress = address;

	if (!dependent) {
		reader.Skip(pps.numExtraSliceHeaderBits);
		uint32_t type = reader.UE();
		if (type > SLICE_I)
			return false;
		header.type = (uint8_t)type;
		header.picOutput = true;
		if (pps.outputFlagPresent)
			header.picOutput = reader.Flag();
		if (sps.separateColourPlane)
			reader.Bits(2);

		if (nalType != NAL_IDR_W_RADL && nalType != NAL_IDR_N_LP) {
			header.pocLsb = reader.Bits(sps.log2MaxPocLsb);

			header.shortTermSetFromSps = reader.Flag();
			if (!header.shortTermSetFromSps) {
				if (!parse_short_term_set(reader, header.shortTermSet,
						sps.numShortTermSets, sps.numShortTermSets,
						sps.shortTermSets)) {
					return false;
				}
			} else {
				if (sps.numShortTermSets == 0)
					return false;
				uint32_t index = 0;
				if (sps.numShortTermSets > 1)
					index = reader.Bits(ceil_log2(sps.numShortTermSets));
				if (index >= sps.numShortTermSets)
					return false;
				header.shortTermSetIndex = (uint8_t)index;
				header.shortTermSet = sps.shortTermSets[index];
			}

			if (sps.longTermPresent) {
				uint32_t fromSps = 0;
				if (sps.numLongTermSps > 0)
					fromSps = reader.UE();
				uint32_t own = reader.UE();
				if (fromSps > sps.numLongTermSps
					|| fromSps + own > (uint32_t)kMaxReferences) {
					return false;
				}
				header.numLongTerm = (uint8_t)(fromSps + own);
				for (uint32_t i = 0; i < fromSps + own; i++) {
					if (i < fromSps) {
						uint32_t index = 0;
						if (sps.numLongTermSps > 1) {
							index = reader.Bits(
								ceil_log2(sps.numLongTermSps));
						}
						if (index >= sps.numLongTermSps)
							return false;
						header.longTermPocLsb[i] = sps.longTermPocLsb[index];
						header.longTermUsed[i] = sps.longTermUsed[index];
					} else {
						header.longTermPocLsb[i]
							= reader.Bits(sps.log2MaxPocLsb);
						header.longTermUsed[i] = reader.Flag();
					}
					header.longTermMsbPresent[i] = reader.Flag();
					uint32_t cycle = 0;
					if (header.longTermMsbPresent[i])
						cycle = reader.UE();
					if (i != 0 && i != fromSps)
						cycle += header.longTermMsbCycle[i - 1];
					header.longTermMsbCycle[i] = cycle;
				}
			}

			if (sps.temporalMvpEnabled)
				header.temporalMvpEnabled = reader.Flag();
		}

		if (sps.saoEnabled) {
			header.saoLuma = reader.Flag();
			header.saoChroma = reader.Flag();
		}

		header.numRefIdx[0] = header.numRefIdx[1] = 0;
		header.collocatedFromL0 = true;
		if (header.type != SLICE_I) {
			header.numRefIdx[0] = pps.numRefIdxDefault[0];
			if (header.type == SLICE_B)
				header.numRefIdx[1] = pps.numRefIdxDefault[1];
			if (reader.Flag()) {
				uint32_t count = reader.UE() + 1;
				if (count > (uint32_t)kMaxReferences)
					return false;
				header.numRefIdx[0] = (uint8_t)count;
				if (header.type == SLICE_B) {
					count = reader.UE() + 1;
					if (count > (uint32_t)kMaxReferences)
						return false;
					header.numRefIdx[1] = (uint8_t)count;
				}
			}

			int total = header.NumPicTotalCurr();
			if (pps.listsModificationPresent && total > 1) {
				int bits = ceil_log2(total);
				for (int list = 0; list < (header.type == SLICE_B ? 2 : 1);
						list++) {
					header.listModification[list] = reader.Flag();
					if (!header.listModification[list])
						continue;
					for (int i = 0; i < header.numRefIdx[list]; i++) {
						header.listEntry[list][i]
							= (uint8_t)reader.Bits(bits);
					}
				}
			}

			if (header.type == SLICE_B)
				header.mvdL1Zero = reader.Flag();
			if (pps.cabacInitPresent)
				header.cabacInit = reader.Flag();

			if (header.temporalMvpEnabled) {
				if (header.type == SLICE_B)
					header.collocatedFromL0 = reader.Flag();
				if (header.numRefIdx[header.collocatedFromL0 ? 0 : 1] > 1) {
					uint32_t index = reader.UE();
					if (index >= header.numRefIdx[
							header.collocatedFromL0 ? 0 : 1]) {
						return false;
					}
					header.collocatedRefIdx = (uint8_t)index;
				}
			}

			if ((pps.weightedPred && header.type == SLICE_P)
				|| (pps.weightedBipred && header.type == SLICE_B)) {
				if (!parse_pred_weight_table(reader, header))
					return false;
			}

			uint32_t fiveMinus = reader.UE();
			if (fiveMinus > 4)
				return false;
			header.maxNumMergeCand = (uint8_t)(5 - fiveMinus);
		}

		header.qpDelta = (int8_t)reader.SE();
		if (pps.sliceChromaQpOffsetsPresent) {
			header.cbQpOffset = (int8_t)reader.SE();
			header.crQpOffset = (int8_t)reader.SE();
		}

		bool deblockingOverride = false;
		if (pps.deblockingOverrideEnabled)
			deblockingOverride = reader.Flag();
		header.deblockingDisabled = pps.deblockingDisabled;
		header.betaOffsetDiv2 = pps.betaOffsetDiv2;
		header.tcOffsetDiv2 = pps.tcOffsetDiv2;
		if (deblockingOverride) {
			header.deblockingDisabled = reader.Flag();
			if (!header.deblockingDisabled) {
				header.betaOffsetDiv2 = (int8_t)reader.SE();
				header.tcOffsetDiv2 = (int8_t)reader.SE();
			}
		}

		header.loopFilterAcrossSlices = pps.loopFilterAcrossSlices;
		if (pps.loopFilterAcrossSlices && (header.saoLuma || header.saoChroma
				|| !header.deblockingDisabled)) {
			header.loopFilterAcrossSlices = reader.Flag();
		}
	}

	header.numEntryPoints = 0;
	if (pps.tilesEnabled || pps.entropyCodingSyncEnabled) {
		header.numEntryPoints = reader.UE();
		if (header.numEntryPoints > sps.ctbWidth * sps.ctbHeight)
			return false;
		if (header.numEntryPoints > 0) {
			uint32_t length = reader.UE() + 1;
			if (length > 32)
				return false;
			reader.Skip((size_t)header.numEntryPoints * length);
		}
	}

	if (pps.sliceHeaderExtensionPresent) {
		uint32_t length = reader.UE();
		if (length > 256)
			return false;
		reader.Skip(length * 8);
	}

	// byte_alignment()
	if (!reader.Flag())
		return false;
	size_t position = (reader.Position() + 7) / 8;
	if (reader.Failed())
		return false;

	header.dataOffset = reader.OffsetInNal(position);
	return header.dataOffset < size;
}

}	// namespace hevc
