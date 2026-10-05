/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef HEVC_PARSER_H
#define HEVC_PARSER_H


#include <stddef.h>
#include <stdint.h>
#include <vector>


/*	The parts of an HEVC stream (ITU-T H.265) a decoder has to read itself
	when the hardware only does the slice data: parameter sets and slice
	segment headers. Names follow the standard's. Main and Main 10 only: no
	range extensions, one layer. */

namespace hevc {

enum {
	NAL_TRAIL_N = 0,
	NAL_RADL_N = 6,
	NAL_RASL_N = 8,
	NAL_RASL_R = 9,
	NAL_BLA_W_LP = 16,
	NAL_BLA_N_LP = 18,
	NAL_IDR_W_RADL = 19,
	NAL_IDR_N_LP = 20,
	NAL_CRA = 21,
	NAL_IRAP_LAST = 23,
	NAL_VCL_LAST = 31,
	NAL_VPS = 32,
	NAL_SPS = 33,
	NAL_PPS = 34,
	NAL_AUD = 35,
	NAL_EOS = 36,
	NAL_EOB = 37,
	NAL_PREFIX_SEI = 39,
	NAL_SUFFIX_SEI = 40
};

enum {
	SLICE_B = 0,
	SLICE_P = 1,
	SLICE_I = 2
};

static const int kMaxReferences = 16;
static const int kMaxShortTermSets = 65;
static const int kMaxTileColumns = 20;
static const int kMaxTileRows = 22;


/*!	The payload of a NAL unit with the emulation prevention bytes taken
	out, read a bit at a time. Reading past the end gives zeros and sets
	Failed(). */
class BitReader {
public:
								BitReader();

			void				SetTo(const uint8_t* nal, size_t size,
									size_t maxBytes = SIZE_MAX);

			uint32_t			Bits(int count);
			bool				Flag() { return Bits(1) != 0; }
			uint32_t			UE();
			int32_t				SE();
			void				Skip(size_t count);

			size_t				Position() const { return fPosition; }
			bool				Failed() const { return fFailed; }
			void				SetFailed() { fFailed = true; }

			/*!	Where byte \a index of the payload is in the NAL unit. */
			size_t				OffsetInNal(size_t index) const;

private:
			std::vector<uint8_t> fData;
			std::vector<uint32_t> fRemoved;
				// payload index at which a prevention byte was dropped
			size_t				fPosition;	// in bits
			bool				fFailed;
};


struct ScalingList {
	// each in raster order
	uint8_t		list4x4[6][16];
	uint8_t		list8x8[6][64];
	uint8_t		list16x16[6][64];
	uint8_t		list32x32[2][64];
	uint8_t		dc16x16[6];
	uint8_t		dc32x32[2];

	void		SetDefault();
	bool		Parse(BitReader& reader);
};


struct ShortTermSet {
	uint8_t		numNegative;
	uint8_t		numPositive;
	int32_t		deltaPocS0[kMaxReferences];
	int32_t		deltaPocS1[kMaxReferences];
	bool		usedS0[kMaxReferences];
	bool		usedS1[kMaxReferences];

	int			NumDeltaPocs() const { return numNegative + numPositive; }
};


struct SPS {
	bool		valid;
	uint8_t		id;
	uint8_t		maxSubLayersMinus1;
	uint8_t		profile;
	uint32_t	profileCompatibility;
	uint8_t		level;

	uint8_t		chromaFormat;
	bool		separateColourPlane;
	uint32_t	width;				// pic_width_in_luma_samples
	uint32_t	height;
	uint32_t	cropLeft, cropRight, cropTop, cropBottom;	// luma samples
	uint8_t		bitDepthLuma;
	uint8_t		bitDepthChroma;
	uint8_t		log2MaxPocLsb;

	uint8_t		maxDecPicBuffering;	// of the highest sub-layer
	uint8_t		maxNumReorder;
	uint32_t	maxLatencyIncreasePlus1;

	uint8_t		log2MinCbSize;
	uint8_t		log2DiffMaxMinCbSize;
	uint8_t		log2MinTbSize;
	uint8_t		log2DiffMaxMinTbSize;
	uint8_t		maxTransformHierarchyDepthInter;
	uint8_t		maxTransformHierarchyDepthIntra;

	bool		scalingListEnabled;
	bool		scalingListPresent;
	ScalingList	scalingList;

	bool		ampEnabled;
	bool		saoEnabled;
	bool		pcmEnabled;
	uint8_t		pcmBitDepthLuma;
	uint8_t		pcmBitDepthChroma;
	uint8_t		log2MinPcmCbSize;
	uint8_t		log2DiffMaxMinPcmCbSize;
	bool		pcmLoopFilterDisabled;

	uint8_t		numShortTermSets;
	ShortTermSet shortTermSets[kMaxShortTermSets];

	bool		longTermPresent;
	uint8_t		numLongTermSps;
	uint16_t	longTermPocLsb[32];
	bool		longTermUsed[32];

	bool		temporalMvpEnabled;
	bool		strongIntraSmoothing;

	// derived
	uint8_t		log2CtbSize;
	uint32_t	ctbWidth;
	uint32_t	ctbHeight;

	bool		Parse(const uint8_t* nal, size_t size);
};


struct PPS {
	bool		valid;
	uint8_t		id;
	uint8_t		spsId;

	bool		dependentSliceSegmentsEnabled;
	bool		outputFlagPresent;
	uint8_t		numExtraSliceHeaderBits;
	bool		signDataHiding;
	bool		cabacInitPresent;
	uint8_t		numRefIdxDefault[2];
	int8_t		initQpMinus26;
	bool		constrainedIntraPred;
	bool		transformSkipEnabled;
	bool		cuQpDeltaEnabled;
	uint8_t		diffCuQpDeltaDepth;
	int8_t		cbQpOffset;
	int8_t		crQpOffset;
	bool		sliceChromaQpOffsetsPresent;
	bool		weightedPred;
	bool		weightedBipred;
	bool		transquantBypassEnabled;
	bool		tilesEnabled;
	bool		entropyCodingSyncEnabled;

	uint8_t		numTileColumns;
	uint8_t		numTileRows;
	bool		uniformSpacing;
	uint16_t	columnWidth[kMaxTileColumns];	// when not uniform
	uint16_t	rowHeight[kMaxTileRows];
	bool		loopFilterAcrossTiles;

	bool		loopFilterAcrossSlices;
	bool		deblockingOverrideEnabled;
	bool		deblockingDisabled;
	int8_t		betaOffsetDiv2;
	int8_t		tcOffsetDiv2;

	bool		scalingListPresent;
	ScalingList	scalingList;

	bool		listsModificationPresent;
	uint8_t		log2ParallelMergeLevel;
	bool		sliceHeaderExtensionPresent;

	bool		Parse(const uint8_t* nal, size_t size);
};


struct PredWeightTable {
	uint8_t		lumaLog2Denom;
	uint8_t		chromaLog2Denom;
	// per list and reference; a weight of 1 << denom and offset 0 where the
	// stream has none
	int16_t		lumaWeight[2][kMaxReferences];
	int16_t		lumaOffset[2][kMaxReferences];
	int16_t		chromaWeight[2][kMaxReferences][2];
	int16_t		chromaOffset[2][kMaxReferences][2];
};


struct SliceHeader {
	uint8_t		nalType;
	uint8_t		temporalId;

	bool		firstSliceSegmentInPic;
	bool		noOutputOfPriorPics;
	uint8_t		ppsId;
	bool		dependentSliceSegment;
	uint32_t	segmentAddress;

	// The rest is of the slice the segment belongs to: a dependent segment
	// has it from the one before.
	uint8_t		type;
	bool		picOutput;
	uint32_t	pocLsb;

	bool		shortTermSetFromSps;
	uint8_t		shortTermSetIndex;
	ShortTermSet shortTermSet;		// the one in use, wherever it is from

	uint8_t		numLongTerm;
	uint32_t	longTermPocLsb[kMaxReferences];
	bool		longTermUsed[kMaxReferences];
	bool		longTermMsbPresent[kMaxReferences];
	uint32_t	longTermMsbCycle[kMaxReferences];	// accumulated

	bool		temporalMvpEnabled;
	bool		saoLuma;
	bool		saoChroma;
	uint8_t		numRefIdx[2];		// active references per list
	bool		listModification[2];
	uint8_t		listEntry[2][kMaxReferences];
	bool		mvdL1Zero;
	bool		cabacInit;
	bool		collocatedFromL0;
	uint8_t		collocatedRefIdx;
	PredWeightTable weights;
	uint8_t		maxNumMergeCand;
	int8_t		qpDelta;
	int8_t		cbQpOffset;
	int8_t		crQpOffset;
	bool		deblockingDisabled;
	int8_t		betaOffsetDiv2;
	int8_t		tcOffsetDiv2;
	bool		loopFilterAcrossSlices;

	uint32_t	numEntryPoints;

	// where the slice data begins in the NAL unit, in bytes
	size_t		dataOffset;

	/*!	References of the picture that the slice may use. */
	int			NumPicTotalCurr() const;
};


/*!	Parses the header of the slice segment in \a nal. \a previous is the
	header of the segment before in the same picture (for a dependent
	segment), \a spsList and \a ppsList the parameter sets by their ids.
	Returns false when the header cannot be read or a set is missing. */
bool parse_slice_header(const uint8_t* nal, size_t size, const SPS* spsList,
	const PPS* ppsList, const SliceHeader* previous, SliceHeader& header);

static const int kMaxSpsCount = 16;
static const int kMaxPpsCount = 64;

}	// namespace hevc

#endif	// HEVC_PARSER_H
