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

// hub: texture formatting unit
#define V3D_TFU_CS					0x0400
#define V3D_TFU_ICFG				0x0408
#define  V3D_TFU_ICFG_IOC			(1 << 0)
#define V3D_TFU_IIA					0x040c
#define V3D_TFU_ICA					0x0410
#define V3D_TFU_IIS					0x0414
#define V3D_TFU_IUA					0x0418
#define V3D_TFU_IOA					0x041c
#define V3D_TFU_IOS					0x0420
#define V3D_TFU_COEF0				0x0424
#define  V3D_TFU_COEF0_USECOEF		(1u << 31)
#define V3D_TFU_COEF1				0x0428
#define V3D_TFU_COEF2				0x042c
#define V3D_TFU_COEF3				0x0430

// hub: MMU
#define V3D_MMUC_CONTROL			0x1000
#define  V3D_MMUC_CONTROL_ENABLE	(1 << 0)
#define  V3D_MMUC_CONTROL_FLUSH		(1 << 1)
#define  V3D_MMUC_CONTROL_FLUSHING	(1 << 2)
#define  V3D_MMUC_CONTROL_CLEAR		(1 << 3)
#define V3D_MMU_CTL					0x1200
#define  V3D_MMU_CTL_ENABLE			(1 << 0)
#define  V3D_MMU_CTL_TLB_CLEAR		(1 << 2)
#define  V3D_MMU_CTL_TLB_CLEARING	(1 << 7)
#define  V3D_MMU_CTL_WRITE_VIOLATION_INT	(1 << 10)
#define  V3D_MMU_CTL_WRITE_VIOLATION_ABORT	(1 << 11)
#define  V3D_MMU_CTL_PT_INVALID_ENABLE		(1 << 16)
#define  V3D_MMU_CTL_PT_INVALID_INT			(1 << 18)
#define  V3D_MMU_CTL_PT_INVALID_ABORT		(1 << 19)
#define  V3D_MMU_CTL_CAP_EXCEEDED_INT		(1 << 25)
#define  V3D_MMU_CTL_CAP_EXCEEDED_ABORT		(1 << 26)
#define V3D_MMU_PT_PA_BASE			0x1204
#define V3D_MMU_VIO_ID				0x122c
#define V3D_MMU_ILLEGAL_ADDR		0x1230
#define  V3D_MMU_ILLEGAL_ADDR_ENABLE	(1u << 31)
#define V3D_MMU_VIO_ADDR			0x1234
#define V3D_MMU_DEBUG_INFO			0x1238

#define V3D_HUB_INT_TFUC			(1 << 1)
#define V3D_HUB_INT_MMU_CAP			(1 << 3)
#define V3D_HUB_INT_MMU_PTI			(1 << 4)
#define V3D_HUB_INT_MMU_WRV			(1 << 5)

#define V3D_PTE_VALID				(1 << 28)
#define V3D_PTE_WRITEABLE			(1 << 29)

// core: caches, control lists, binner memory, compute
#define V3D_CTL_SLCACTL				0x0024
#define V3D_CTL_L2TCACTL			0x0030
#define  V3D_L2TCACTL_L2TFLS		(1 << 0)
#define  V3D_L2TCACTL_FLM_FLUSH		(0 << 1)
#define  V3D_L2TCACTL_FLM_CLEAN		(2 << 1)
#define  V3D_L2TCACTL_TMUWCF		(1 << 8)
#define V3D_CTL_L2TFLSTA			0x0034
#define V3D_CTL_L2TFLEND			0x0038
#define V3D_INT_FRDONE				(1 << 0)
#define V3D_INT_FLDONE				(1 << 1)
#define V3D_INT_OUTOMEM				(1 << 2)
#define V3D_INT_GMPV				(1 << 5)
#define V3D_INT_CSDDONE				(1 << 7)
#define V3D_CLE_CT0CS				0x0100
#define V3D_CLE_CT1CS				0x0104
#define V3D_CLE_CT0CA				0x0110
#define V3D_CLE_CT1CA				0x0114
#define V3D_CLE_CT0QTS				0x015c
#define  V3D_CLE_CT0QTS_ENABLE		(1 << 1)
#define V3D_CLE_CT0QBA				0x0160
#define V3D_CLE_CT1QBA				0x0164
#define V3D_CLE_CT0QEA				0x0168
#define V3D_CLE_CT1QEA				0x016c
#define V3D_CLE_CT0QMA				0x0170
#define V3D_CLE_CT0QMS				0x0174
#define V3D_PTB_BPOA				0x0308
#define V3D_PTB_BPOS				0x030c
#define V3D_CSD_STATUS				0x0900
#define V3D_CSD_QUEUED_CFG0			0x0904
#define V3D_ERR_STAT				0x0f20

// power management block ("pm") and its bridge control ("rpivid_asb")
#define PM_GRAFX					0x10c
#define  PM_V3DRSTN					(1 << 6)
#define PM_PASSWORD					0x5a000000
#define ASB_V3D_S_CTRL				0x08
#define ASB_V3D_M_CTRL				0x0c
#define  ASB_REQ_STOP				(1 << 0)
#define  ASB_ACK					(1 << 1)


#endif	/* V3D_REGS_H */
