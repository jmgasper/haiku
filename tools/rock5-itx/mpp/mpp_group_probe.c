/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#define MODULE_TAG "rock5_mpp_group_probe"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "mpp_buffer.h"

typedef struct {
    uint32_t version;
    uint32_t handle;
    uint64_t offset;
    uint64_t bytes;
} BufferValidation;

#define VPU_VALIDATE_BUFFER 0x5256500au

int
main(void)
{
    MppBufferGroup group = NULL;
    MppBuffer buffer = NULL;
    if (mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DMA_HEAP) != MPP_OK
        || group == NULL) {
        fputs("ROCK5_MPP_GROUP open failed\n", stderr);
        return 1;
    }
    if (mpp_buffer_get(group, &buffer, 1024 * 1024) != MPP_OK
        || buffer == NULL) {
        fputs("ROCK5_MPP_GROUP allocation failed\n", stderr);
        mpp_buffer_group_put(group);
        return 1;
    }
    int handle = mpp_buffer_get_fd(buffer);
    size_t size = mpp_buffer_get_size(buffer);
    volatile uint8_t* memory = (volatile uint8_t*)mpp_buffer_get_ptr(buffer);
    int valid = handle > 0 && size >= 1024 * 1024 && memory != NULL;
    if (valid) {
        for (size_t i = 0; i < size; i += 4096)
            memory[i] = (uint8_t)(i / 4096);
        valid = mpp_buffer_sync_end(buffer) == MPP_OK
            && mpp_buffer_sync_begin(buffer) == MPP_OK;
        for (size_t i = 0; valid && i < size; i += 4096)
            valid = memory[i] == (uint8_t)(i / 4096);
    }
    int device = open("/dev/video/rk3588_vpu/0", O_RDWR);
    if (device < 0)
        valid = 0;
    else {
        BufferValidation reference = {1, (uint32_t)handle, size - 4096, 4096};
        valid = valid && ioctl(device, VPU_VALIDATE_BUFFER,
            &reference, sizeof(reference)) == 0;
        reference.offset++;
        valid = valid && ioctl(device, VPU_VALIDATE_BUFFER,
            &reference, sizeof(reference)) != 0;
        close(device);
    }
    MPP_RET release = mpp_buffer_put(buffer);
    MPP_RET close = mpp_buffer_group_put(group);
    printf("ROCK5_MPP_GROUP handle=%d bytes=%zu mapped_and_validated=%d"
        " release=%d close=%d\n",
        handle, size, valid, release, close);
    return valid && release == MPP_OK && close == MPP_OK ? 0 : 1;
}
