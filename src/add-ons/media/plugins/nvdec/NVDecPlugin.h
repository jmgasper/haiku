/* H.264 on the graphics card, offered to every program that plays video.
 *
 * Haiku picks one decoder for a format, by add-on directory, so this one takes
 * H.264 away from libavcodec wherever it is installed. It decodes what the
 * card's engine can - eight bit 4:2:0, progressive - and refuses the rest
 * rather than producing a wrong picture.
 */
#ifndef NVDEC_PLUGIN_H
#define NVDEC_PLUGIN_H

#include "DecoderPlugin.h"

extern "C" {
#include "nvdec_convert.h"
#include "nvdec_h264.h"
}


class NVDecDecoder : public Decoder {
public:
								NVDecDecoder();
	virtual						~NVDecDecoder();

	virtual	void				GetCodecInfo(media_codec_info* info);
	virtual	status_t			Setup(media_format* ioEncodedFormat,
									const void* infoBuffer, size_t infoSize);
	virtual	status_t			NegotiateOutputFormat(
									media_format* ioDecodedFormat);
	virtual	status_t			SeekedTo(int64 frame, bigtime_t time);
	virtual	status_t			Decode(void* buffer, int64* frameCount,
									media_header* mediaHeader,
									media_decode_info* info);

private:
			status_t			_ReadParameterSets(const uint8* data, size_t size);
			status_t			_AppendChunk(const uint8* data, size_t size);
			void				_Deliver(const NvdecFrame& frame, void* buffer,
									media_header* mediaHeader);

			NvdecEngine*		fEngine;
			NvdecH264*			fDecoder;

			/* A chunk from an MPEG-4 file is a run of length-prefixed units
			 * rather than one with start codes; this says how long the
			 * prefix is, or zero when the stream already has start codes. */
			int					fLengthSize;
			uint8*				fAccessUnit;
			size_t				fAccessUnitSize;
			size_t				fAccessUnitUsed;
			uint8*				fParameterSets;
			size_t				fParameterSetsSize;
			bool				fSentParameterSets;

			media_format		fInputFormat;
			int					fWidth;
			int					fHeight;
			uint32				fColorSpace;
			size_t				fRowBytes;
			bigtime_t			fFrameTime;
			bigtime_t			fLastTime;
			int64				fFrameNumber;
			NvdecColorRange		fRange;
};


class NVDecPlugin : public DecoderPlugin {
public:
	virtual	Decoder*			NewDecoder(uint index);
	virtual	status_t			GetSupportedFormats(media_format** formats,
									size_t* count);
};

#endif	// NVDEC_PLUGIN_H
