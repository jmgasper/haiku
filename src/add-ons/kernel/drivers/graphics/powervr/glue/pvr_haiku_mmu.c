/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	The MMU cache flush of drm/imagination's pvr_mmu.c, until that file is
	reused as a whole with the GPU page tables of user VM contexts (M3).
	Same flags and command as pvr_mmu_flush_request_all() and
	pvr_mmu_flush_exec(); a failed flush is reported instead of followed by
	a hard reset and a retry, which come with the reset code (M4). */


#include "pvr_ccb.h"
#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_haiku_device.h"
#include "pvr_mmu.h"
#include "pvr_rogue_fwif.h"


/* pvr_mmu.c's PVR_MMU_SYNC_LEVEL_2_FLAGS: page tables, directories and
   catalogues, the TLB, and an interrupt once done. */
#define PVR_MMU_SYNC_LEVEL_2_FLAGS \
	(ROGUE_FWIF_MMUCACHEDATA_FLAGS_PT \
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_INTERRUPT \
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_TLB \
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_PD \
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_PC)


void
pvr_mmu_flush_request_all(struct pvr_device* pvr_dev)
{
	atomic_fetch_or(PVR_MMU_SYNC_LEVEL_2_FLAGS,
		&pvr_dev->mmu_flush_cache_flags);
}


int
pvr_mmu_flush_exec(struct pvr_device* pvr_dev, bool wait)
{
	struct rogue_fwif_kccb_cmd command = {};
	struct rogue_fwif_mmucachedata* data
		= &command.cmd_data.mmu_cache_data;
	(void)wait;
		// pvr_mmu.c waits for the first attempt either way

	// Nothing to flush before the firmware runs.
	if (!READ_ONCE(pvr_dev->fw_dev.initialised))
		return 0;

	data->cache_flags = atomic_xchg(&pvr_dev->mmu_flush_cache_flags, 0);
	if (data->cache_flags == 0)
		return 0;

	command.cmd_type = ROGUE_FWIF_KCCB_CMD_MMUCACHE;
	pvr_fw_object_get_fw_addr(pvr_dev->fw_dev.mem.mmucache_sync_obj,
		&data->mmu_cache_sync_fw_addr);
	data->mmu_cache_sync_update_value = 0;

	int error = pvr_haiku_kccb_execute(pvr_dev, &command, "MMU cache flush",
		true, NULL);
	if (error != 0) {
		drm_err(from_pvr_device(pvr_dev),
			"MMU cache flush failed (%d); no reset before M4\n", error);
	}
	return error;
}
