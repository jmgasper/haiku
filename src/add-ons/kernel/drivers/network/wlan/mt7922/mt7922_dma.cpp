/*
 * Memory the MT7922 can reach by itself.
 *
 * The card fetches its own descriptors and moves its own payloads, so the
 * memory it works from has to satisfy it rather than us: contiguous, because a
 * ring is one array as far as the hardware is concerned, and below four
 * gigabytes, because this part addresses no higher. Neither is the default for
 * an ordinary allocation, and neither failure announces itself - a ring placed
 * where the card cannot reach simply never advances.
 *
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

#include <util/AutoLock.h>
#include <vm/vm.h>

#include "mt7922.h"


#define TRACE(x...)	dprintf("mt7922: " x)
#define ERROR(x...)	dprintf("mt7922: " x)


/* Take a block of memory the card can fetch from directly, and say where it
 * is from the card's point of view as well as ours.
 */
status_t
mt7922_dma_alloc(const char* name, size_t size, mt7922_dma_mem* memory)
{
	size = ROUNDUP(size, B_PAGE_SIZE);

	memory->size = size;
	memory->area = create_area(name, &memory->address, B_ANY_KERNEL_ADDRESS,
		size, B_32_BIT_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);

	if (memory->area < B_OK) {
		ERROR("cannot find %" B_PRIuSIZE " bytes the card can reach: %s\n",
			size, strerror(memory->area));
		return memory->area;
	}

	physical_entry entry;
	status_t status = get_memory_map(memory->address, size, &entry, 1);
	if (status != B_OK) {
		ERROR("cannot find where %s ended up: %s\n", name, strerror(status));
		delete_area(memory->area);
		memory->area = -1;
		return status;
	}

	/* Contiguous was asked for, so one entry should cover all of it. If it
	 * does not, the card would walk off the end of the first piece.
	 */
	if (entry.size < size) {
		ERROR("%s came back in pieces: %" B_PRIuSIZE " of %" B_PRIuSIZE "\n",
			name, entry.size, size);
		delete_area(memory->area);
		memory->area = -1;
		return B_NO_MEMORY;
	}

	memory->physical = entry.address;

	/* The card reaches no higher than four gigabytes, and an address above
	 * that would be quietly truncated into someone else's memory.
	 */
	if ((memory->physical + size) > 0x100000000ULL) {
		ERROR("%s is at %#" B_PRIxPHYSADDR ", which the card cannot reach\n",
			name, memory->physical);
		delete_area(memory->area);
		memory->area = -1;
		return B_BAD_VALUE;
	}

	memset(memory->address, 0, size);
	return B_OK;
}


void
mt7922_dma_free(mt7922_dma_mem* memory)
{
	if (memory->area >= B_OK)
		delete_area(memory->area);

	memory->area = -1;
	memory->address = NULL;
	memory->physical = 0;
	memory->size = 0;
}


/* Set a ring up and tell the card where it is.
 *
 * The order of these writes is the card's, not ours: the descriptors are made
 * ready first, then the indices are zeroed, then how many there are, and the
 * address of them last - because that write is what the card acts on.
 */
static status_t
mt7922_ring_init(mt7922_dev* device, mt7922_ring* ring, uint32 base,
	uint32 index, uint16 count, const char* name)
{
	status_t status = mt7922_dma_alloc(name, count * MT7922_DESC_SIZE,
		&ring->descriptors);
	if (status != B_OK)
		return status;

	ring->registers = base + index * MT_RING_SIZE;
	ring->count = count;

	/* A descriptor marked done is one the card has finished with, which is
	 * how an empty ring looks to it.
	 */
	mt7922_desc* descriptors = (mt7922_desc*)ring->descriptors.address;
	for (uint16 i = 0; i < count; i++)
		descriptors[i].ctrl = MT_DMA_CTL_DMA_DONE;

	mt7922_write32(device, ring->registers + MT_RING_CPU_INDEX, 0);
	mt7922_write32(device, ring->registers + MT_RING_DMA_INDEX, 0);
	mt7922_write32(device, ring->registers + MT_RING_COUNT, count);
	mt7922_write32(device, ring->registers + MT_RING_DESC_BASE,
		(uint32)ring->descriptors.physical);

	/* Start from wherever the card says it is rather than from zero. */
	uint32 where = mt7922_read32(device, ring->registers + MT_RING_DMA_INDEX);
	if (where >= count)
		where = 0;

	ring->head = where;
	ring->tail = where;

	TRACE("%s: %u descriptors at %#" B_PRIxPHYSADDR ", card is at %" B_PRIu32
		"\n", name, count, ring->descriptors.physical, where);

	return B_OK;
}


static void
mt7922_ring_free(mt7922_ring* ring)
{
	mt7922_dma_free(&ring->descriptors);
	ring->count = 0;
	ring->head = 0;
	ring->tail = 0;
}


/* Hand every descriptor of a receiving ring an empty buffer. Clearing the
 * done bit is what gives it to the card; it sets the bit again when it has
 * put something there.
 */
static void
mt7922_ring_fill(mt7922_ring* ring)
{
	mt7922_desc* descriptors = (mt7922_desc*)ring->descriptors.address;

	for (uint16 i = 0; i < ring->count; i++) {
		phys_addr_t buffer = ring->buffers.physical
			+ (phys_addr_t)i * MT7922_RX_BUFFER_SIZE;

		descriptors[i].buf0 = (uint32)buffer;
		descriptors[i].buf1 = 0;
		descriptors[i].info = 0;
		descriptors[i].ctrl = (uint32)MT7922_RX_BUFFER_SIZE
			<< MT_DMA_CTL_SD_LEN0_SHIFT;
	}

	ring->head = ring->count - 1;
}


static status_t
mt7922_rx_ring_init(mt7922_dev* device, mt7922_ring* ring, uint32 base,
	uint32 index, uint16 count, const char* name)
{
	status_t status = mt7922_dma_alloc(name, count * MT7922_DESC_SIZE,
		&ring->descriptors);
	if (status != B_OK)
		return status;

	status = mt7922_dma_alloc("mt7922 receive buffers",
		(size_t)count * MT7922_RX_BUFFER_SIZE, &ring->buffers);
	if (status != B_OK) {
		mt7922_dma_free(&ring->descriptors);
		return status;
	}

	ring->registers = base + index * MT_RING_SIZE;
	ring->count = count;

	mt7922_ring_fill(ring);

	mt7922_write32(device, ring->registers + MT_RING_CPU_INDEX, 0);
	mt7922_write32(device, ring->registers + MT_RING_DMA_INDEX, 0);
	mt7922_write32(device, ring->registers + MT_RING_COUNT, count);
	mt7922_write32(device, ring->registers + MT_RING_DESC_BASE,
		(uint32)ring->descriptors.physical);

	ring->tail = 0;

	/* Everything written before the card is told how far we have got has to
	 * have landed in memory first.
	 */
	memory_write_barrier();
	mt7922_write32(device, ring->registers + MT_RING_CPU_INDEX, ring->head);

	TRACE("%s: %u buffers at %#" B_PRIxPHYSADDR "\n", name, count,
		ring->buffers.physical);

	return B_OK;
}


/* Quiet the transfer engine before rearranging what it works from. */
static void
mt7922_dma_disable(mt7922_dev* device)
{
	uint32 config = mt7922_read32(device, MT_WFDMA0_GLO_CFG);

	config &= ~(MT_WFDMA0_TX_DMA_EN | MT_WFDMA0_RX_DMA_EN
		| MT_WFDMA0_CSR_DISP_BASE_PTR_CHAIN_EN | MT_WFDMA0_OMIT_TX_INFO
		| MT_WFDMA0_OMIT_RX_INFO | MT_WFDMA0_OMIT_RX_INFO_PFET2);
	mt7922_write32(device, MT_WFDMA0_GLO_CFG, config);

	for (int i = 0; i < 100; i++) {
		config = mt7922_read32(device, MT_WFDMA0_GLO_CFG);
		if ((config & (MT_WFDMA0_TX_DMA_BUSY | MT_WFDMA0_RX_DMA_BUSY)) == 0)
			break;
		snooze(1000);
	}

	/* The scheduler has opinions about which ring may send when. Nothing here
	 * wants them yet.
	 */
	uint32 scheduler = mt7922_read32(device, MT_DMASHDL_SW_CONTROL);
	mt7922_write32(device, MT_DMASHDL_SW_CONTROL,
		scheduler | MT_DMASHDL_BYPASS);

	uint32 extended = mt7922_read32(device, MT_WFDMA0_GLO_CFG_EXT0);
	mt7922_write32(device, MT_WFDMA0_GLO_CFG_EXT0,
		extended & ~MT_WFDMA0_CSR_TX_DMASHDL_EN);
}


/* Each ring gets a slice of the engine's own buffer to read ahead into. A
 * ring with no slice has nowhere to stage what it fetches, and the traffic
 * that needs staging is what stops - which is why sending can look healthy
 * while nothing ever comes back.
 */
static void
mt7922_dma_prefetch(mt7922_dev* device)
{
	const uint32 depth = 0x4;

	/* The ring ordinary traffic goes out on needs its own window like every
	 * other; giving it only a depth and no place to read ahead into leaves
	 * it unable to move anything.
	 */
	mt7922_write32(device, MT_WFDMA0_TX_RING_EXT_CTRL, (0x140u << 16) | depth);

	/* The two high-numbered outbound rings sit apart from the others. */
	mt7922_write32(device, MT_WFDMA0_TX_RING_EXT_CTRL + 0x40,
		(0x340u << 16) | depth);
	mt7922_write32(device, MT_WFDMA0_TX_RING_EXT_CTRL + 0x44,
		(0x380u << 16) | depth);

	mt7922_write32(device, MT_WFDMA0_RX_RING_EXT_CTRL, (0x0u << 16) | depth);
	mt7922_write32(device, MT_WFDMA0_RX_RING_EXT_CTRL + 0x08,
		(0x40u << 16) | depth);
	mt7922_write32(device, MT_WFDMA0_RX_RING_EXT_CTRL + 0x0c,
		(0x80u << 16) | depth);
	mt7922_write32(device, MT_WFDMA0_RX_RING_EXT_CTRL + 0x10,
		(0xc0u << 16) | depth);
	mt7922_write32(device, MT_WFDMA0_RX_RING_EXT_CTRL + 0x14,
		(0x100u << 16) | depth);
}


static void
mt7922_dma_enable(mt7922_dev* device)
{
	mt7922_dma_prefetch(device);

	mt7922_write32(device, MT_WFDMA0_RST_DTX_PTR, ~0u);
	mt7922_write32(device, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

	uint32 config = mt7922_read32(device, MT_WFDMA0_GLO_CFG);
	config &= ~MT_WFDMA0_DMA_SIZE_MASK;
	config |= MT_WFDMA0_TX_WB_DDONE
		| MT_WFDMA0_FIFO_DIS_CHECK
		| MT_WFDMA0_FIFO_LITTLE_ENDIAN
		| MT_WFDMA0_RX_WB_DDONE
		| MT_WFDMA0_CSR_DISP_BASE_PTR_CHAIN_EN
		| MT_WFDMA0_OMIT_RX_INFO_PFET2
		| MT_WFDMA0_OMIT_TX_INFO
		| MT_WFDMA0_CLK_GAT_DIS
		| (3u << MT_WFDMA0_DMA_SIZE_SHIFT);
	mt7922_write32(device, MT_WFDMA0_GLO_CFG, config);

	config |= MT_WFDMA0_TX_DMA_EN | MT_WFDMA0_RX_DMA_EN;
	mt7922_write32(device, MT_WFDMA0_GLO_CFG, config);

	uint32 dummy = mt7922_read32(device, MT_WFDMA_DUMMY_CR);
	mt7922_write32(device, MT_WFDMA_DUMMY_CR, dummy | MT_WFDMA_NEED_REINIT);

	/* Let the part's processor prod the host side awake when it has put
	 * something in a receiving ring.
	 */
	uint32 interrupt = mt7922_read32(device, MT_MCU2HOST_SW_INT_ENA);
	mt7922_write32(device, MT_MCU2HOST_SW_INT_ENA,
		interrupt | MT_MCU_CMD_WAKE_RX_PCIE);

	/* Nothing here waits on an interrupt - the rings are read by looking.
	 * These are set anyway because it is not certain from anywhere whether
	 * the mask only quietens the signal or also stops the engine bothering
	 * to write, and the difference costs one register to rule out.
	 */
	mt7922_write32(device, MT_WFDMA0_HOST_INT_ENA, MT7922_INTERRUPTS_ALL);
}


status_t
mt7922_dma_setup(mt7922_dev* device)
{
	TRACE("transfer engine starts at %#" B_PRIx32 "\n",
		mt7922_read32(device, MT_WFDMA0_GLO_CFG));

	mt7922_dma_disable(device);

	status_t status = mt7922_ring_init(device, &device->firmwareRing,
		MT_TX_RING_BASE, MT7922_TXQ_FWDL, MT7922_TX_FWDL_RING_SIZE,
		"mt7922 firmware ring");
	if (status != B_OK)
		return status;

	status = mt7922_ring_init(device, &device->commandRing, MT_TX_RING_BASE,
		MT7922_TXQ_MCU_WM, MT7922_TX_MCU_RING_SIZE, "mt7922 command ring");
	if (status != B_OK)
		goto fail;

	/* And one for frames of our own, which is a different ring from the one
	 * commands go out on.
	 */
	status = mt7922_ring_init(device, &device->transmitRing, MT_TX_RING_BASE,
		MT7922_TXQ_BAND0, MT7922_TX_RING_SIZE, "mt7922 transmit ring");
	if (status != B_OK)
		goto fail;

	status = mt7922_rx_ring_init(device, &device->eventRing,
		MT_RX_EVENT_RING_BASE, MT7922_RXQ_MCU_WM, MT7922_RX_MCU_RING_SIZE,
		"mt7922 event ring");
	if (status != B_OK)
		goto fail;

	/* The part stops answering on that ring once its firmware is running and
	 * starts answering on this one instead. Both are kept, because nothing
	 * says precisely when it changes over.
	 */
	status = mt7922_rx_ring_init(device, &device->lateEventRing,
		MT_RX_LATE_RING_BASE, MT7922_RXQ_MCU_WM, MT7922_RX_LATE_RING_SIZE,
		"mt7922 late event ring");
	if (status != B_OK)
		goto fail;

	status = mt7922_rx_ring_init(device, &device->dataRing,
		MT_RX_DATA_RING_BASE, MT7922_RXQ_BAND0, MT7922_RX_DATA_RING_SIZE,
		"mt7922 data ring");
	if (status != B_OK)
		goto fail;

	/* One buffer for the command being sent and one for the piece of firmware
	 * being sent. Both are reused, so each is refilled only once the card has
	 * finished reading the last thing put there.
	 */
	status = mt7922_dma_alloc("mt7922 command buffer", 2048,
		&device->commandBuffer);
	if (status != B_OK)
		goto fail;

	status = mt7922_dma_alloc("mt7922 firmware buffer", 4096,
		&device->firmwareBuffer);
	if (status != B_OK)
		goto fail;

	/* A frame of ours goes out as two pieces of memory: a description of it
	 * that sits on the ring, and the frame itself, which the card fetches
	 * from wherever that description points.
	 */
	status = mt7922_dma_alloc("mt7922 frame description", 256,
		&device->transmitHeader);
	if (status != B_OK)
		goto fail;

	status = mt7922_dma_alloc("mt7922 frame", 512, &device->transmitFrame);
	if (status != B_OK)
		goto fail;

	mt7922_dma_enable(device);

	TRACE("transfer engine now %#" B_PRIx32 "\n",
		mt7922_read32(device, MT_WFDMA0_GLO_CFG));

	device->ringsReady = true;
	return B_OK;

fail:
	mt7922_dma_teardown(device);
	return status;
}


void
mt7922_dma_teardown(mt7922_dev* device)
{
	if (device->registers != NULL)
		mt7922_dma_disable(device);

	mt7922_ring_free(&device->firmwareRing);
	mt7922_ring_free(&device->commandRing);
	mt7922_ring_free(&device->eventRing);
	mt7922_dma_free(&device->eventRing.buffers);
	mt7922_ring_free(&device->lateEventRing);
	mt7922_dma_free(&device->lateEventRing.buffers);
	mt7922_ring_free(&device->dataRing);
	mt7922_dma_free(&device->dataRing.buffers);
	mt7922_ring_free(&device->transmitRing);
	mt7922_dma_free(&device->commandBuffer);
	mt7922_dma_free(&device->firmwareBuffer);
	mt7922_dma_free(&device->transmitHeader);
	mt7922_dma_free(&device->transmitFrame);
	device->ringsReady = false;
}
