/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rk_mpi.h"

int
main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s sample.h264|sample.h265|sample.obu [frame.raw]\n",
            argv[0]);
        return 2;
    }
    const char* suffix = strrchr(argv[1], '.');
    MppCodingType coding = MPP_VIDEO_CodingAVC;
    const char* codec = "h264";
    if (suffix != NULL
        && (strcmp(suffix, ".h265") == 0 || strcmp(suffix, ".hevc") == 0)) {
        coding = MPP_VIDEO_CodingHEVC;
        codec = "h265";
    } else if (suffix != NULL
        && (strcmp(suffix, ".obu") == 0 || strcmp(suffix, ".av1") == 0)) {
        coding = MPP_VIDEO_CodingAV1;
        codec = "av1";
    }
    FILE* input = fopen(argv[1], "rb");
    if (input == NULL)
        return 1;
    fseek(input, 0, SEEK_END);
    long length = ftell(input);
    rewind(input);
    if (length <= 0 || length > 16 * 1024 * 1024) {
        fclose(input);
        return 1;
    }
    void* bytes = malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1, (size_t)length, input) != (size_t)length) {
        fclose(input);
        free(bytes);
        return 1;
    }
    fclose(input);

    MppCtx context = NULL;
    MppApi* api = NULL;
    MppPacket packet = NULL;
    MppFrame frame = NULL;
    MppBufferGroup group = NULL;
    MPP_RET create = mpp_create(&context, &api);
    MPP_RET init = create == MPP_OK
        ? mpp_init(context, MPP_CTX_DEC, coding) : MPP_NOK;
    RK_U32 split = 1;
    MPP_RET configure = init == MPP_OK
        ? api->control(context, MPP_DEC_SET_PARSER_SPLIT_MODE, &split) : MPP_NOK;
    MPP_RET packet_init = configure == MPP_OK
        ? mpp_packet_init(&packet, bytes, (size_t)length) : MPP_NOK;
    if (packet_init == MPP_OK)
        mpp_packet_set_eos(packet);
    MPP_RET submit = packet_init == MPP_OK
        ? api->decode_put_packet(context, packet) : MPP_NOK;
    MPP_RET receive = MPP_NOK;
    MPP_RET group_open = MPP_NOK;
    MPP_RET group_set = MPP_NOK;
    MPP_RET ready = MPP_NOK;
    int info_change = 0;
    int got_frame = 0;
    uint32_t frame_error = 0;
    uint32_t width = 0, height = 0, horizontal = 0, vertical = 0, format = 0;
    size_t frame_bytes = 0;
    unsigned long long checksum = 1469598103934665603ULL;
    int dumped = argc == 2;
    if (submit == MPP_OK) {
        for (int attempt = 0; attempt < 300 && !got_frame; attempt++) {
            receive = api->decode_get_frame(context, &frame);
            if (frame != NULL) {
                if (mpp_frame_get_info_change(frame)) {
                    info_change++;
                    group_open = mpp_buffer_group_get_internal(&group,
                        MPP_BUFFER_TYPE_DMA_HEAP);
                    group_set = group_open == MPP_OK ? api->control(context,
                        MPP_DEC_SET_EXT_BUF_GROUP, group) : MPP_NOK;
                    ready = group_set == MPP_OK ? api->control(context,
                        MPP_DEC_SET_INFO_CHANGE_READY, NULL) : MPP_NOK;
                } else {
                    got_frame = 1;
                    frame_error = mpp_frame_get_errinfo(frame)
                        | mpp_frame_get_discard(frame);
                    width = mpp_frame_get_width(frame);
                    height = mpp_frame_get_height(frame);
                    horizontal = mpp_frame_get_hor_stride(frame);
                    vertical = mpp_frame_get_ver_stride(frame);
                    format = mpp_frame_get_fmt(frame);
                    MppBuffer output = mpp_frame_get_buffer(frame);
                    if (output != NULL) {
                        const unsigned char* pixels = mpp_buffer_get_ptr(output);
                        size_t allocation_bytes = mpp_buffer_get_size(output);
                        frame_bytes = allocation_bytes;
                        if ((format & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP
                            && horizontal > 0 && vertical > 0
                            && (size_t)horizontal * vertical <= SIZE_MAX / 3) {
                            size_t image_bytes
                                = (size_t)horizontal * vertical * 3 / 2;
                            if (image_bytes <= allocation_bytes)
                                frame_bytes = image_bytes;
                        }
                        for (size_t index = 0; pixels != NULL
                                && index < frame_bytes; index++) {
                            checksum = (checksum ^ pixels[index])
                                * 1099511628211ULL;
                        }
                        if (argc == 3 && pixels != NULL) {
                            FILE* dump = fopen(argv[2], "wb");
                            if (dump != NULL) {
                                dumped = fwrite(pixels, 1, frame_bytes, dump)
                                    == frame_bytes;
                                fclose(dump);
                            }
                        }
                    }
                }
                mpp_frame_deinit(&frame);
            } else {
                usleep(10000);
            }
        }
    }
    if (packet != NULL)
        mpp_packet_deinit(&packet);
    printf("ROCK5_MPP_DECODE codec=%s create=%d init=%d configure=%d packet=%d"
        " submit=%d receive=%d info=%d group=%d/%d ready=%d frame=%d"
        " error=%u width=%u height=%u stride=%ux%u format=%u"
        " frame_bytes=%zu checksum=%016llx dumped=%d input_bytes=%ld\n",
        codec, create, init, configure, packet_init,
        submit, receive, info_change, group_open, group_set, ready,
        got_frame, frame_error, width, height, horizontal, vertical, format,
        frame_bytes, checksum, dumped, length);
    fflush(stdout);
    free(bytes);
    // MPP's worker waits for a hardware completion after its job was rejected.
    // Exit the diagnostic without joining it; the team teardown releases all
    // driver handles and buffers.
    _exit(create == MPP_OK && init == MPP_OK && configure == MPP_OK
        && packet_init == MPP_OK && submit == MPP_OK && info_change > 0
        && group_open == MPP_OK && group_set == MPP_OK && ready == MPP_OK
        && got_frame && frame_error == 0 && width > 0 && height > 0
        && frame_bytes > 0 && dumped ? 0 : 1);
}
