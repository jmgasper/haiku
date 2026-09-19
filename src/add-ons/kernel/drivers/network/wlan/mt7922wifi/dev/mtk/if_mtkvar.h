/*
 * MediaTek MT7922 and relatives, in the shape the wireless stack expects.
 *
 * Distributed under the terms of the MIT License.
 */
#ifndef _IF_MTKVAR_H_
#define _IF_MTKVAR_H_

#define MTK_VENDOR_MEDIATEK	0x14c3

/* Inside the chip, which is not where these appear in the window. */
#define MTK_HW_CHIPID		0x70010200
#define MTK_HW_REV		0x70010204

#define MTK_CONN_ON_LPCTL	0x7c060010
#define MTK_LPCTL_SET_OWN	(1 << 0)
#define MTK_LPCTL_CLR_OWN	(1 << 1)
#define MTK_LPCTL_OWN_SYNC	(1 << 2)

/* The window, and the register that aims it. The upper half of the address
 * being reached goes into the LOWER half of this register.
 */
#define MTK_HIF_REMAP_L1	0x000fe24c
#define MTK_HIF_REMAP_BASE_L1	0x00040000
#define MTK_HIF_REMAP_L1_MASK	0x0000ffff

#define MTK_DIRECT_LIMIT	0x00100000

/* The transfer engine. All of this is already where it appears in the window.
 */
#define MTK_WFDMA0_RST			0x000d4100
#define MTK_WFDMA0_HOST_INT_ENA		0x000d4204
#define MTK_MCU2HOST_SW_INT_ENA		0x000d41f4
#define MTK_MCU_CMD_WAKE_RX_PCIE	(1 << 0)
#define MTK_INTERRUPTS_ALL		0x2c7ffff5

#define MTK_WFDMA0_GLO_CFG		0x000d4208
#define MTK_TX_DMA_EN			(1 << 0)
#define MTK_TX_DMA_BUSY			(1 << 1)
#define MTK_RX_DMA_EN			(1 << 2)
#define MTK_RX_DMA_BUSY			(1 << 3)
#define MTK_DMA_SIZE_MASK		0x00000030
#define MTK_DMA_SIZE_SHIFT		4
#define MTK_TX_WB_DDONE			(1 << 6)
#define MTK_FIFO_DIS_CHECK		(1 << 11)
#define MTK_FIFO_LITTLE_ENDIAN		(1 << 12)
#define MTK_RX_WB_DDONE			(1 << 13)
#define MTK_CSR_DISP_BASE_PTR_CHAIN_EN	(1 << 15)
#define MTK_OMIT_RX_INFO_PFET2		(1 << 21)
#define MTK_OMIT_RX_INFO		(1 << 27)
#define MTK_OMIT_TX_INFO		(1 << 28)
#define MTK_CLK_GAT_DIS			(1u << 30)

#define MTK_WFDMA0_RST_DTX_PTR		0x000d420c
#define MTK_WFDMA0_GLO_CFG_EXT0		0x000d42b0
#define MTK_CSR_TX_DMASHDL_EN		(1 << 6)
#define MTK_WFDMA0_PRI_DLY_INT_CFG0	0x000d42f0

#define MTK_TX_RING_BASE		0x000d4300
#define MTK_RX_EVENT_RING_BASE		0x000d4500
#define MTK_RX_DATA_RING_BASE		0x000d4520
#define MTK_RX_LATE_RING_BASE		0x000d4540
#define MTK_RING_SIZE			0x10
#define MTK_RING_DESC_BASE		0x00
#define MTK_RING_COUNT			0x04
#define MTK_RING_CPU_INDEX		0x08
#define MTK_RING_DMA_INDEX		0x0c

#define MTK_WFDMA0_TX_RING_EXT_CTRL	0x000d4600
#define MTK_WFDMA0_RX_RING_EXT_CTRL	0x000d4680

#define MTK_DMASHDL_SW_CONTROL		0x7c026004
#define MTK_DMASHDL_BYPASS		(1 << 28)
#define MTK_WFDMA_DUMMY_CR		0x54000120
#define MTK_WFDMA_NEED_REINIT		(1 << 1)

/* Which rings, how deep, and how big a piece each holds. */
#define MTK_TXQ_BAND0			0
#define MTK_TXQ_FWDL			16
#define MTK_TXQ_MCU_WM			17
#define MTK_RXQ_MCU			0

#define MTK_TX_RING_COUNT		256
#define MTK_TX_MCU_RING_COUNT		256
#define MTK_TX_FWDL_RING_COUNT		128
#define MTK_RX_EVENT_RING_COUNT		8
#define MTK_RX_LATE_RING_COUNT		512
#define MTK_RX_DATA_RING_COUNT		256

#define MTK_RX_BUFFER_SIZE		2048
#define MTK_DESC_SIZE			16

#define MTK_DMA_CTL_LEN_SHIFT		16
#define MTK_DMA_CTL_LEN_MASK		0x3fff0000
#define MTK_DMA_CTL_LAST_SEC0		(1u << 30)
#define MTK_DMA_CTL_DMA_DONE		(1u << 31)


/* Talking to the part's own processor. */
#define MTK_MCU_TXD_SIZE		64
#define MTK_MCU_RXD_SIZE		36
#define MTK_TX_TYPE_CMD			2
#define MTK_TX_MCU_PORT_RX_Q0		0x20
#define MTK_TXD1_LONG_FORMAT		(1u << 31)
#define MTK_HDR_FORMAT_CMD		1
#define MTK_MCU_PKT_ID			0xa0
#define MTK_MCU_Q_SET			1
#define MTK_MCU_Q_NA			3
#define MTK_MCU_S2D_H2N			0

#define MTK_MCU_CMD_TARGET_ADDRESS_LEN	0x01
#define MTK_MCU_CMD_FW_START		0x02
#define MTK_MCU_CMD_NIC_POWER_CTRL	0x04
#define MTK_MCU_CMD_PATCH_START		0x05
#define MTK_MCU_CMD_PATCH_FINISH	0x07
#define MTK_MCU_CMD_PATCH_SEM		0x10
#define MTK_MCU_CE_GET_NIC_CAPAB	0x8a

#define MTK_PATCH_SEM_RELEASE		0
#define MTK_PATCH_SEM_GET		1
#define MTK_PATCH_IS_DL			1
#define MTK_PATCH_SEM_SUCCESS		2

#define MTK_DL_MODE_ENCRYPT		(1u << 0)
#define MTK_DL_MODE_RESET_SEC_IV	(1u << 3)
#define MTK_DL_CONFIG_ENCRY_MODE_SEL	(1u << 6)
#define MTK_DL_MODE_NEED_RSP		(1u << 31)
#define MTK_FW_START_OVERRIDE		(1u << 0)

#define MTK_FW_FEATURE_ENCRYPT		(1 << 0)
#define MTK_FW_FEATURE_ENCRY_MODE	(1 << 4)
#define MTK_FW_FEATURE_OVERRIDE		(1 << 5)
#define MTK_FW_FEATURE_NOT_SENT		(1 << 6)

#define MTK_NIC_CAP_MAC_ADDR		0x07
#define MTK_NIC_CAP_PHY			0x08

#define MTK_FIRMWARE_CHUNK		4096
#define MTK_PATCH_HEADER		96
#define MTK_PATCH_SECTION		64
#define MTK_RAM_TRAILER			36
#define MTK_RAM_REGION			40

#define MTK_PATCH_NAME			"WIFI_MT7922_patch_mcu_1_1_hdr.bin"
#define MTK_RAM_NAME			"WIFI_RAM_CODE_MT7922_1.bin"

/* A second claim of ownership, made once the rings exist. */
#define MTK_TOP_LPCR_HOST_BAND0		0x18060010
#define MTK_LPCR_HOST_FW_OWN		(1 << 0)
#define MTK_LPCR_HOST_DRV_OWN		(1 << 1)
#define MTK_SWDEF_MODE			0x0041f23c
#define MTK_CONN_ON_MISC		0x7c0600f0


/* Memory the card reaches by itself. */
struct mtk_dma_mem {
	bus_dma_tag_t		tag;
	bus_dmamap_t		map;
	void*			addr;
	bus_addr_t		paddr;
	bus_size_t		size;
};

/* One ring of descriptors the card walks. */
struct mtk_ring {
	struct mtk_dma_mem	desc;
	struct mtk_dma_mem	buffers;
	uint32_t		regs;
	uint16_t		count;
	uint16_t		head;
	uint16_t		tail;
};


struct mtk_vap {
	struct ieee80211vap	vap;
	int			(*newstate)(struct ieee80211vap *,
					enum ieee80211_state, int);
};
#define MTK_VAP(vap)	((struct mtk_vap *)(vap))


struct mtk_softc {
	struct ieee80211com	sc_ic;
	device_t		sc_dev;
	struct resource*	sc_mem;
	struct resource*	sc_irq;
	void*			sc_ih;
	bus_space_tag_t		sc_st;
	bus_space_handle_t	sc_sh;

	/* There is one window for the whole chip, so aiming it and then reading
	 * through it must not be interrupted by anyone else aiming it.
	 */
	struct mtx		sc_mtx;

	uint8_t			sc_macaddr[6];
	int			sc_running;

	struct mtk_ring		sc_txq;		/* frames of our own */
	struct mtk_ring		sc_cmdq;	/* commands */
	struct mtk_ring		sc_fwq;		/* firmware payload */
	struct mtk_ring		sc_eventq;	/* answers, before firmware */
	struct mtk_ring		sc_lateq;	/* answers, after it */
	struct mtk_ring		sc_dataq;	/* what the air brings */
	struct mtk_dma_mem	sc_cmdbuf;	/* one command at a time */
	struct mtk_dma_mem	sc_fwbuf;	/* one piece of firmware */
	uint8_t			sc_seq;
	uint8_t			sc_streams;
	int			sc_rings;

	uint32_t		sc_chipid;
	uint32_t		sc_rev;
	int			sc_owned;
};

uint32_t	mtk_read(struct mtk_softc*, uint32_t);
void		mtk_write(struct mtk_softc*, uint32_t, uint32_t);
int		mtk_dma_setup(struct mtk_softc*);
int		mtk_firmware_start(struct mtk_softc*);
void		mtk_dma_teardown(struct mtk_softc*);

#define MTK_LOCK(sc)		mtx_lock(&(sc)->sc_mtx)
#define MTK_UNLOCK(sc)		mtx_unlock(&(sc)->sc_mtx)

#endif	/* _IF_MTKVAR_H_ */
