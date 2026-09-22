/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <stdint.h>
#include <stdio.h>
#include "mpp_device.h"
#include "mpp_buffer.h"
#include "mpp_buffer_impl.h"
#include "mpp_platform.h"

int
main(void)
{
    MppIoctlVersion version = mpp_get_ioctl_version();
    uint32_t support = mpp_get_vcodec_type();
    uint32_t id = mpp_get_client_hw_id(VPU_CLIENT_RKVDEC);
    MppDev device = NULL;
    MPP_RET init = mpp_dev_init(&device, VPU_CLIENT_RKVDEC);
	MppBufferGroup group = NULL;
	MppBuffer buffer = NULL;
	MPP_RET group_open = mpp_buffer_group_get_internal(&group,
		MPP_BUFFER_TYPE_DMA_HEAP);
	MPP_RET allocate = group_open == MPP_OK
		? mpp_buffer_get(group, &buffer, 1024 * 1024) : MPP_NOK;
	uint32_t handle = allocate == MPP_OK
		? (uint32_t)mpp_buffer_get_fd(buffer) : UINT32_MAX;
	MPP_RET attach = init == MPP_OK && allocate == MPP_OK
		? mpp_buffer_attach_dev(buffer, device) : MPP_NOK;
	uint32_t iova = attach == MPP_OK
		? mpp_buffer_get_iova(buffer, device) : UINT32_MAX;
	MPP_RET detach = attach == MPP_OK
		? mpp_buffer_detach_dev(buffer, device) : MPP_NOK;
    uint32_t zero = 0;
    MppDevRegWrCfg write = {&zero, sizeof(zero), 0};
    MPP_RET queued = init == MPP_OK
        ? mpp_dev_ioctl(device, MPP_DEV_REG_WR, &write) : MPP_NOK;
    // The handshake is live, but a register job must fail until the guarded
    // submit path and completion handling are implemented.
    MPP_RET send = queued == MPP_OK
        ? mpp_dev_ioctl(device, MPP_DEV_CMD_SEND, NULL) : MPP_OK;
    MPP_RET close = init == MPP_OK ? mpp_dev_deinit(device) : MPP_NOK;
	MPP_RET release = allocate == MPP_OK ? mpp_buffer_put(buffer) : MPP_NOK;
	MPP_RET group_close = group_open == MPP_OK
		? mpp_buffer_group_put(group) : MPP_NOK;
    printf("ROCK5_MPP_DEVICE ioctl=%d support=%08x rkvdec_id=%08x"
        " init=%d attach=%d iova=%u detach=%d queued=%d send=%d"
		" close=%d release=%d group=%d\n", version, support, id, init,
		attach, iova, detach, queued, send, close, release, group_close);
    return version == IOCTL_MPP_SERVICE_V1
        && (support & (1u << VPU_CLIENT_RKVDEC)) != 0
        && id == 0x53813f05u && init == MPP_OK && queued == MPP_OK
		&& attach == MPP_OK && iova == handle
		&& detach == MPP_OK && send != MPP_OK && close == MPP_OK
		&& release == MPP_OK && group_close == MPP_OK ? 0 : 1;
}
