/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */

#include <DecoderPlugin.h>
#include <MediaFormats.h>

#include <algorithm>
#include <new>
#include <vector>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <rk_mpi.h>
}


class RockchipMppDecoder : public Decoder {
public:
	RockchipMppDecoder() = default;

	~RockchipMppDecoder() override
	{
		if (fSws != NULL)
			sws_freeContext(fSws);
		if (fBsf != NULL)
			av_bsf_free(&fBsf);
		_DestroyContext();
	}

	void GetCodecInfo(media_codec_info* info) override
	{
		memset(info, 0, sizeof(*info));
		strlcpy(info->pretty_name, "Rockchip RK3588 hardware video decoder",
			sizeof(info->pretty_name));
		strlcpy(info->short_name, "rk3588_mpp", sizeof(info->short_name));
	}

	status_t Setup(media_format* format, const void* info, size_t infoSize) override
	{
		if (format == NULL || format->type != B_MEDIA_ENCODED_VIDEO)
			return B_NOT_SUPPORTED;
		media_format_description description = {};
		if (BMediaFormats().GetCodeFor(*format, B_MISC_FORMAT_FAMILY,
				&description) != B_OK
			|| description.u.misc.file_format != 'ffmp') {
			return B_NOT_SUPPORTED;
		}

		const char* filterName = NULL;
		switch (description.u.misc.codec) {
			case AV_CODEC_ID_H264:
				fCoding = MPP_VIDEO_CodingAVC;
				filterName = "h264_mp4toannexb";
				break;
			case AV_CODEC_ID_HEVC:
				fCoding = MPP_VIDEO_CodingHEVC;
				filterName = "hevc_mp4toannexb";
				break;
			case AV_CODEC_ID_AV1:
				fCoding = MPP_VIDEO_CodingAV1;
				break;
			default:
				return B_NOT_SUPPORTED;
		}
		fCodecId = (AVCodecID)description.u.misc.codec;
		fInputFormat = *format;
		fprintf(stderr, "RockchipMppDecoder: codec=%d extra=%zu\n",
			(int)fCodecId, infoSize);

		if (filterName != NULL) {
			const AVBitStreamFilter* filter = av_bsf_get_by_name(filterName);
			if (filter == NULL || av_bsf_alloc(filter, &fBsf) < 0)
				return B_ERROR;
			fBsf->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
			fBsf->par_in->codec_id = fCodecId;
			fBsf->par_in->width = format->u.encoded_video.output.display.line_width;
			fBsf->par_in->height = format->u.encoded_video.output.display.line_count;
			if (info != NULL && infoSize > 0) {
				fBsf->par_in->extradata = (uint8_t*)av_mallocz(infoSize
					+ AV_INPUT_BUFFER_PADDING_SIZE);
				if (fBsf->par_in->extradata == NULL)
					return B_NO_MEMORY;
				memcpy(fBsf->par_in->extradata, info, infoSize);
				fBsf->par_in->extradata_size = infoSize;
			}
			fBsf->time_base_in = AVRational{1, 1000000};
			if (av_bsf_init(fBsf) < 0)
				return B_ERROR;
		}

		return _CreateContext();
	}

	status_t NegotiateOutputFormat(media_format* format) override
	{
		if (format == NULL || (format->type != B_MEDIA_RAW_VIDEO
				&& format->type != B_MEDIA_UNKNOWN_TYPE
				&& format->type != B_MEDIA_NO_TYPE)) {
			return B_MEDIA_BAD_FORMAT;
		}
		if (format->type == B_MEDIA_RAW_VIDEO
			&& format->u.raw_video.display.format != B_NO_COLOR_SPACE
			&& format->u.raw_video.display.format != B_RGB32) {
			return B_MEDIA_BAD_FORMAT;
		}
		status_t status = _ReadFrame(fCachedFrame, fCachedHeader);
		if (status != B_OK)
			return status;

		media_raw_video_format raw = fInputFormat.u.encoded_video.output;
		raw.interlace = 1;
		raw.first_active = 0;
		raw.last_active = fHeight - 1;
		raw.display.format = B_RGB32;
		raw.display.line_width = fWidth;
		raw.display.line_count = fHeight;
		raw.display.bytes_per_row = fWidth * 4;
		format->type = B_MEDIA_RAW_VIDEO;
		format->require_flags = 0;
		format->deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;
		format->u.raw_video = raw;
		return B_OK;
	}

	status_t SeekedTo(int64, bigtime_t) override
	{
		_DestroyContext();
		if (_CreateContext() != B_OK)
			return B_ERROR;
		if (fBsf != NULL)
			av_bsf_flush(fBsf);
		fCachedFrame.clear();
		fEndOfInput = false;
		fEosSent = false;
		fSubmittedPackets = 0;
		fPollsAfterSubmit = 30;
		return B_OK;
	}

	status_t Decode(void* buffer, int64* frameCount, media_header* header,
		media_decode_info*) override
	{
		if (buffer == NULL || frameCount == NULL || header == NULL)
			return B_BAD_VALUE;
		std::vector<uint8_t> frame;
		media_header decodedHeader = {};
		if (!fCachedFrame.empty()) {
			frame.swap(fCachedFrame);
			decodedHeader = fCachedHeader;
		} else {
			status_t status = _ReadFrame(frame, decodedHeader);
			if (status != B_OK)
				return status;
		}
		memcpy(buffer, frame.data(), frame.size());
		*frameCount = 1;
		*header = decodedHeader;
		return B_OK;
	}

private:
	void _DestroyContext()
	{
		if (fContext != NULL)
			mpp_destroy(fContext);
		fContext = NULL;
		fApi = NULL;
		if (fFrameGroup != NULL)
			mpp_buffer_group_put(fFrameGroup);
		fFrameGroup = NULL;
	}

	status_t _CreateContext()
	{
		if (mpp_create(&fContext, &fApi) != MPP_OK
			|| mpp_init(fContext, MPP_CTX_DEC, fCoding) != MPP_OK
			|| _ConfigureContext() != B_OK) {
			_DestroyContext();
			return B_ERROR;
		}
		return B_OK;
	}

	status_t _ConfigureContext()
	{
		RK_U32 split = 1;
		RK_S64 inputTimeout = 0;
		MppFrameFormat outputFormat = MPP_FMT_YUV420SP;
		return fApi->control(fContext, MPP_SET_INPUT_TIMEOUT, &inputTimeout)
				== MPP_OK
			&& fApi->control(fContext, MPP_DEC_SET_PARSER_SPLIT_MODE, &split)
				== MPP_OK
			&& fApi->control(fContext, MPP_DEC_SET_OUTPUT_FORMAT, &outputFormat)
				== MPP_OK
			? B_OK : B_ERROR;
	}

	status_t _PreparePacket(const void* chunk, size_t size, bigtime_t startTime,
		std::vector<uint8_t>& bytes)
	{
		if (fBsf == NULL) {
			if (fCoding == MPP_VIDEO_CodingAV1)
				bytes.assign({0x12, 0x00});
			const uint8_t* source = static_cast<const uint8_t*>(chunk);
			bytes.insert(bytes.end(), source, source + size);
			return B_OK;
		}
		AVPacket* input = av_packet_alloc();
		AVPacket* output = av_packet_alloc();
		if (input == NULL || output == NULL) {
			av_packet_free(&input);
			av_packet_free(&output);
			return B_NO_MEMORY;
		}
		status_t status = B_ERROR;
		if (av_new_packet(input, size) == 0) {
			memcpy(input->data, chunk, size);
			input->pts = input->dts = startTime;
			if (av_bsf_send_packet(fBsf, input) == 0
				&& av_bsf_receive_packet(fBsf, output) == 0) {
				bytes.assign(output->data, output->data + output->size);
				status = B_OK;
			}
		}
		av_packet_free(&input);
		av_packet_free(&output);
		return status;
	}

	status_t _Submit(const void* chunk, size_t size, bigtime_t startTime,
		bool eos)
	{
		std::vector<uint8_t> bytes;
		if (size > 0) {
			status_t status = _PreparePacket(chunk, size, startTime, bytes);
			if (status != B_OK)
				return status;
		}
		MppPacket packet = NULL;
		MPP_RET result = mpp_packet_init(&packet,
			bytes.empty() ? NULL : bytes.data(), bytes.size());
		if (result == MPP_OK) {
			mpp_packet_set_pts(packet, startTime);
			if (eos)
				mpp_packet_set_eos(packet);
			for (int attempt = 0; attempt < 10; attempt++) {
				result = fApi->decode_put_packet(fContext, packet);
				if (result == MPP_OK)
					break;
				snooze(1000);
			}
		}
		if (packet != NULL)
			mpp_packet_deinit(&packet);
		if (fSubmittedPackets < 4 || result != MPP_OK) {
			fprintf(stderr, "RockchipMppDecoder: packet=%u input=%zu stream=%zu"
				" pts=%lld result=%d\n", fSubmittedPackets, size, bytes.size(),
				(long long)startTime, result);
		}
		fSubmittedPackets++;
		if (result == MPP_OK)
			fPollsAfterSubmit = 0;
		return result == MPP_OK ? B_OK : B_ERROR;
	}

	status_t _ConvertFrame(MppFrame frame, std::vector<uint8_t>& output,
		media_header& header)
	{
		MppBuffer buffer = mpp_frame_get_buffer(frame);
		uint8_t* pixels = buffer != NULL
			? static_cast<uint8_t*>(mpp_buffer_get_ptr(buffer)) : NULL;
		int width = mpp_frame_get_width(frame);
		int height = mpp_frame_get_height(frame);
		int horizontal = mpp_frame_get_hor_stride(frame);
		int vertical = mpp_frame_get_ver_stride(frame);
		MppFrameFormat format = mpp_frame_get_fmt(frame);
		if (pixels == NULL || width <= 0 || height <= 0 || horizontal < width
			|| vertical < height || mpp_frame_get_errinfo(frame) != 0
			|| mpp_frame_get_discard(frame) != 0) {
			return B_ERROR;
		}
		AVPixelFormat sourceFormat;
		const uint8_t* planes[4] = {pixels, NULL, NULL, NULL};
		int strides[4] = {horizontal, 0, 0, 0};
		switch (format & MPP_FRAME_FMT_MASK) {
			case MPP_FMT_YUV420SP:
				sourceFormat = AV_PIX_FMT_NV12;
				planes[1] = pixels + (size_t)horizontal * vertical;
				strides[1] = horizontal;
				break;
			case MPP_FMT_YUV420SP_VU:
				sourceFormat = AV_PIX_FMT_NV21;
				planes[1] = pixels + (size_t)horizontal * vertical;
				strides[1] = horizontal;
				break;
			case MPP_FMT_YUV420P:
				sourceFormat = AV_PIX_FMT_YUV420P;
				planes[1] = pixels + (size_t)horizontal * vertical;
				planes[2] = planes[1] + (size_t)horizontal * vertical / 4;
				strides[1] = strides[2] = horizontal / 2;
				break;
			default:
				return B_NOT_SUPPORTED;
		}
		fSws = sws_getCachedContext(fSws, width, height, sourceFormat,
			width, height, AV_PIX_FMT_RGB32, SWS_FAST_BILINEAR, NULL, NULL, NULL);
		if (fSws == NULL)
			return B_ERROR;
		output.resize((size_t)width * height * 4);
		uint8_t* destination[4] = {output.data(), NULL, NULL, NULL};
		int destinationStride[4] = {width * 4, 0, 0, 0};
		if (sws_scale(fSws, planes, strides, 0, height, destination,
				destinationStride) != height) {
			return B_ERROR;
		}
		fWidth = width;
		fHeight = height;
		if (fDecodedFrames < 4) {
			fprintf(stderr, "RockchipMppDecoder: frame=%u %dx%d stride=%dx%d"
				" format=%#x pts=%lld\n", fDecodedFrames, width, height,
				horizontal, vertical, format, (long long)mpp_frame_get_pts(frame));
		}
		fDecodedFrames++;
		memset(&header, 0, sizeof(header));
		header.type = B_MEDIA_RAW_VIDEO;
		RK_S64 pts = mpp_frame_get_pts(frame);
		header.start_time = pts >= 0 ? pts : fLastInputTime;
		header.size_used = output.size();
		header.file_pos = -1;
		header.u.raw_video.field_gamma = 1.0f;
		header.u.raw_video.display_line_width = width;
		header.u.raw_video.display_line_count = height;
		header.u.raw_video.bytes_per_row = width * 4;
		return B_OK;
	}

	status_t _ReadFrame(std::vector<uint8_t>& output, media_header& header)
	{
		for (int step = 0; step < 20000; step++) {
			MppFrame frame = NULL;
			MPP_RET result = fApi->decode_get_frame(fContext, &frame);
			if (result != MPP_OK)
				return B_ERROR;
			if (frame != NULL) {
				if (mpp_frame_get_info_change(frame)) {
					fprintf(stderr, "RockchipMppDecoder: info-change %ux%u"
						" stride=%ux%u buffer=%zu\n", mpp_frame_get_width(frame),
						mpp_frame_get_height(frame), mpp_frame_get_hor_stride(frame),
						mpp_frame_get_ver_stride(frame), mpp_frame_get_buf_size(frame));
					if (fFrameGroup == NULL
						&& mpp_buffer_group_get_internal(&fFrameGroup,
							MPP_BUFFER_TYPE_DMA_HEAP) != MPP_OK) {
						mpp_frame_deinit(&frame);
						return B_ERROR;
					}
					result = fApi->control(fContext, MPP_DEC_SET_EXT_BUF_GROUP,
						fFrameGroup);
					if (result == MPP_OK)
						result = fApi->control(fContext,
							MPP_DEC_SET_INFO_CHANGE_READY, NULL);
					mpp_frame_deinit(&frame);
					if (result != MPP_OK)
						return B_ERROR;
					fPollsAfterSubmit = 0;
					continue;
				}
				bool eos = mpp_frame_get_eos(frame);
				status_t status = _ConvertFrame(frame, output, header);
				mpp_frame_deinit(&frame);
				if (status == B_OK)
					return B_OK;
				if (eos)
					return B_LAST_BUFFER_ERROR;
				return status;
			}
			if (fSubmittedPackets > 0 && fPollsAfterSubmit < 30) {
				fPollsAfterSubmit++;
				snooze(1000);
				continue;
			}

			if (!fEndOfInput) {
				const void* chunk = NULL;
				size_t size = 0;
				media_header chunkHeader = {};
				status_t status = GetNextChunk(&chunk, &size, &chunkHeader);
				if (status == B_OK) {
					fLastInputTime = chunkHeader.start_time;
					status = _Submit(chunk, size, fLastInputTime, false);
					if (status != B_OK)
						return status;
					continue;
				}
				if (status != B_LAST_BUFFER_ERROR)
					return status;
				fEndOfInput = true;
			}
			if (!fEosSent) {
				status_t status = _Submit(NULL, 0, fLastInputTime, true);
				if (status != B_OK)
					return status;
				fEosSent = true;
				continue;
			}
			snooze(1000);
		}
		return fEndOfInput ? B_LAST_BUFFER_ERROR : B_TIMED_OUT;
	}

	media_format fInputFormat = {};
	AVCodecID fCodecId = AV_CODEC_ID_NONE;
	MppCodingType fCoding = MPP_VIDEO_CodingUnused;
	MppCtx fContext = NULL;
	MppApi* fApi = NULL;
	MppBufferGroup fFrameGroup = NULL;
	AVBSFContext* fBsf = NULL;
	SwsContext* fSws = NULL;
	std::vector<uint8_t> fCachedFrame;
	media_header fCachedHeader = {};
	bigtime_t fLastInputTime = 0;
	int fWidth = 0;
	int fHeight = 0;
	bool fEndOfInput = false;
	bool fEosSent = false;
	uint32 fSubmittedPackets = 0;
	uint32 fDecodedFrames = 0;
	uint32 fPollsAfterSubmit = 30;
};


class RockchipMppPlugin : public DecoderPlugin {
public:
	Decoder* NewDecoder(uint) override
	{
		return new(std::nothrow) RockchipMppDecoder;
	}

	status_t GetSupportedFormats(media_format** formats, size_t* count) override
	{
		static media_format supported[3];
		static bool initialized = false;
		if (!initialized) {
			const AVCodecID codecs[] = {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC,
				AV_CODEC_ID_AV1};
			BMediaFormats registry;
			for (size_t index = 0; index < 3; index++) {
				media_format_description description = {};
				description.family = B_MISC_FORMAT_FAMILY;
				description.u.misc.file_format = 'ffmp';
				description.u.misc.codec = codecs[index];
				media_format format = {};
				format.type = B_MEDIA_ENCODED_VIDEO;
				format.deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;
				if (registry.MakeFormatFor(&description, 1, &format) != B_OK)
					return B_ERROR;
				supported[index] = format;
			}
			initialized = true;
		}
		*formats = supported;
		*count = 3;
		return B_OK;
	}
};


extern "C" MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) RockchipMppPlugin;
}
