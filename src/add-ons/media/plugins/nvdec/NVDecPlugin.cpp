/* See NVDecPlugin.h. */

#include "NVDecPlugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <MediaFormats.h>

/* libavcodec's number for H.264. The reader describes every encoded stream
 * with it, so this is how a format is recognised without linking to it. */
#define CODEC_ID_H264	27

#define ACCESS_UNIT_START	(1 << 20)


NVDecDecoder::NVDecDecoder()
	:
	fEngine(NULL),
	fDecoder(NULL),
	fLengthSize(0),
	fAccessUnit(NULL),
	fAccessUnitSize(0),
	fAccessUnitUsed(0),
	fParameterSets(NULL),
	fParameterSetsSize(0),
	fSentParameterSets(false),
	fWidth(0),
	fHeight(0),
	fColorSpace(B_RGB32),
	fRowBytes(0),
	fFrameTime(0),
	fLastTime(0),
	fFrameNumber(0),
	fRange(NVDEC_RANGE_BT709)
{
}


NVDecDecoder::~NVDecDecoder()
{
	if (fDecoder != NULL)
		nvdecH264Destroy(fDecoder);
	if (fEngine != NULL)
		nvdecClose(fEngine);
	free(fAccessUnit);
	free(fParameterSets);
}


void
NVDecDecoder::GetCodecInfo(media_codec_info* info)
{
	strlcpy(info->short_name, "nvdec h264", sizeof(info->short_name));
	strlcpy(info->pretty_name, "H.264 on the graphics card (NVDEC)",
		sizeof(info->pretty_name));
}


/* The parameter sets of an MPEG-4 file arrive as an AVCDecoderConfiguration
 * record rather than as start codes. Turn them into a piece of stream the
 * decoder can be handed. */
status_t
NVDecDecoder::_ReadParameterSets(const uint8* data, size_t size)
{
	if (size == 0)
		return B_OK;

	if (data[0] != 1 || size < 7) {
		/* Already a piece of stream with start codes. */
		fLengthSize = 0;
		fParameterSets = (uint8*)malloc(size);
		if (fParameterSets == NULL)
			return B_NO_MEMORY;
		memcpy(fParameterSets, data, size);
		fParameterSetsSize = size;
		return B_OK;
	}

	fLengthSize = (data[4] & 0x3) + 1;
	size_t capacity = size + 64;
	fParameterSets = (uint8*)malloc(capacity);
	if (fParameterSets == NULL)
		return B_NO_MEMORY;
	fParameterSetsSize = 0;

	size_t at = 5;
	for (int round = 0; round < 2 && at < size; round++) {
		int count = (round == 0) ? (data[at] & 0x1f) : data[at];
		at++;
		for (int i = 0; i < count && at + 2 <= size; i++) {
			size_t length = ((size_t)data[at] << 8) | data[at + 1];
			at += 2;
			if (at + length > size || fParameterSetsSize + length + 4 > capacity)
				break;
			fParameterSets[fParameterSetsSize++] = 0;
			fParameterSets[fParameterSetsSize++] = 0;
			fParameterSets[fParameterSetsSize++] = 0;
			fParameterSets[fParameterSetsSize++] = 1;
			memcpy(fParameterSets + fParameterSetsSize, data + at, length);
			fParameterSetsSize += length;
			at += length;
		}
	}
	return B_OK;
}


status_t
NVDecDecoder::Setup(media_format* ioEncodedFormat, const void* infoBuffer,
	size_t infoSize)
{
	if (ioEncodedFormat->type != B_MEDIA_ENCODED_VIDEO)
		return B_ERROR;

	media_format_description description;
	BMediaFormats formats;
	if (formats.GetCodeFor(*ioEncodedFormat, B_MISC_FORMAT_FAMILY,
			&description) != B_OK) {
		return B_ERROR;
	}
	if (description.u.misc.codec != CODEC_ID_H264)
		return B_ERROR;

	char reason[256] = "";
	fEngine = nvdecOpen(reason, sizeof(reason));
	if (fEngine == NULL) {
		fprintf(stderr, "nvdec: the card's decoder is not available: %s\n", reason);
		return B_ERROR;
	}
	fDecoder = nvdecH264Create(fEngine, reason, sizeof(reason));
	if (fDecoder == NULL) {
		nvdecClose(fEngine);
		fEngine = NULL;
		return B_NO_MEMORY;
	}

	if (_ReadParameterSets((const uint8*)infoBuffer, infoSize) != B_OK)
		return B_NO_MEMORY;

	fInputFormat = *ioEncodedFormat;
	const media_video_display_info& display
		= ioEncodedFormat->u.encoded_video.output.display;
	fWidth = display.line_width;
	fHeight = display.line_count;
	float rate = ioEncodedFormat->u.encoded_video.output.field_rate;
	fFrameTime = rate > 0 ? (bigtime_t)(1000000 / rate) : 0;
	fRange = nvdecRangeForHeight(fHeight > 0 ? fHeight : 720);
	return B_OK;
}


status_t
NVDecDecoder::NegotiateOutputFormat(media_format* ioDecodedFormat)
{
	media_format format;
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = fInputFormat.u.encoded_video.output;
	format.u.raw_video.interlace = 1;
	format.u.raw_video.first_active = 0;
	format.u.raw_video.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	format.u.raw_video.pixel_width_aspect
		= fInputFormat.u.encoded_video.output.pixel_width_aspect;
	format.u.raw_video.pixel_height_aspect
		= fInputFormat.u.encoded_video.output.pixel_height_aspect;

	/* Anything that asks for luma and chroma gets it without a conversion;
	 * everything else gets pixels. */
	uint32 wanted = ioDecodedFormat->u.raw_video.display.format;
	if (wanted == B_YCbCr422)
		fColorSpace = B_YCbCr422;
	else
		fColorSpace = B_RGB32;

	format.u.raw_video.display.format = (color_space)fColorSpace;
	format.u.raw_video.display.line_width = fWidth;
	format.u.raw_video.display.line_count = fHeight;
	format.u.raw_video.last_active = fHeight - 1;
	fRowBytes = (size_t)fWidth * (fColorSpace == B_YCbCr422 ? 2 : 4);
	format.u.raw_video.display.bytes_per_row = fRowBytes;

	*ioDecodedFormat = format;
	return B_OK;
}


status_t
NVDecDecoder::SeekedTo(int64 frame, bigtime_t time)
{
	if (fDecoder != NULL)
		nvdecH264Reset(fDecoder);
	fAccessUnitUsed = 0;
	fSentParameterSets = false;
	fFrameNumber = frame;
	fLastTime = time;
	return B_OK;
}


/* Collect one chunk into the access unit being built, turning length prefixes
 * into start codes if the file uses them. */
status_t
NVDecDecoder::_AppendChunk(const uint8* data, size_t size)
{
	/* A start code is four bytes where a length prefix may be fewer, so the
	 * access unit can come out longer than the chunk went in. */
	size_t needed = fAccessUnitUsed + size * 2 + 64;
	if (needed > fAccessUnitSize) {
		size_t grown = needed * 2;
		uint8* buffer = (uint8*)realloc(fAccessUnit, grown);
		if (buffer == NULL)
			return B_NO_MEMORY;
		fAccessUnit = buffer;
		fAccessUnitSize = grown;
	}
	if (fLengthSize == 0) {
		memcpy(fAccessUnit + fAccessUnitUsed, data, size);
		fAccessUnitUsed += size;
		return B_OK;
	}
	size_t at = 0;
	while (at + (size_t)fLengthSize <= size) {
		size_t length = 0;
		for (int i = 0; i < fLengthSize; i++)
			length = (length << 8) | data[at + i];
		at += fLengthSize;
		if (length == 0 || at + length > size)
			break;
		uint8* to = fAccessUnit + fAccessUnitUsed;
		to[0] = 0; to[1] = 0; to[2] = 0; to[3] = 1;
		memcpy(to + 4, data + at, length);
		fAccessUnitUsed += 4 + length;
		at += length;
	}
	return B_OK;
}


void
NVDecDecoder::_Deliver(const NvdecFrame& frame, void* buffer,
	media_header* mediaHeader)
{
	/* What the file said the picture is need not be what the stream says,
	 * and the buffer was made from what the file said. */
	NvdecFrame fitted = frame;
	if (fitted.width > fWidth)
		fitted.width = fWidth;
	if (fitted.height > fHeight)
		fitted.height = fHeight;
	const NvdecFrame& frameRef = fitted;
	if (fColorSpace == B_YCbCr422)
		nvdecFrameToYCbCr422Threaded(&frameRef, (uint8*)buffer, fRowBytes);
	else
		nvdecFrameToRGB32Threaded(&frameRef, (uint8*)buffer, fRowBytes, fRange);

	mediaHeader->type = B_MEDIA_RAW_VIDEO;
	mediaHeader->start_time = frame.time != 0 ? frame.time : fLastTime;
	mediaHeader->file_pos = 0;
	mediaHeader->orig_size = 0;
	mediaHeader->size_used = fRowBytes * fHeight;
	mediaHeader->u.raw_video.display_line_width = fWidth;
	mediaHeader->u.raw_video.display_line_count = fHeight;
	mediaHeader->u.raw_video.bytes_per_row = fRowBytes;
	mediaHeader->u.raw_video.field_number = 0;
	mediaHeader->u.raw_video.pulldown_number = 0;
	mediaHeader->u.raw_video.first_active_line = 0;
	mediaHeader->u.raw_video.line_count = fHeight;
	fLastTime = mediaHeader->start_time + fFrameTime;
}


status_t
NVDecDecoder::Decode(void* buffer, int64* frameCount, media_header* mediaHeader,
	media_decode_info* info)
{
	if (fDecoder == NULL)
		return B_NO_INIT;

	if (!fSentParameterSets && fParameterSetsSize > 0) {
		nvdecH264Decode(fDecoder, fParameterSets, fParameterSetsSize, 0);
		fSentParameterSets = true;
	}

	NvdecFrame frame;
	for (;;) {
		if (nvdecH264NextFrame(fDecoder, &frame)) {
			_Deliver(frame, buffer, mediaHeader);
			nvdecH264ReleaseFrame(fDecoder, &frame);
			*frameCount = 1;
			fFrameNumber++;
			return B_OK;
		}

		const void* chunk = NULL;
		size_t chunkSize = 0;
		media_header chunkHeader;
		status_t status = GetNextChunk(&chunk, &chunkSize, &chunkHeader);
		if (status != B_OK) {
			/* The file has ended: let go of everything still held. */
			nvdecH264DrainAll(fDecoder);
			if (nvdecH264NextFrame(fDecoder, &frame)) {
				_Deliver(frame, buffer, mediaHeader);
				nvdecH264ReleaseFrame(fDecoder, &frame);
				*frameCount = 1;
				fFrameNumber++;
				return B_OK;
			}
			return status;
		}

		fAccessUnitUsed = 0;
		if (_AppendChunk((const uint8*)chunk, chunkSize) != B_OK)
			return B_NO_MEMORY;
		if (fAccessUnitUsed == 0)
			continue;
		if (!nvdecH264Decode(fDecoder, fAccessUnit, fAccessUnitUsed,
				chunkHeader.start_time)) {
			fprintf(stderr, "nvdec: %s\n", nvdecH264LastError(fDecoder));
			/* One bad picture should not end the film. */
			continue;
		}
	}
}


Decoder*
NVDecPlugin::NewDecoder(uint index)
{
	return new(std::nothrow) NVDecDecoder();
}


status_t
NVDecPlugin::GetSupportedFormats(media_format** formats, size_t* count)
{
	static media_format sFormats[1];

	media_format_description description;
	memset(&description, 0, sizeof(description));
	description.family = B_MISC_FORMAT_FAMILY;
	description.u.misc.file_format = 'ffmp';
	description.u.misc.codec = CODEC_ID_H264;

	media_format format;
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_ENCODED_VIDEO;
	format.require_flags = 0;
	format.deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;

	BMediaFormats mediaFormats;
	if (mediaFormats.InitCheck() != B_OK)
		return B_ERROR;
	if (mediaFormats.MakeFormatFor(&description, 1, &format) != B_OK)
		return B_ERROR;

	sFormats[0] = format;
	*formats = sFormats;
	*count = 1;
	return B_OK;
}


MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) NVDecPlugin();
}
