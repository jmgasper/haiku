/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	H.264 on the Raspberry Pi's hardware decoder, as a decoder add-on of the
	Media Kit. The decoder is the VideoCore firmware's (MmalDecoder).

	The add-on registers no format: a stream the firmware cannot decode
	(ten bit, larger than 1080p) would have no other decoder in the Media
	Kit's lookup. A player that knows the add-on loads it and falls back by
	itself; airTime does.

	Pictures are handed out in one of two colour spaces Haiku has no
	constant for, line_count rows of luma bytes_per_row apart and then
	- 'I420': a plane of Cb and one of Cr, half as many rows of half the
	  length (what the decoder makes), or
	- 'NV12': half as many rows of Cb and Cr in pairs. */


#include <DecoderPlugin.h>
#include <MediaFormats.h>

#include <new>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "MmalDecoder.h"


// AV_CODEC_ID_H264 of the FFmpeg add-on's format descriptions
static const uint32 kCodecH264 = 27;

static const color_space kColorSpaceNV12 = (color_space)0x4e563132;	// 'NV12'
static const color_space kColorSpaceI420 = (color_space)0x49343230;	// 'I420'


class RpiMmalDecoder : public Decoder {
public:
								RpiMmalDecoder();
	virtual						~RpiMmalDecoder();

	virtual	void				GetCodecInfo(media_codec_info* info);
	virtual	status_t			Setup(media_format* format, const void* info,
									size_t infoSize);
	virtual	status_t			NegotiateOutputFormat(media_format* format);
	virtual	status_t			SeekedTo(int64 frame, bigtime_t time);
	virtual status_t			Decode(void* buffer, int64* frameCount,
									media_header* header,
									media_decode_info* info);

private:
			status_t			_NextFrame(MmalDecoder::Frame& frame);
			status_t			_Feed();
			void				_SetParameterSets(const uint8* data,
									size_t size);
			void				_Copy(const MmalDecoder::Frame& frame,
									uint8* buffer);

			MmalDecoder			fDecoder;
			media_format		fInputFormat;
			uint32				fNalLengthSize;
				// of the stream's NAL units; 0 when they come with start
				// codes
			std::vector<uint8>	fParameterSets;
			bool				fParameterSetsSent;

			std::vector<uint8>	fAccessUnit;
			size_t				fSent;
			int64				fTime;
			bool				fKeyFrame;
			bool				fInputEnded;

			color_space			fOutputSpace;
			bool				fHaveFrame;
			MmalDecoder::Frame	fFrame;
};


RpiMmalDecoder::RpiMmalDecoder()
	:
	fNalLengthSize(0),
	fParameterSetsSent(false),
	fSent(0),
	fTime(0),
	fKeyFrame(false),
	fInputEnded(false),
	fOutputSpace(kColorSpaceI420),
	fHaveFrame(false)
{
}


RpiMmalDecoder::~RpiMmalDecoder()
{
	fDecoder.Close();
}


void
RpiMmalDecoder::GetCodecInfo(media_codec_info* info)
{
	memset(info, 0, sizeof(*info));
	strlcpy(info->pretty_name, "Raspberry Pi hardware video decoder",
		sizeof(info->pretty_name));
	strlcpy(info->short_name, "rpi_mmal h264", sizeof(info->short_name));
}


/*!	The parameter sets of an MP4 or Matroska stream ("avcC") with start
	codes; \a data that has start codes already is taken as it is.
*/
void
RpiMmalDecoder::_SetParameterSets(const uint8* data, size_t size)
{
	static const uint8 kStartCode[] = {0, 0, 0, 1};

	fParameterSets.clear();
	fNalLengthSize = 0;
	if (data == NULL || size < 4)
		return;

	if (data[0] != 1) {
		fParameterSets.assign(data, data + size);
		return;
	}
	if (size < 7)
		return;

	fNalLengthSize = (data[4] & 3) + 1;
	size_t offset = 5;
	// the sequence, then the picture parameter sets
	for (int kind = 0; kind < 2 && offset < size; kind++) {
		uint32 count = data[offset++];
		if (kind == 0)
			count &= 0x1f;
		for (uint32 i = 0; i < count && offset + 2 <= size; i++) {
			size_t length = ((size_t)data[offset] << 8) | data[offset + 1];
			offset += 2;
			if (offset + length > size)
				return;
			fParameterSets.insert(fParameterSets.end(), kStartCode,
				kStartCode + 4);
			fParameterSets.insert(fParameterSets.end(), data + offset,
				data + offset + length);
			offset += length;
		}
	}
}


status_t
RpiMmalDecoder::Setup(media_format* format, const void* info, size_t infoSize)
{
	if (format == NULL || format->type != B_MEDIA_ENCODED_VIDEO)
		return B_NOT_SUPPORTED;

	media_format_description description;
	
	if (BMediaFormats().GetCodeFor(*format, B_MISC_FORMAT_FAMILY,
			&description) != B_OK
		|| description.u.misc.file_format != 'ffmp'
		|| description.u.misc.codec != kCodecH264) {
		return B_NOT_SUPPORTED;
	}

	const media_video_display_info& display
		= format->u.encoded_video.output.display;
	if (display.line_width == 0 || display.line_count == 0
		|| display.line_width > 1920 || display.line_count > 1920
		|| display.line_width * display.line_count > 1920 * 1088) {
		return B_NOT_SUPPORTED;
	}

	fInputFormat = *format;
	_SetParameterSets((const uint8*)info, infoSize);

	BString error;
	status_t status = fDecoder.Open(display.line_width, display.line_count,
		true, &error);
	if (status != B_OK) {
		fprintf(stderr, "rpi_mmal: %s\n", error.String());
		return status;
	}
	return B_OK;
}


status_t
RpiMmalDecoder::NegotiateOutputFormat(media_format* format)
{
	if (format == NULL || format->type != B_MEDIA_RAW_VIDEO
		|| (format->u.raw_video.display.format != kColorSpaceNV12
			&& format->u.raw_video.display.format != kColorSpaceI420)) {
		return B_MEDIA_BAD_FORMAT;
	}
	fOutputSpace = format->u.raw_video.display.format;

	// The first picture tells for sure how large they are.
	if (!fHaveFrame) {
		status_t status = _NextFrame(fFrame);
		if (status != B_OK)
			return status;
		fHaveFrame = true;
	}

	media_raw_video_format raw = fInputFormat.u.encoded_video.output;
	raw.interlace = 1;
	raw.first_active = 0;
	raw.last_active = fDecoder.Height() - 1;
	raw.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	raw.display.format = fOutputSpace;
	raw.display.line_width = fDecoder.Width();
	raw.display.line_count = fDecoder.Height();
	raw.display.bytes_per_row = fDecoder.Stride();
	raw.display.pixel_offset = 0;
	raw.display.line_offset = 0;
	raw.display.flags = 0;

	format->type = B_MEDIA_RAW_VIDEO;
	format->require_flags = 0;
	format->deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;
	format->u.raw_video = raw;
	return B_OK;
}


status_t
RpiMmalDecoder::SeekedTo(int64 frame, bigtime_t time)
{
	if (fHaveFrame) {
		fDecoder.ReleaseFrame(fFrame);
		fHaveFrame = false;
	}
	fAccessUnit.clear();
	fSent = 0;
	fInputEnded = false;
	fParameterSetsSent = false;
	return fDecoder.Flush();
}


/*!	Gives the firmware the next piece of the stream if it takes one. */
status_t
RpiMmalDecoder::_Feed()
{
	if (fInputEnded || !fDecoder.CanSend())
		return B_WOULD_BLOCK;

	if (fSent >= fAccessUnit.size()) {
		const void* chunk;
		size_t size;
		media_header header;
		status_t status = GetNextChunk(&chunk, &size, &header);
		if (status == B_LAST_BUFFER_ERROR) {
			fInputEnded = true;
			return fDecoder.SendEndOfStream();
		}
		if (status != B_OK)
			return status;

		fAccessUnit.clear();
		fSent = 0;
		fTime = header.start_time;
		fKeyFrame = (header.u.encoded_video.field_flags & B_MEDIA_KEY_FRAME)
			!= 0;

		// after the start and a seek the decoder needs the parameter sets
		if (!fParameterSetsSent) {
			fAccessUnit = fParameterSets;
			fParameterSetsSent = true;
		}

		const uint8* data = (const uint8*)chunk;
		if (fNalLengthSize == 0)
			fAccessUnit.insert(fAccessUnit.end(), data, data + size);
		else {
			// a start code in place of each NAL unit's length
			static const uint8 kStartCode[] = {0, 0, 0, 1};
			size_t offset = 0;
			while (offset + fNalLengthSize <= size) {
				size_t length = 0;
				for (uint32 i = 0; i < fNalLengthSize; i++)
					length = (length << 8) | data[offset++];
				if (length > size - offset)
					length = size - offset;
				fAccessUnit.insert(fAccessUnit.end(), kStartCode,
					kStartCode + 4);
				fAccessUnit.insert(fAccessUnit.end(), data + offset,
					data + offset + length);
				offset += length;
			}
		}
		if (fAccessUnit.empty())
			return B_OK;
	}

	size_t size = min_c(fAccessUnit.size() - fSent,
		(size_t)fDecoder.InputSize());
	uint32 flags = 0;
	if (fSent == 0)
		flags |= MMAL_BUFFER_FLAG_FRAME_START;
	if (fSent + size == fAccessUnit.size())
		flags |= MMAL_BUFFER_FLAG_FRAME_END;
	if (fKeyFrame)
		flags |= MMAL_BUFFER_FLAG_KEYFRAME;

	status_t status = fDecoder.Send(&fAccessUnit[fSent], size, fTime, flags);
	if (status == B_OK)
		fSent += size;
	return status;
}


status_t
RpiMmalDecoder::_NextFrame(MmalDecoder::Frame& frame)
{
	while (true) {
		status_t status = fDecoder.NextFrame(frame, 0);
		if (status != B_TIMED_OUT) {
			if (status != B_OK && status != B_LAST_BUFFER_ERROR) {
				fprintf(stderr, "rpi_mmal: %s\n", fDecoder.Error());
				return B_ERROR;
			}
			return status;
		}

		status = _Feed();
		if (status == B_WOULD_BLOCK)
			fDecoder.Wait(20000);
		else if (status != B_OK)
			return status;
	}
}


void
RpiMmalDecoder::_Copy(const MmalDecoder::Frame& frame, uint8* buffer)
{
	uint32 stride = fDecoder.Stride();
	uint32 half = stride / 2;
	uint32 height = fDecoder.Height();
	uint32 chromaRows = (height + 1) / 2;
	const uint8* luma = frame.data;
	const uint8* chroma = luma + (size_t)stride * fDecoder.SliceHeight();
	bool sourcePlanar = fDecoder.Encoding() == MMAL_ENCODING_I420;
	// the second chroma plane of I420
	const uint8* cr = chroma + (size_t)half * (fDecoder.SliceHeight() / 2);

	memcpy(buffer, luma, (size_t)stride * height);
	buffer += (size_t)stride * height;

	if (fOutputSpace == kColorSpaceI420) {
		if (sourcePlanar) {
			memcpy(buffer, chroma, (size_t)half * chromaRows);
			memcpy(buffer + (size_t)half * chromaRows, cr,
				(size_t)half * chromaRows);
			return;
		}
		uint8* cbOut = buffer;
		uint8* crOut = buffer + (size_t)half * chromaRows;
		for (uint32 y = 0; y < chromaRows; y++) {
			for (uint32 x = 0; x < half; x++) {
				cbOut[x] = chroma[2 * x];
				crOut[x] = chroma[2 * x + 1];
			}
			chroma += stride;
			cbOut += half;
			crOut += half;
		}
		return;
	}

	if (!sourcePlanar) {
		memcpy(buffer, chroma, (size_t)stride * chromaRows);
		return;
	}
	const uint8* cb = chroma;
	for (uint32 y = 0; y < chromaRows; y++) {
		for (uint32 x = 0; x < half; x++) {
			buffer[2 * x] = cb[x];
			buffer[2 * x + 1] = cr[x];
		}
		buffer += stride;
		cb += half;
		cr += half;
	}
}


status_t
RpiMmalDecoder::Decode(void* buffer, int64* frameCount, media_header* header,
	media_decode_info* info)
{
	if (buffer == NULL || frameCount == NULL || header == NULL)
		return B_BAD_VALUE;

	if (!fHaveFrame) {
		status_t status = _NextFrame(fFrame);
		if (status != B_OK)
			return status;
	}
	fHaveFrame = false;

	_Copy(fFrame, (uint8*)buffer);

	memset(header, 0, sizeof(*header));
	header->type = B_MEDIA_RAW_VIDEO;
	header->start_time = fFrame.pts == MMAL_TIME_UNKNOWN ? 0 : fFrame.pts;
	header->size_used = (size_t)fDecoder.Stride()
		* (fDecoder.Height() + (fDecoder.Height() + 1) / 2);
	header->u.raw_video.display_line_width = fDecoder.Width();
	header->u.raw_video.display_line_count = fDecoder.Height();
	header->u.raw_video.bytes_per_row = fDecoder.Stride();
	header->u.raw_video.line_count = fDecoder.Height();
	*frameCount = 1;

	fDecoder.ReleaseFrame(fFrame);
	return B_OK;
}


//	#pragma mark -


class RpiMmalPlugin : public DecoderPlugin {
public:
	virtual	Decoder*			NewDecoder(uint index);
	virtual	status_t			GetSupportedFormats(media_format** formats,
									size_t* count);
};


Decoder*
RpiMmalPlugin::NewDecoder(uint index)
{
	return new(std::nothrow) RpiMmalDecoder;
}


status_t
RpiMmalPlugin::GetSupportedFormats(media_format** formats, size_t* count)
{
	// none for the Media Kit's lookup, see above
	static media_format sNone;
	*formats = &sNone;
	*count = 0;
	return B_OK;
}


MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) RpiMmalPlugin;
}
