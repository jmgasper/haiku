/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef V3D_REGS_H
#define V3D_REGS_H


// Broadcom V3D 4.x (BCM2711). Two register blocks: the hub and core 0.

// hub
#define V3D_HUB_AXICFG				0x0000
#define V3D_HUB_UIFCFG				0x0004
#define V3D_HUB_IDENT0				0x0008
#define V3D_HUB_IDENT1				0x000c
#define  V3D_HUB_IDENT1_TVER(x)		((x) & 0xf)
#define  V3D_HUB_IDENT1_REV(x)		(((x) >> 4) & 0xf)
#define  V3D_HUB_IDENT1_NCORES(x)	(((x) >> 8) & 0xf)
#define  V3D_HUB_IDENT1_WITH_TFU	(1 << 17)
#define V3D_HUB_IDENT2				0x0010
#define  V3D_HUB_IDENT2_WITH_MMU	(1 << 8)
#define V3D_HUB_IDENT3				0x0014
#define V3D_HUB_INT_STS				0x0050
#define V3D_HUB_INT_CLR				0x0058
#define V3D_HUB_INT_MSK_SET			0x0060
#define V3D_HUB_INT_MSK_CLR			0x0064

// core
#define V3D_CTL_IDENT0				0x0000
#define V3D_CTL_IDENT1				0x0004
#define V3D_CTL_IDENT2				0x0008
#define V3D_CTL_INT_STS				0x0050
#define V3D_CTL_INT_CLR				0x0058
#define V3D_CTL_INT_MSK_SET			0x0060
#define V3D_CTL_INT_MSK_CLR			0x0064

// power management block ("pm") and its bridge control ("rpivid_asb")
#define PM_GRAFX					0x10c
#define  PM_V3DRSTN					(1 << 6)
#define PM_PASSWORD					0x5a000000
#define ASB_V3D_S_CTRL				0x08
#define ASB_V3D_M_CTRL				0x0c
#define  ASB_REQ_STOP				(1 << 0)
#define  ASB_ACK					(1 << 1)


#endif	/* V3D_REGS_H */
