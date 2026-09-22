#include <stdint.h>
#include <stdio.h>
#include "mpp_allocator.h"

int main(void)
{
    MppAllocator allocator = NULL;
    MppAllocatorApi *api = NULL;
    MPP_RET ret = mpp_allocator_get(&allocator, &api,
        MPP_BUFFER_TYPE_DMA_HEAP, MPP_ALLOC_FLAG_NONE);
    if (ret != MPP_OK || !allocator || !api) {
        fprintf(stderr, "ROCK5_MPP_BUFFER allocator=%d\n", ret);
        return 1;
    }
    MppBufferInfo info = {0};
    info.type = MPP_BUFFER_TYPE_DMA_HEAP;
    info.size = 1024 * 1024;
    ret = api->alloc(allocator, &info);
    if (ret != MPP_OK || info.fd <= 0 || !info.ptr || info.size < 1024 * 1024) {
        fprintf(stderr, "ROCK5_MPP_BUFFER alloc=%d handle=%d ptr=%p bytes=%zu\n",
            ret, info.fd, info.ptr, info.size);
        mpp_allocator_put(&allocator);
        return 1;
    }
    volatile uint8_t *memory = (volatile uint8_t *)info.ptr;
    for (size_t i = 0; i < info.size; i += 4096)
        memory[i] = (uint8_t)(i / 4096);
    for (size_t i = 0; i < info.size; i += 4096) {
        if (memory[i] != (uint8_t)(i / 4096)) {
            fprintf(stderr, "ROCK5_MPP_BUFFER mismatch=%zu\n", i);
            return 1;
        }
    }
    int handle = info.fd;
    ret = api->free(allocator, &info);
    if (ret != MPP_OK || info.fd != -1 || info.ptr != NULL) {
        fprintf(stderr, "ROCK5_MPP_BUFFER free=%d\n", ret);
        return 1;
    }
    ret = mpp_allocator_put(&allocator);
    if (ret != MPP_OK) {
        fprintf(stderr, "ROCK5_MPP_BUFFER close=%d\n", ret);
        return 1;
    }
    printf("ROCK5_MPP_BUFFER handle=%d bytes=1048576 mapped=1 freed=1\n", handle);
    return 0;
}
