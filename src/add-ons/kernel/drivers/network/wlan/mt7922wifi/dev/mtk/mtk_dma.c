/*
 * The rings the MT7922 works from.
 *
 * The card fetches its own descriptors and moves its own payloads, so the
 * memory it works from has to suit it rather than us: contiguous, because a
 * ring is one array as far as the hardware is concerned, and below four
 * gigabytes, because this part addresses no higher. Neither is what an
 * ordinary allocation gives, and neither failure announces itself - a ring
 * placed where the card cannot reach simply never advances.
 *
 * Distributed under the terms of the MIT License.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/socket.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include "if_mtkvar.h"


static void
mtk_dma_map_addr(void* arg, bus_dma_segment_t* segs, int nseg, int error)
{
	if (error == 0 && nseg > 0)
		*(bus_addr_t*)arg = segs[0].ds_addr;
}


static int
mtk_dma_alloc(struct mtk_softc* sc, struct mtk_dma_mem* memory,
	bus_size_t size, const char* what)
{
	int error;

	memory->size = size;

	/* Below four gigabytes, and all in one piece: the card walks a ring as
	 * a single array and addresses no higher than that.
	 */
	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev), 16, 0,
		BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
		size, 1, size, 0, NULL, NULL, &memory->tag);
	if (error != 0) {
		device_printf(sc->sc_dev, "no way to describe memory for %s\n", what);
		return error;
	}

	error = bus_dmamem_alloc(memory->tag, &memory->addr,
		BUS_DMA_NOWAIT | BUS_DMA_ZERO, &memory->map);
	if (error != 0) {
		device_printf(sc->sc_dev, "no memory for %s\n", what);
		return error;
	}

	error = bus_dmamap_load(memory->tag, memory->map, memory->addr, size,
		mtk_dma_map_addr, &memory->paddr, 0);
	if (error != 0) {
		device_printf(sc->sc_dev, "cannot place %s where the card reaches\n",
			what);
		return error;
	}

	bus_dmamap_sync(memory->tag, memory->map, BUS_DMASYNC_PREWRITE);
	return 0;
}


static void
mtk_dma_free(struct mtk_dma_mem* memory)
{
	if (memory->addr != NULL) {
		bus_dmamap_sync(memory->tag, memory->map, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(memory->tag, memory->map);
		bus_dmamem_free(memory->tag, memory->addr, memory->map);
		memory->addr = NULL;
	}
	if (memory->tag != NULL) {
		bus_dma_tag_destroy(memory->tag);
		memory->tag = NULL;
	}
}


/* Set a ring up and tell the card where it is. The order of these writes is
 * the card's, not ours: the descriptors are made ready first, then the
 * indices, then how many there are, and where they are last - because that
 * write is the one the card acts on.
 */
static int
mtk_ring_init(struct mtk_softc* sc, struct mtk_ring* ring, uint32_t base,
	uint32_t index, uint16_t count, const char* what)
{
	uint32_t* desc;
	uint32_t where;
	int error, i;

	error = mtk_dma_alloc(sc, &ring->desc, count * MTK_DESC_SIZE, what);
	if (error != 0)
		return error;

	ring->regs = base + index * MTK_RING_SIZE;
	ring->count = count;

	/* A descriptor marked done is one the card has finished with, which is
	 * how an empty ring looks to it.
	 */
	desc = (uint32_t*)ring->desc.addr;
	for (i = 0; i < count; i++)
		desc[i * 4 + 1] = MTK_DMA_CTL_DMA_DONE;

	mtk_write(sc, ring->regs + MTK_RING_CPU_INDEX, 0);
	mtk_write(sc, ring->regs + MTK_RING_DMA_INDEX, 0);
	mtk_write(sc, ring->regs + MTK_RING_COUNT, count);
	mtk_write(sc, ring->regs + MTK_RING_DESC_BASE, (uint32_t)ring->desc.paddr);

	where = mtk_read(sc, ring->regs + MTK_RING_DMA_INDEX);
	if (where >= count)
		where = 0;

	ring->head = where;
	ring->tail = where;

	return 0;
}


/* A receiving ring also needs somewhere for what arrives to be put. Clearing
 * a descriptor's done mark is what hands it to the card.
 */
static int
mtk_rx_ring_init(struct mtk_softc* sc, struct mtk_ring* ring, uint32_t base,
	uint32_t index, uint16_t count, const char* what)
{
	uint32_t* desc;
	int error, i;

	error = mtk_dma_alloc(sc, &ring->desc, count * MTK_DESC_SIZE, what);
	if (error != 0)
		return error;

	error = mtk_dma_alloc(sc, &ring->buffers,
		(bus_size_t)count * MTK_RX_BUFFER_SIZE, what);
	if (error != 0)
		return error;

	ring->regs = base + index * MTK_RING_SIZE;
	ring->count = count;

	desc = (uint32_t*)ring->desc.addr;
	for (i = 0; i < count; i++) {
		desc[i * 4 + 0] = (uint32_t)(ring->buffers.paddr
			+ (bus_addr_t)i * MTK_RX_BUFFER_SIZE);
		desc[i * 4 + 2] = 0;
		desc[i * 4 + 3] = 0;
		desc[i * 4 + 1] = (uint32_t)MTK_RX_BUFFER_SIZE
			<< MTK_DMA_CTL_LEN_SHIFT;
	}

	ring->head = count - 1;
	ring->tail = 0;

	mtk_write(sc, ring->regs + MTK_RING_CPU_INDEX, 0);
	mtk_write(sc, ring->regs + MTK_RING_DMA_INDEX, 0);
	mtk_write(sc, ring->regs + MTK_RING_COUNT, count);
	mtk_write(sc, ring->regs + MTK_RING_DESC_BASE, (uint32_t)ring->desc.paddr);

	wmb();
	mtk_write(sc, ring->regs + MTK_RING_CPU_INDEX, ring->head);

	return 0;
}


static void
mtk_ring_free(struct mtk_ring* ring)
{
	mtk_dma_free(&ring->desc);
	mtk_dma_free(&ring->buffers);
	ring->count = 0;
}


/* Each ring gets a slice of the engine's own buffer to read ahead into. A ring
 * without one has nowhere to stage what it fetches, and on this engine that
 * stops receiving while leaving sending looking perfectly healthy.
 */
static void
mtk_dma_prefetch(struct mtk_softc* sc)
{
	const uint32_t depth = 0x4;

	mtk_write(sc, MTK_WFDMA0_RX_RING_EXT_CTRL + 0x00, (0x000u << 16) | depth);
	mtk_write(sc, MTK_WFDMA0_RX_RING_EXT_CTRL + 0x08, (0x040u << 16) | depth);
	mtk_write(sc, MTK_WFDMA0_RX_RING_EXT_CTRL + 0x0c, (0x080u << 16) | depth);
	mtk_write(sc, MTK_WFDMA0_RX_RING_EXT_CTRL + 0x10, (0x0c0u << 16) | depth);
	mtk_write(sc, MTK_WFDMA0_RX_RING_EXT_CTRL + 0x14, (0x100u << 16) | depth);

	mtk_write(sc, MTK_WFDMA0_TX_RING_EXT_CTRL + 0x00, (0x140u << 16) | depth);
	mtk_write(sc, MTK_WFDMA0_TX_RING_EXT_CTRL + 0x40, (0x340u << 16) | depth);
	mtk_write(sc, MTK_WFDMA0_TX_RING_EXT_CTRL + 0x44, (0x380u << 16) | depth);
}


static void
mtk_dma_disable(struct mtk_softc* sc)
{
	uint32_t config;
	int i;

	config = mtk_read(sc, MTK_WFDMA0_GLO_CFG);
	config &= ~(MTK_TX_DMA_EN | MTK_RX_DMA_EN
		| MTK_CSR_DISP_BASE_PTR_CHAIN_EN | MTK_OMIT_TX_INFO
		| MTK_OMIT_RX_INFO | MTK_OMIT_RX_INFO_PFET2);
	mtk_write(sc, MTK_WFDMA0_GLO_CFG, config);

	for (i = 0; i < 100; i++) {
		config = mtk_read(sc, MTK_WFDMA0_GLO_CFG);
		if ((config & (MTK_TX_DMA_BUSY | MTK_RX_DMA_BUSY)) == 0)
			break;
		DELAY(1000);
	}

	/* The scheduler has opinions about which ring may send when; nothing
	 * here wants them yet.
	 */
	mtk_write(sc, MTK_DMASHDL_SW_CONTROL,
		mtk_read(sc, MTK_DMASHDL_SW_CONTROL) | MTK_DMASHDL_BYPASS);
	mtk_write(sc, MTK_WFDMA0_GLO_CFG_EXT0,
		mtk_read(sc, MTK_WFDMA0_GLO_CFG_EXT0) & ~MTK_CSR_TX_DMASHDL_EN);
}


static void
mtk_dma_enable(struct mtk_softc* sc)
{
	uint32_t config;

	mtk_dma_prefetch(sc);

	mtk_write(sc, MTK_WFDMA0_RST_DTX_PTR, ~0u);
	mtk_write(sc, MTK_WFDMA0_PRI_DLY_INT_CFG0, 0);

	config = mtk_read(sc, MTK_WFDMA0_GLO_CFG);
	config &= ~MTK_DMA_SIZE_MASK;
	config |= MTK_TX_WB_DDONE | MTK_FIFO_DIS_CHECK | MTK_FIFO_LITTLE_ENDIAN
		| MTK_RX_WB_DDONE | MTK_CSR_DISP_BASE_PTR_CHAIN_EN
		| MTK_OMIT_RX_INFO_PFET2 | MTK_OMIT_TX_INFO | MTK_CLK_GAT_DIS
		| (3u << MTK_DMA_SIZE_SHIFT);
	mtk_write(sc, MTK_WFDMA0_GLO_CFG, config);

	config |= MTK_TX_DMA_EN | MTK_RX_DMA_EN;
	mtk_write(sc, MTK_WFDMA0_GLO_CFG, config);

	mtk_write(sc, MTK_WFDMA_DUMMY_CR,
		mtk_read(sc, MTK_WFDMA_DUMMY_CR) | MTK_WFDMA_NEED_REINIT);
	mtk_write(sc, MTK_MCU2HOST_SW_INT_ENA,
		mtk_read(sc, MTK_MCU2HOST_SW_INT_ENA) | MTK_MCU_CMD_WAKE_RX_PCIE);
	mtk_write(sc, MTK_WFDMA0_HOST_INT_ENA, MTK_INTERRUPTS_ALL);
}


int
mtk_dma_setup(struct mtk_softc* sc)
{
	int error;

	mtk_dma_disable(sc);

	error = mtk_ring_init(sc, &sc->sc_fwq, MTK_TX_RING_BASE, MTK_TXQ_FWDL,
		MTK_TX_FWDL_RING_COUNT, "the firmware ring");
	if (error != 0)
		goto fail;

	error = mtk_ring_init(sc, &sc->sc_cmdq, MTK_TX_RING_BASE, MTK_TXQ_MCU_WM,
		MTK_TX_MCU_RING_COUNT, "the command ring");
	if (error != 0)
		goto fail;

	error = mtk_ring_init(sc, &sc->sc_txq, MTK_TX_RING_BASE, MTK_TXQ_BAND0,
		MTK_TX_RING_COUNT, "the transmit ring");
	if (error != 0)
		goto fail;

	error = mtk_rx_ring_init(sc, &sc->sc_eventq, MTK_RX_EVENT_RING_BASE,
		MTK_RXQ_MCU, MTK_RX_EVENT_RING_COUNT, "the event ring");
	if (error != 0)
		goto fail;

	/* The part answers on one ring while it is being given its firmware and
	 * on another once that firmware is running, and nothing says exactly
	 * when it changes over.
	 */
	error = mtk_rx_ring_init(sc, &sc->sc_lateq, MTK_RX_LATE_RING_BASE,
		MTK_RXQ_MCU, MTK_RX_LATE_RING_COUNT, "the late event ring");
	if (error != 0)
		goto fail;

	error = mtk_rx_ring_init(sc, &sc->sc_dataq, MTK_RX_DATA_RING_BASE,
		MTK_RXQ_MCU, MTK_RX_DATA_RING_COUNT, "the data ring");
	if (error != 0)
		goto fail;

	mtk_dma_enable(sc);
	sc->sc_rings = 1;

	device_printf(sc->sc_dev, "rings ready, engine at %#x\n",
		mtk_read(sc, MTK_WFDMA0_GLO_CFG));

	return 0;

fail:
	mtk_dma_teardown(sc);
	return error;
}


void
mtk_dma_teardown(struct mtk_softc* sc)
{
	if (sc->sc_mem != NULL)
		mtk_dma_disable(sc);

	mtk_ring_free(&sc->sc_fwq);
	mtk_ring_free(&sc->sc_cmdq);
	mtk_ring_free(&sc->sc_txq);
	mtk_ring_free(&sc->sc_eventq);
	mtk_ring_free(&sc->sc_lateq);
	mtk_ring_free(&sc->sc_dataq);
	sc->sc_rings = 0;
}
