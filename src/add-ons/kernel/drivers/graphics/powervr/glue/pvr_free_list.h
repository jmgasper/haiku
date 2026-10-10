/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PVR_FREE_LIST_H
#define PVR_FREE_LIST_H


/*	Stand-in for drm/imagination's pvr_free_list.h until the free lists
	themselves are reused (M4, with render jobs): just the two FWCCB
	requests pvr_ccb.c passes on. The firmware sends them only for render
	jobs; pvr_haiku_stubs.c logs them. */


#include <linux/types.h>


struct pvr_device;
struct rogue_fwif_fwccb_cmd_freelist_gs_data;
struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data;


void	pvr_free_list_process_grow_req(struct pvr_device* pvr_dev,
			struct rogue_fwif_fwccb_cmd_freelist_gs_data* req);
void	pvr_free_list_process_reconstruct_req(struct pvr_device* pvr_dev,
			struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data* req);


#endif	/* PVR_FREE_LIST_H */
