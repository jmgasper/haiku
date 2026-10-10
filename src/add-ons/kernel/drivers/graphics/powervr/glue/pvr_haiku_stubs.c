/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	Functions the reused files refer to for cases this GPU never reaches
	yet: the META and RISC-V firmware processors (the BXM-4-64 runs its
	firmware on MIPS; pvr_fw_meta.c and pvr_fw_riscv.c map their code
	through a kernel GPU VM context, which a MIPS core does not have), the
	kernel VM context's page table root, and the free-list requests of the
	firmware, which come with render jobs (M4). Each says so if it runs. */


#include "pvr_device.h"
#include "pvr_free_list.h"
#include "pvr_fw.h"
#include "pvr_fw_meta.h"
#include "pvr_vm.h"


static int
pvr_haiku_unsupported_init(struct pvr_device* pvr_dev)
{
	drm_err(from_pvr_device(pvr_dev), "only MIPS firmware processors are"
		" supported\n");
	return -ENODEV;
}


const struct pvr_fw_defs pvr_fw_defs_meta = {
	.init = pvr_haiku_unsupported_init,
};

const struct pvr_fw_defs pvr_fw_defs_riscv = {
	.init = pvr_haiku_unsupported_init,
};


int
pvr_meta_cr_read32(struct pvr_device* pvr_dev, u32 reg_addr,
	u32* reg_value_out)
{
	(void)reg_addr;
	*reg_value_out = 0;
	drm_err(from_pvr_device(pvr_dev), "no META firmware processor\n");
	return -ENODEV;
}


dma_addr_t
pvr_vm_get_page_table_root_addr(struct pvr_vm_context* vm_ctx)
{
	// only rogue_bif_init() asks, and only for META and RISC-V
	(void)vm_ctx;
	lx_log(LX_LOG_ERROR, "no kernel VM context before M3");
	return 0;
}


void
pvr_free_list_process_grow_req(struct pvr_device* pvr_dev,
	struct rogue_fwif_fwccb_cmd_freelist_gs_data* req)
{
	(void)req;
	drm_warn(from_pvr_device(pvr_dev), "FWCCB: free list grow request"
		" without free lists, ignored\n");
}


void
pvr_free_list_process_reconstruct_req(struct pvr_device* pvr_dev,
	struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data* req)
{
	(void)req;
	drm_warn(from_pvr_device(pvr_dev), "FWCCB: free list reconstruction"
		" request without free lists, ignored\n");
}
