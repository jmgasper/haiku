/*
 * MediaTek MT7922 and its relatives.
 *
 * Distributed under the terms of the MIT License.
 */
#ifndef _MT7922_H_
#define _MT7922_H_

#include <KernelExport.h>
#include <PCI.h>
#include <lock.h>


#define MT7922_VENDOR_ID	0x14c3

/* The parts this reaches. They are one design with different radios on it,
 * and they differ here only in which firmware they ask for.
 */
#define MT7922_CHIP_7922	0x7922
#define MT7922_CHIP_7921	0x7961
#define MT7922_CHIP_7920	0x7920
#define MT7922_CHIP_7902	0x7902


/* Addresses inside the chip, which are not where they appear in the window.
 * See mt7922_translate() for how each of these is reached.
 */
#define MT_HW_CHIPID		0x70010200
#define MT_HW_REV		0x70010204
#define MT_HW_BOUND		0x70010020

/* Whose registers these are. The chip's own firmware holds them until the
 * host asks, and asks are made here.
 */
#define MT_CONN_ON_LPCTL	0x7c060010
#define LPCTL_HOST_SET_OWN	(1 << 0)
#define LPCTL_HOST_CLR_OWN	(1 << 1)
#define LPCTL_HOST_OWN_SYNC	(1 << 2)
	/* Set means the firmware has them; clear means we do. */

#define MT_CONN_ON_MISC		0x7c0600f0
#define MT_TOP_MISC_FW_STATE	0x00000007
#define MT_TOP_MISC2_FW_PWR_ON	(1 << 0)
#define MT_TOP_MISC2_FW_N9_RDY	0x00000003

/* Which mode the part's firmware comes up in. Normal is zero, and it has to
 * be said before the firmware is sent rather than after.
 */
#define MT_SWDEF_MODE			0x0041f23c
#define MT_SWDEF_NORMAL_MODE		0

/* A second, different claim of ownership, made once the rings exist and
 * before the firmware goes over them. This is not the one that wakes the
 * registers - that is MT_CONN_ON_LPCTL above, and this is no substitute for
 * it.
 */
#define MT_TOP_LPCR_HOST_BAND0		0x18060010
#define LPCR_HOST_FW_OWN		(1 << 0)
#define LPCR_HOST_DRV_OWN		(1 << 1)

/* What the radio does with what it hears. The first radio's blocks are named
 * here; the second's are the same, one stride along.
 */
#define MT7922_BAND_STRIDE		0x10000
#define MT7922_STATION_COUNT		20

#define MT_MDP_DCR0			0x820cd000
#define MT_MDP_DCR0_DAMSDU_EN		(1 << 15)
#define MT_MDP_DCR1			0x820cd004

#define MT_WTBL_UPDATE			0x820d4230
#define MT_WTBL_UPDATE_INDEX_MASK	0x3ff
#define MT_WTBL_UPDATE_CLEAR		(1 << 12)
#define MT_WTBL_UPDATE_BUSY		(1u << 31)

#define MT_TMAC_CTCR0			0x820e40f4
#define MT_TMAC_CTCR0_DDLMT_EN		(1 << 17)
#define MT_TMAC_CTCR0_VHT_SMPDU_EN	(1 << 18)

#define MT_WF_RMAC_MIB_AIRTIME0		0x820e5380
#define MT_WF_RMAC_MIB_TIME0		0x820e53c4
#define MT_RMAC_MIB_RXTIME_EN		(1u << 30)

#define MT_MIB_SCR1			0x820ed004
#define MT_MIB_TXDUR_EN			(1 << 8)
#define MT_MIB_RXDUR_EN			(1 << 9)

#define MT_DMA_DCR0			0x820e7000
#define MT_DMA_DCR0_RXD_G5_EN		(1 << 23)

#define MT_WTBLOFF_TOP_RSCR		0x820e9008

/* What the receiver throws away before we ever see it. */
/* Whether the radio is allowed to transmit and receive at all. Nothing else
 * in this driver touches these, and nothing clears them at reset - they are
 * cleared by the routine that sets the air timings, which is why a radio that
 * has never had its timings set hears nothing.
 */
#define MT_ARB_SCR			0x820e3080
#define MT_ARB_SCR_TX_DISABLE		(1 << 8)
#define MT_ARB_SCR_RX_DISABLE		(1 << 9)

/* The air timings themselves. */
#define MT_TMAC_CDTR			0x820e4090
#define MT_TMAC_ODTR			0x820e4094
#define MT_TMAC_ICR0			0x820e40a4
#define MT_AGG_ACR0			0x820e2084
#define MT_AGG_ACR_CFEND_RATE		0x3fff

#define MT_MDP_BNRCFR0			0x820cd070
#define MT_WF_RFCR			0x820e5000
#define MT_RFCR_DROP_OTHER_BEACON	(1 << 11)

#define MT7922_FILTER_ENABLE		(1u << 31)
#define MT7922_FILTER_OTHER_BSS		(1 << 6)

/* The Wi-Fi subsystem's own reset. */
#define MT_WFSYS_SW_RST_B	0x18000140
#define WFSYS_SW_RST_B		(1 << 0)
#define WFSYS_SW_INIT_DONE	(1 << 4)

/* The window, and the register that points it. The upper half of the address
 * being reached goes into the LOWER half of this register, which is the one
 * thing here that is easy to get backwards.
 */
#define MT_HIF_REMAP_L1		0x000fe24c
#define MT_HIF_REMAP_BASE_L1	0x00040000
#define MT_HIF_REMAP_L1_MASK	0x0000ffff

/* Anything below this is already where it appears in the window. */
#define MT_DIRECT_LIMIT		0x00100000


/* A block of memory the card fetches from by itself: where it is to us, where
 * it is to the card, and the area holding it.
 */
struct mt7922_dma_mem {
	area_id		area;
	void*		address;
	phys_addr_t	physical;
	size_t		size;
};



/* The transfer engine. Everything here is already where it appears in the
 * window, so none of it needs the window moved.
 */
#define MT_WFDMA0_BASE			0x000d4000
#define MT_WFDMA0_RST			0x000d4100
#define MT_WFDMA0_RST_LOGIC_RST		(1 << 4)
#define MT_WFDMA0_RST_DMASHDL_ALL_RST	(1 << 5)

#define MT_WFDMA0_HOST_INT_STA		0x000d4200
#define MT_WFDMA0_HOST_INT_ENA		0x000d4204
#define MT_MCU2HOST_SW_INT_ENA		0x000d41f4
#define MT_MCU_CMD_WAKE_RX_PCIE		(1 << 0)

#define MT_WFDMA0_GLO_CFG		0x000d4208
#define MT_WFDMA0_TX_DMA_EN		(1 << 0)
#define MT_WFDMA0_TX_DMA_BUSY		(1 << 1)
#define MT_WFDMA0_RX_DMA_EN		(1 << 2)
#define MT_WFDMA0_RX_DMA_BUSY		(1 << 3)
#define MT_WFDMA0_DMA_SIZE_SHIFT	4
#define MT_WFDMA0_DMA_SIZE_MASK		0x00000030
#define MT_WFDMA0_TX_WB_DDONE		(1 << 6)
#define MT_WFDMA0_FIFO_DIS_CHECK	(1 << 11)
#define MT_WFDMA0_FIFO_LITTLE_ENDIAN	(1 << 12)
#define MT_WFDMA0_RX_WB_DDONE		(1 << 13)
#define MT_WFDMA0_CSR_DISP_BASE_PTR_CHAIN_EN	(1 << 15)
#define MT_WFDMA0_OMIT_RX_INFO_PFET2	(1 << 21)
#define MT_WFDMA0_OMIT_RX_INFO		(1 << 27)
#define MT_WFDMA0_OMIT_TX_INFO		(1 << 28)
#define MT_WFDMA0_CLK_GAT_DIS		(1 << 30)

#define MT_WFDMA0_RST_DTX_PTR		0x000d420c
#define MT_WFDMA0_GLO_CFG_EXT0		0x000d42b0
#define MT_WFDMA0_CSR_TX_DMASHDL_EN	(1 << 6)
#define MT_WFDMA0_PRI_DLY_INT_CFG0	0x000d42f0

/* Where each ring's four registers live: descriptor address, how many there
 * are, where we have got to, and where the card has got to.
 */
#define MT_TX_RING_BASE			0x000d4300
#define MT_RX_EVENT_RING_BASE		0x000d4500
#define MT_RX_DATA_RING_BASE		0x000d4520
#define MT_RX_WA_RING_BASE		0x000d4540
#define MT_RING_SIZE			0x10
#define MT_RING_DESC_BASE		0x00
#define MT_RING_COUNT			0x04
#define MT_RING_CPU_INDEX		0x08
#define MT_RING_DMA_INDEX		0x0c

#define MT_WFDMA0_TX_RING_EXT_CTRL	0x000d4600
#define MT_WFDMA0_RX_RING_EXT_CTRL	0x000d4680

/* The part answers on one ring while it is being given its firmware and on
 * another once that firmware is running. Both have the same hardware index
 * and are told apart only by where their registers live.
 */
#define MT_RX_LATE_RING_BASE		0x000d4540

#define MT_DMASHDL_SW_CONTROL		0x7c026004
#define MT_DMASHDL_BYPASS		(1 << 28)

#define MT_WFDMA_DUMMY_CR		0x54000120
#define MT_WFDMA_NEED_REINIT		(1 << 1)

/* Which rings this part uses, and how deep each is. */
#define MT7922_TXQ_BAND0		0
#define MT7922_TXQ_FWDL			16
#define MT7922_TXQ_MCU_WM		17
#define MT7922_RXQ_BAND0		0
#define MT7922_RXQ_MCU_WM		0

#define MT7922_TX_RING_SIZE		2048
#define MT7922_TX_MCU_RING_SIZE		256
#define MT7922_TX_FWDL_RING_SIZE	128
#define MT7922_RX_MCU_RING_SIZE		8
#define MT7922_RX_LATE_RING_SIZE	512
#define MT7922_RX_DATA_RING_SIZE	256
	/* Fewer than the vendor uses. This only has to hold what a sweep of the
	 * channels turns up, not a working network's traffic.
	 */
#define MT7922_RX_WA_RING_SIZE		512
#define MT7922_RX_RING_SIZE		1536


/* One descriptor. The card walks an array of these; note that the control
 * word is the second of the four, not the last, and that writing it is what
 * hands a descriptor over - so it is always written last.
 */
struct mt7922_desc {
	uint32	buf0;
	uint32	ctrl;
	uint32	buf1;
	uint32	info;
} _PACKED;

#define MT_DMA_CTL_SD_LEN0_SHIFT	16
#define MT_DMA_CTL_SD_LEN0_MASK		0x3fff0000
#define MT_DMA_CTL_LAST_SEC0		(1u << 30)
#define MT_DMA_CTL_DMA_DONE		(1u << 31)

#define MT7922_RX_BUFFER_SIZE		2048
#define MT7922_DESC_SIZE		16


/* One ring: the descriptors the card walks, and where we think it has got to.
 */
struct mt7922_ring {
	mt7922_dma_mem	descriptors;
	mt7922_dma_mem	buffers;	/* for a ring the card writes into */
	uint32		registers;	/* where its four registers are */
	uint16		count;
	uint16		head;		/* the next one we will fill */
	uint16		tail;		/* the next one to reclaim */
};

/* One piece of a firmware file: where it is in the file, and where the part
 * wants it put.
 */
struct mt7922_region {
	uint32	offset;		/* where it starts in the file */
	uint32	size;		/* how much of the file it takes */
	uint32	address;	/* where it goes in the part */
	uint32	length;		/* how much is sent there */
	uint32	keyIndex;
	uint32	features;
};


struct mt7922_firmware {
	uint8*		data;
	size_t		size;
	uint32		count;
	mt7922_region	region[16];
};


/* A network the radio has heard, and where to find it again. */
struct mt7922_network {
	char	name[33];
	uint8	address[6];
	uint8	channel;
	int8	strength;
	bool	answered;	/* it replied to us, so it hears us too */
};

#define MT7922_MAX_NETWORKS	32


struct mt7922_dev {
	pci_info	pci;
	char		name[32];

	area_id		registersArea;
	volatile uint8*	registers;
	size_t		registersSize;

	/* There is one window for the whole chip, so pointing it and then
	 * reading through it has to happen without anyone else moving it.
	 */
	mutex		windowLock;

	uint32		chipId;
	uint32		revision;

	bool		owned;

	/* The rings the firmware travels over. The data rings come later; these
	 * are what it takes to talk to the part's own processor.
	 */
	mt7922_ring	firmwareRing;	/* firmware payload, outbound */
	mt7922_ring	commandRing;	/* commands, outbound */
	mt7922_ring	eventRing;	/* what it says back before firmware */
	mt7922_ring	lateEventRing;	/* and after: the part changes rings */
	mt7922_ring	dataRing;	/* what it hears on the air */
	mt7922_dma_mem	commandBuffer;	/* one command at a time */
	mt7922_dma_mem	firmwareBuffer;	/* one piece of firmware at a time */
	uint8		sequence;	/* ties an answer to what was asked */

	uint8		address[6];	/* what this radio answers to */
	bool		hasAddress;
	uint8		streams;
	uint8		bands;		/* 1 is 2.4 GHz, 2 is 5 GHz */
	int		framesShown;
	int		frameKind[16];
	int		management;
	int		packetKind[32];
	int		beacons;
	int		badFrames;
	int		tooShort;
	int		badKind[16];
	int		badBeacons;
	int		framesOnEvents;
	int		answers;

	/* A network to join, from the driver's settings. The second of these is
	 * a secret and is never logged.
	 */
	char		wanted[33];
	char		secret[64];

	mt7922_network	network[MT7922_MAX_NETWORKS];
	int		networks;
	int		chosen;
	bool		ringsReady;
};


extern pci_module_info* gPci;

status_t mt7922_dma_alloc(const char* name, size_t size,
	mt7922_dma_mem* memory);
status_t mt7922_dma_setup(mt7922_dev* device);
void mt7922_dma_teardown(mt7922_dev* device);
void mt7922_dma_free(mt7922_dma_mem* memory);

status_t mt7922_firmware_read_patch(mt7922_dev* device,
	mt7922_firmware* firmware);
status_t mt7922_firmware_read_ram(mt7922_dev* device,
	mt7922_firmware* firmware);
void mt7922_firmware_free(mt7922_firmware* firmware);

status_t mt7922_mcu_start_firmware(mt7922_dev* device);
status_t mt7922_mcu_read_capability(mt7922_dev* device);
status_t mt7922_mcu_prepare(mt7922_dev* device);
status_t mt7922_mcu_scan(mt7922_dev* device);
void mt7922_mac_set_timing(mt7922_dev* device);
void mt7922_dump_air(mt7922_dev* device, int wanted);

status_t mt7922_setup(mt7922_dev* device);
void mt7922_teardown(mt7922_dev* device);

uint32 mt7922_read32(mt7922_dev* device, uint32 address);
void mt7922_write32(mt7922_dev* device, uint32 address, uint32 value);

#endif	/* _MT7922_H_ */
