/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Broadcom GENET v5, the Ethernet controller of the BCM2711 (Raspberry
	Pi 4), with its BCM54213PE RGMII PHY on the built-in MDIO bus.

	Like U-Boot's driver this one only uses the default queue (ring 16) in
	each direction, with all 256 descriptors. Packets are copied to and from
	two buffer areas that are kept out of the CPU cache, so no cache
	maintenance is needed around DMA. The controller filters nothing
	(promiscuous); the network stack sorts the packets out.

	Register layout and bring-up order follow Linux' bcmgenet.c and bcmmii.c. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <net/if_media.h>
#include <sys/sockio.h>

#include <ethernet.h>
#include <kernel.h>
#include <lock.h>
#include <net_buffer.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include "ether_driver.h"


//#define TRACE_GENET
#ifdef TRACE_GENET
#	define TRACE(x...) dprintf("bcm_genet: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("bcm_genet: " x)
#define ERROR(x...)	dprintf("bcm_genet: " x)


#define GENET_DRIVER_MODULE_NAME	"drivers/network/bcm_genet/driver_v1"
#define GENET_DEVICE_MODULE_NAME	"drivers/network/bcm_genet/device_v1"
#define GENET_DEVICE_ID_GENERATOR	"bcm_genet/device_id"


// register blocks
#define SYS_REV_CTRL			0x0000
#define SYS_PORT_CTRL			0x0004
#define  PORT_MODE_EXT_GPHY		3
#define SYS_RBUF_FLUSH_CTRL		0x0008
#define SYS_TBUF_FLUSH_CTRL		0x000c

#define EXT_RGMII_OOB_CTRL		0x008c
#define  RGMII_LINK				(1 << 4)
#define  OOB_DISABLE			(1 << 5)
#define  RGMII_MODE_EN			(1 << 6)
#define  ID_MODE_DIS			(1 << 16)

#define INTRL2_0				0x0200
#define INTRL2_1				0x0240
#define  INTRL2_CPU_STAT		0x00
#define  INTRL2_CPU_CLEAR		0x08
#define  INTRL2_CPU_MASK_STATUS	0x0c
#define  INTRL2_CPU_MASK_SET	0x10
#define  INTRL2_CPU_MASK_CLEAR	0x14
#define  IRQ_RXDMA_DONE			(1 << 13)
#define  IRQ_TXDMA_DONE			(1 << 16)

#define RBUF_CTRL				0x0300
#define  RBUF_ALIGN_2B			(1 << 1)
#define RBUF_TBUF_SIZE_CTRL		0x03b4

#define UMAC_CMD				0x0808
#define  CMD_TX_EN				(1 << 0)
#define  CMD_RX_EN				(1 << 1)
#define  CMD_SPEED_SHIFT		2
#define  CMD_SPEED_MASK			(3 << CMD_SPEED_SHIFT)
#define  CMD_PROMISC			(1 << 4)
#define  CMD_HD_EN				(1 << 10)
#define  CMD_SW_RESET			(1 << 13)
#define  CMD_LCL_LOOP_EN		(1 << 15)
#define UMAC_MAC0				0x080c
#define UMAC_MAC1				0x0810
#define UMAC_MAX_FRAME_LEN		0x0814
#define UMAC_TX_FLUSH			0x0b34
#define UMAC_MIB_CTRL			0x0d80
#define UMAC_MDIO_CMD			0x0e14
#define  MDIO_START_BUSY		(1 << 29)
#define  MDIO_READ_FAIL			(1 << 28)
#define  MDIO_RD				(2 << 26)
#define  MDIO_WR				(1 << 26)
#define  MDIO_PHY_SHIFT			21
#define  MDIO_REG_SHIFT			16

// DMA: 256 descriptors of three words, then the ring registers of 17 rings,
// then the registers of the DMA engine
#define RDMA_BASE				0x2000
#define TDMA_BASE				0x4000
#define DESCRIPTOR_COUNT		256
#define DESCRIPTOR_SIZE			12
#define DEFAULT_RING			16
#define DMA_RINGS				(DESCRIPTOR_COUNT * DESCRIPTOR_SIZE)
#define DMA_RING(ring)			(DMA_RINGS + 0x40 * (ring))
#define DMA_ENGINE				(DMA_RINGS + 0x40 * 17)

#define DESC_LENGTH_STATUS		0x00
#define DESC_ADDRESS_LOW		0x04
#define DESC_ADDRESS_HIGH		0x08
#define  DESC_LENGTH_SHIFT		16
#define  DESC_LENGTH_MASK		0x0fff
#define  DESC_EOP				0x4000
#define  DESC_SOP				0x2000
#define  DESC_TX_APPEND_CRC		0x0040
#define  DESC_TX_QTAG_MASK		(0x3f << 7)
#define  DESC_RX_ERRORS			0x0017
	// overrun, CRC, receive error, too long

// ring registers; the names are the transmit side's, the receive side uses
// the same slots the other way around
#define RING_TX_READ_PTR		0x00
#define RING_RX_WRITE_PTR		0x00
#define RING_TX_CONS_INDEX		0x08
#define RING_RX_PROD_INDEX		0x08
#define RING_TX_PROD_INDEX		0x0c
#define RING_RX_CONS_INDEX		0x0c
#define RING_BUF_SIZE			0x10
#define RING_START_ADDR			0x14
#define RING_END_ADDR			0x1c
#define RING_MBUF_DONE_THRESH	0x24
#define RING_TX_FLOW_PERIOD		0x28
#define RING_RX_XON_XOFF_THRESH	0x28
#define RING_TX_WRITE_PTR		0x2c
#define RING_RX_READ_PTR		0x2c

#define DMA_RING_CFG			0x00
#define DMA_CTRL				0x04
#define  DMA_EN					(1 << 0)
#define  DMA_RING_BUF_EN_SHIFT	1
#define DMA_SCB_BURST_SIZE		0x0c

#define DMA_INDEX_MASK			0xffff
#define BUFFER_SIZE				2048
#define RX_OFFSET				2
#define MAX_FRAME_SIZE			1536

// PHY registers (Broadcom BCM54xx)
#define MII_BMCR				0x00
#define  BMCR_RESET				0x8000
#define  BMCR_ANENABLE			0x1000
#define  BMCR_ANRESTART			0x0200
#define MII_BMSR				0x01
#define  BMSR_LSTATUS			0x0004
#define MII_PHYSID1				0x02
#define MII_PHYSID2				0x03
#define MII_ADVERTISE			0x04
#define MII_LPA					0x05
#define MII_CTRL1000			0x09
#define MII_STAT1000			0x0a
#define MII_BCM54XX_AUX_CTL		0x18
#define MII_BCM54XX_SHD			0x1c


struct genet_info {
	device_node*	node;
	uint64			registersBase;
	uint64			registersSize;
	uint32			interrupt;
	uint8			macAddress[6];
	uint32			phyAddress;
	bool			rxDelayInPhy;

	area_id			registerArea;
	volatile uint8*	registers;
	bool			interruptInstalled;

	area_id			bufferArea;
	uint8*			buffers;
		// DESCRIPTOR_COUNT receive buffers, then as many for transmission
	phys_addr_t		buffersAddress;

	mutex			rxLock;
	mutex			txLock;
	sem_id			rxSemaphore;
	sem_id			txSemaphore;
	uint32			rxIndex;	// consumer
	uint32			txIndex;	// producer
	uint32			txCleaned;

	int32			openCount;
	bool			nonblocking;

	thread_id		linkThread;
	bool			stopping;
	sem_id			linkChangeSemaphore;
	bool			linkUp;
	uint32			linkSpeed;	// Mbit/s
	bool			fullDuplex;
};


static device_manager_info* sDeviceManager;
static net_buffer_module_info* sBufferModule;


static inline uint32
read32(genet_info* info, uint32 reg)
{
	return *(volatile uint32*)(info->registers + reg);
}


static inline void
write32(genet_info* info, uint32 reg, uint32 value)
{
	*(volatile uint32*)(info->registers + reg) = value;
}


//	#pragma mark - PHY


static status_t
mdio_wait(genet_info* info)
{
	bigtime_t timeout = system_time() + 20000;
	while ((read32(info, UMAC_MDIO_CMD) & MDIO_START_BUSY) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(20);
	}
	return B_OK;
}


static int32
mdio_read(genet_info* info, uint32 reg)
{
	uint32 command = MDIO_RD | info->phyAddress << MDIO_PHY_SHIFT
		| reg << MDIO_REG_SHIFT;
	write32(info, UMAC_MDIO_CMD, command);
	write32(info, UMAC_MDIO_CMD, command | MDIO_START_BUSY);
	if (mdio_wait(info) != B_OK)
		return B_TIMED_OUT;

	uint32 value = read32(info, UMAC_MDIO_CMD);
	if ((value & MDIO_READ_FAIL) != 0)
		return B_IO_ERROR;
	return value & 0xffff;
}


static status_t
mdio_write(genet_info* info, uint32 reg, uint16 value)
{
	uint32 command = MDIO_WR | info->phyAddress << MDIO_PHY_SHIFT
		| reg << MDIO_REG_SHIFT | value;
	write32(info, UMAC_MDIO_CMD, command);
	write32(info, UMAC_MDIO_CMD, command | MDIO_START_BUSY);
	return mdio_wait(info);
}


static status_t
phy_init(genet_info* info)
{
	int32 id1 = mdio_read(info, MII_PHYSID1);
	int32 id2 = mdio_read(info, MII_PHYSID2);
	if (id1 < 0 || id2 < 0 || id1 == 0xffff) {
		ERROR("no PHY at MDIO address %" B_PRIu32 "\n", info->phyAddress);
		return B_DEVICE_NOT_FOUND;
	}
	INFO("PHY %04" B_PRIx32 ":%04" B_PRIx32 " at MDIO address %" B_PRIu32 "\n",
		id1, id2, info->phyAddress);

	mdio_write(info, MII_BMCR, BMCR_RESET);
	bigtime_t timeout = system_time() + 500000;
	while ((mdio_read(info, MII_BMCR) & BMCR_RESET) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(1000);
	}

	// RGMII clock delays ("phy-mode"): the receive clock delay in the PHY
	// (auxiliary control, shadow 7, bit 8), no transmit clock delay (shadow
	// register 0x1c, selector 3, bit 9).
	mdio_write(info, MII_BCM54XX_AUX_CTL, 0x7007);
	int32 value = mdio_read(info, MII_BCM54XX_AUX_CTL);
	if (value >= 0) {
		if (info->rxDelayInPhy)
			value |= 1 << 8;
		else
			value &= ~(1 << 8);
		mdio_write(info, MII_BCM54XX_AUX_CTL, 0x8000 | (value & 0x7ff8) | 0x7);
	}

	mdio_write(info, MII_BCM54XX_SHD, 0x03 << 10);
	value = mdio_read(info, MII_BCM54XX_SHD);
	if (value >= 0) {
		value &= 0x3ff & ~(1 << 9);
		mdio_write(info, MII_BCM54XX_SHD, 0x8000 | 0x03 << 10 | value);
	}

	// advertise everything up to 1000BASE-T full duplex, with pause
	mdio_write(info, MII_ADVERTISE, 0x0001 | 0x01e0 | 0x0400);
	mdio_write(info, MII_CTRL1000, 0x0200);
	mdio_write(info, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
	return B_OK;
}


static void
update_link(genet_info* info)
{
	// the link bit latches a failure: read twice
	mdio_read(info, MII_BMSR);
	int32 status = mdio_read(info, MII_BMSR);
	bool up = status >= 0 && (status & BMSR_LSTATUS) != 0;

	uint32 speed = 0;
	bool fullDuplex = false;
	if (up) {
		int32 status1000 = mdio_read(info, MII_STAT1000);
		int32 partner = mdio_read(info, MII_LPA);
		int32 own = mdio_read(info, MII_ADVERTISE);
		if (status1000 < 0 || partner < 0 || own < 0)
			return;

		uint32 common = partner & own;
		if ((status1000 & 0x0800) != 0) {
			speed = 1000;
			fullDuplex = true;
		} else if ((common & 0x0100) != 0) {
			speed = 100;
			fullDuplex = true;
		} else if ((common & 0x0080) != 0) {
			speed = 100;
		} else if ((common & 0x0040) != 0) {
			speed = 10;
			fullDuplex = true;
		} else
			speed = 10;
	}

	if (up == info->linkUp && speed == info->linkSpeed
		&& fullDuplex == info->fullDuplex) {
		return;
	}

	if (up) {
		// tell the MAC what the PHY negotiated
		uint32 oob = read32(info, EXT_RGMII_OOB_CTRL);
		oob &= ~OOB_DISABLE;
		oob |= RGMII_LINK | RGMII_MODE_EN;
		write32(info, EXT_RGMII_OOB_CTRL, oob);

		uint32 command = read32(info, UMAC_CMD);
		command &= ~(CMD_SPEED_MASK | CMD_HD_EN);
		command |= (speed == 1000 ? 2 : speed == 100 ? 1 : 0) << CMD_SPEED_SHIFT;
		if (!fullDuplex)
			command |= CMD_HD_EN;
		write32(info, UMAC_CMD, command);
	}

	info->linkUp = up;
	info->linkSpeed = speed;
	info->fullDuplex = fullDuplex;

	if (up) {
		INFO("link up, %" B_PRIu32 " Mbit/s %s duplex\n", speed,
			fullDuplex ? "full" : "half");
	} else
		INFO("link down\n");

	if (info->linkChangeSemaphore >= 0)
		release_sem_etc(info->linkChangeSemaphore, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
link_thread(void* data)
{
	genet_info* info = (genet_info*)data;
	while (!info->stopping) {
		update_link(info);
		snooze(1000000);
	}
	return B_OK;
}


//	#pragma mark - controller


static int32
genet_interrupt(void* data)
{
	genet_info* info = (genet_info*)data;

	uint32 status = read32(info, INTRL2_0 + INTRL2_CPU_STAT)
		& ~read32(info, INTRL2_0 + INTRL2_CPU_MASK_STATUS);
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;

	write32(info, INTRL2_0 + INTRL2_CPU_CLEAR, status);

	if ((status & IRQ_RXDMA_DONE) != 0)
		release_sem_etc(info->rxSemaphore, 1, B_DO_NOT_RESCHEDULE);
	if ((status & IRQ_TXDMA_DONE) != 0)
		release_sem_etc(info->txSemaphore, 1, B_DO_NOT_RESCHEDULE);

	return B_INVOKE_SCHEDULER;
}


static void
genet_stop(genet_info* info)
{
	write32(info, INTRL2_0 + INTRL2_CPU_MASK_SET, 0xffffffff);
	write32(info, INTRL2_0 + INTRL2_CPU_CLEAR, 0xffffffff);
	write32(info, INTRL2_1 + INTRL2_CPU_MASK_SET, 0xffffffff);
	write32(info, INTRL2_1 + INTRL2_CPU_CLEAR, 0xffffffff);

	write32(info, UMAC_CMD, read32(info, UMAC_CMD) & ~(CMD_TX_EN | CMD_RX_EN));

	const uint32 enable = DMA_EN | 1 << (DEFAULT_RING + DMA_RING_BUF_EN_SHIFT);
	write32(info, TDMA_BASE + DMA_ENGINE + DMA_CTRL,
		read32(info, TDMA_BASE + DMA_ENGINE + DMA_CTRL) & ~enable);
	write32(info, RDMA_BASE + DMA_ENGINE + DMA_CTRL,
		read32(info, RDMA_BASE + DMA_ENGINE + DMA_CTRL) & ~enable);

	write32(info, UMAC_TX_FLUSH, 1);
	spin(10);
	write32(info, UMAC_TX_FLUSH, 0);
}


static void
genet_start(genet_info* info)
{
	genet_stop(info);

	// reset the MAC
	write32(info, SYS_RBUF_FLUSH_CTRL, 1 << 1);
	spin(10);
	write32(info, SYS_RBUF_FLUSH_CTRL, 0);
	spin(10);

	write32(info, UMAC_CMD, 0);
	write32(info, UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN);
	spin(2);
	write32(info, UMAC_CMD, 0);

	// clear the counters
	write32(info, UMAC_MIB_CTRL, 0x7);
	write32(info, UMAC_MIB_CTRL, 0);

	write32(info, UMAC_MAX_FRAME_LEN, MAX_FRAME_SIZE);

	// two bytes in front of each received frame align the IP header
	write32(info, RBUF_CTRL, read32(info, RBUF_CTRL) | RBUF_ALIGN_2B);
	write32(info, RBUF_TBUF_SIZE_CTRL, 1);

	// an external RGMII PHY; who delays the clocks is the PHY's business
	write32(info, SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);
	uint32 oob = read32(info, EXT_RGMII_OOB_CTRL);
	oob &= ~ID_MODE_DIS;
	oob |= RGMII_MODE_EN;
	write32(info, EXT_RGMII_OOB_CTRL, oob);

	const uint8* mac = info->macAddress;
	write32(info, UMAC_MAC0, mac[0] << 24 | mac[1] << 16 | mac[2] << 8 | mac[3]);
	write32(info, UMAC_MAC1, mac[4] << 8 | mac[5]);

	// the receive ring: every descriptor has its buffer for good
	for (uint32 i = 0; i < DESCRIPTOR_COUNT; i++) {
		uint64 address = info->buffersAddress + (uint64)i * BUFFER_SIZE;
		uint32 descriptor = RDMA_BASE + i * DESCRIPTOR_SIZE;
		write32(info, descriptor + DESC_ADDRESS_LOW, (uint32)address);
		write32(info, descriptor + DESC_ADDRESS_HIGH, address >> 32);
		write32(info, descriptor + DESC_LENGTH_STATUS, 0);
	}

	uint32 ring = RDMA_BASE + DMA_RING(DEFAULT_RING);
	write32(info, RDMA_BASE + DMA_ENGINE + DMA_SCB_BURST_SIZE, 0x08);
	write32(info, ring + RING_START_ADDR, 0);
	write32(info, ring + RING_RX_READ_PTR, 0);
	write32(info, ring + RING_RX_WRITE_PTR, 0);
	write32(info, ring + RING_END_ADDR,
		DESCRIPTOR_COUNT * DESCRIPTOR_SIZE / 4 - 1);
	// The producer index survives a reset of the MAC: start where it is.
	write32(info, ring + RING_RX_PROD_INDEX, 0);
	info->rxIndex = read32(info, ring + RING_RX_PROD_INDEX) & DMA_INDEX_MASK;
	write32(info, ring + RING_RX_CONS_INDEX, info->rxIndex);
	write32(info, ring + RING_BUF_SIZE,
		DESCRIPTOR_COUNT << 16 | BUFFER_SIZE);
	write32(info, ring + RING_RX_XON_XOFF_THRESH,
		5 << 16 | DESCRIPTOR_COUNT >> 4);
	write32(info, ring + RING_MBUF_DONE_THRESH, 1);
	write32(info, RDMA_BASE + DMA_ENGINE + DMA_RING_CFG, 1 << DEFAULT_RING);

	// the transmit ring
	ring = TDMA_BASE + DMA_RING(DEFAULT_RING);
	write32(info, TDMA_BASE + DMA_ENGINE + DMA_SCB_BURST_SIZE, 0x08);
	write32(info, ring + RING_START_ADDR, 0);
	write32(info, ring + RING_TX_READ_PTR, 0);
	write32(info, ring + RING_TX_WRITE_PTR, 0);
	write32(info, ring + RING_END_ADDR,
		DESCRIPTOR_COUNT * DESCRIPTOR_SIZE / 4 - 1);
	write32(info, ring + RING_TX_CONS_INDEX, 0);
	info->txIndex = read32(info, ring + RING_TX_CONS_INDEX) & DMA_INDEX_MASK;
	info->txCleaned = info->txIndex;
	write32(info, ring + RING_TX_PROD_INDEX, info->txIndex);
	write32(info, ring + RING_MBUF_DONE_THRESH, 1);
	write32(info, ring + RING_TX_FLOW_PERIOD, 0);
	write32(info, ring + RING_BUF_SIZE,
		DESCRIPTOR_COUNT << 16 | BUFFER_SIZE);
	write32(info, TDMA_BASE + DMA_ENGINE + DMA_RING_CFG, 1 << DEFAULT_RING);

	const uint32 enable = DMA_EN | 1 << (DEFAULT_RING + DMA_RING_BUF_EN_SHIFT);
	write32(info, RDMA_BASE + DMA_ENGINE + DMA_CTRL, enable);
	write32(info, TDMA_BASE + DMA_ENGINE + DMA_CTRL, enable);

	info->linkUp = false;
	info->linkSpeed = 0;
	phy_init(info);

	write32(info, UMAC_CMD, read32(info, UMAC_CMD) | CMD_TX_EN | CMD_RX_EN
		| CMD_PROMISC | 2 << CMD_SPEED_SHIFT);

	write32(info, INTRL2_0 + INTRL2_CPU_MASK_CLEAR,
		IRQ_RXDMA_DONE | IRQ_TXDMA_DONE);
}


static status_t
genet_receive(genet_info* info, net_buffer** _buffer)
{
	uint32 ring = RDMA_BASE + DMA_RING(DEFAULT_RING);

	while (true) {
		MutexLocker locker(info->rxLock);

		uint32 produced = read32(info, ring + RING_RX_PROD_INDEX)
			& DMA_INDEX_MASK;
		if (produced != info->rxIndex) {
			uint32 slot = info->rxIndex % DESCRIPTOR_COUNT;
			uint32 status = read32(info,
				RDMA_BASE + slot * DESCRIPTOR_SIZE + DESC_LENGTH_STATUS);
			uint32 length = (status >> DESC_LENGTH_SHIFT) & DESC_LENGTH_MASK;

			net_buffer* buffer = NULL;
			bool good = (status & (DESC_SOP | DESC_EOP)) == (DESC_SOP | DESC_EOP)
				&& (status & DESC_RX_ERRORS) == 0
				&& length > RX_OFFSET + ETHER_HEADER_LENGTH
				&& length <= BUFFER_SIZE;
			if (good) {
				buffer = sBufferModule->create(0);
				if (buffer != NULL && sBufferModule->append(buffer,
						info->buffers + slot * BUFFER_SIZE + RX_OFFSET,
						length - RX_OFFSET) != B_OK) {
					sBufferModule->free(buffer);
					buffer = NULL;
				}
			}

			info->rxIndex = (info->rxIndex + 1) & DMA_INDEX_MASK;
			write32(info, ring + RING_RX_CONS_INDEX, info->rxIndex);

			if (buffer != NULL) {
				*_buffer = buffer;
				return B_OK;
			}
			continue;
		}

		locker.Unlock();

		if (info->nonblocking)
			return B_WOULD_BLOCK;

		status_t status = acquire_sem_etc(info->rxSemaphore, 1,
			B_CAN_INTERRUPT, 0);
		if (status != B_OK)
			return status;
	}
}


static status_t
genet_send(genet_info* info, net_buffer* buffer)
{
	size_t length = buffer->size;
	if (length > MAX_FRAME_SIZE)
		return B_BAD_DATA;

	uint32 ring = TDMA_BASE + DMA_RING(DEFAULT_RING);

	MutexLocker locker(info->txLock);
	while (true) {
		uint32 consumed = read32(info, ring + RING_TX_CONS_INDEX)
			& DMA_INDEX_MASK;
		uint32 pending = (info->txIndex - consumed) & DMA_INDEX_MASK;
		if (pending < DESCRIPTOR_COUNT - 1)
			break;

		locker.Unlock();
		if (info->nonblocking)
			return B_WOULD_BLOCK;

		status_t status = acquire_sem_etc(info->txSemaphore, 1,
			B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 100000);
		if (status != B_OK && status != B_TIMED_OUT)
			return status;
		locker.Lock();
	}

	uint32 slot = info->txIndex % DESCRIPTOR_COUNT;
	uint8* data = info->buffers + (DESCRIPTOR_COUNT + slot) * BUFFER_SIZE;
	if (sBufferModule->read(buffer, 0, data, length) != B_OK)
		return B_BAD_DATA;
	if (length < 60) {
		memset(data + length, 0, 60 - length);
		length = 60;
	}
	memory_full_barrier();

	uint64 address = info->buffersAddress
		+ (uint64)(DESCRIPTOR_COUNT + slot) * BUFFER_SIZE;
	uint32 descriptor = TDMA_BASE + slot * DESCRIPTOR_SIZE;
	write32(info, descriptor + DESC_ADDRESS_LOW, (uint32)address);
	write32(info, descriptor + DESC_ADDRESS_HIGH, address >> 32);
	write32(info, descriptor + DESC_LENGTH_STATUS,
		length << DESC_LENGTH_SHIFT | DESC_TX_QTAG_MASK | DESC_TX_APPEND_CRC
			| DESC_SOP | DESC_EOP);

	info->txIndex = (info->txIndex + 1) & DMA_INDEX_MASK;
	write32(info, ring + RING_TX_PROD_INDEX, info->txIndex);
	locker.Unlock();

	sBufferModule->free(buffer);
	return B_OK;
}


//	#pragma mark - device


static status_t
genet_init_device(void* _info, void** _cookie)
{
	genet_info* info = (genet_info*)_info;

	info->registerArea = map_physical_memory("bcm_genet registers",
		info->registersBase, info->registersSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&info->registers);
	if (info->registerArea < 0)
		return info->registerArea;

	uint32 revision = read32(info, SYS_REV_CTRL);
	uint32 major = (revision >> 24) & 0x0f;
	if (major != 6) {
		// version 5 identifies itself as 6
		ERROR("not a GENET v5 (revision register %#" B_PRIx32 ")\n", revision);
		delete_area(info->registerArea);
		return B_NOT_SUPPORTED;
	}

	// receive and transmit buffers, out of the CPU cache
	size_t size = 2 * DESCRIPTOR_COUNT * BUFFER_SIZE;
	info->bufferArea = create_area("bcm_genet buffers", (void**)&info->buffers,
		B_ANY_KERNEL_ADDRESS, size, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (info->bufferArea < 0) {
		delete_area(info->registerArea);
		return info->bufferArea;
	}

	physical_entry entry;
	get_memory_map(info->buffers, size, &entry, 1);
	info->buffersAddress = entry.address;

	memset(info->buffers, 0, size);
	for (addr_t line = (addr_t)info->buffers;
			line < (addr_t)info->buffers + size; line += 64) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();
	vm_set_area_memory_type(info->bufferArea, info->buffersAddress,
		B_WRITE_COMBINING_MEMORY);

	mutex_init(&info->rxLock, "bcm_genet rx");
	mutex_init(&info->txLock, "bcm_genet tx");
	info->rxSemaphore = -1;
	info->txSemaphore = -1;
	info->linkChangeSemaphore = -1;
	info->linkThread = -1;

	genet_stop(info);

	INFO("GENET revision %#" B_PRIx32 " at %#" B_PRIx64 ", interrupt %"
		B_PRIu32 ", %02x:%02x:%02x:%02x:%02x:%02x\n", revision,
		info->registersBase, info->interrupt, info->macAddress[0],
		info->macAddress[1], info->macAddress[2], info->macAddress[3],
		info->macAddress[4], info->macAddress[5]);

	*_cookie = info;
	return B_OK;
}


static void
genet_uninit_device(void* cookie)
{
	genet_info* info = (genet_info*)cookie;

	genet_stop(info);
	mutex_destroy(&info->rxLock);
	mutex_destroy(&info->txLock);
	delete_area(info->bufferArea);
	delete_area(info->registerArea);
}


static status_t
genet_open(void* _info, const char* path, int openMode, void** _cookie)
{
	genet_info* info = (genet_info*)_info;

	if (atomic_add(&info->openCount, 1) != 0) {
		atomic_add(&info->openCount, -1);
		return B_BUSY;
	}

	info->nonblocking = (openMode & O_NONBLOCK) != 0;
	info->stopping = false;
	info->rxSemaphore = create_sem(0, "bcm_genet rx");
	info->txSemaphore = create_sem(0, "bcm_genet tx");

	status_t status = install_io_interrupt_handler(info->interrupt,
		genet_interrupt, info, 0);
	if (status != B_OK) {
		delete_sem(info->rxSemaphore);
		delete_sem(info->txSemaphore);
		atomic_add(&info->openCount, -1);
		return status;
	}
	info->interruptInstalled = true;

	genet_start(info);

	info->linkThread = spawn_kernel_thread(link_thread, "bcm_genet link",
		B_NORMAL_PRIORITY, info);
	resume_thread(info->linkThread);

	*_cookie = info;
	return B_OK;
}


static status_t
genet_close(void* cookie)
{
	genet_info* info = (genet_info*)cookie;

	info->stopping = true;
	status_t result;
	wait_for_thread(info->linkThread, &result);
	info->linkThread = -1;

	genet_stop(info);

	if (info->interruptInstalled) {
		remove_io_interrupt_handler(info->interrupt, genet_interrupt, info);
		info->interruptInstalled = false;
	}

	delete_sem(info->rxSemaphore);
	delete_sem(info->txSemaphore);
	info->rxSemaphore = info->txSemaphore = -1;
	return B_OK;
}


static status_t
genet_free(void* cookie)
{
	genet_info* info = (genet_info*)cookie;
	atomic_add(&info->openCount, -1);
	return B_OK;
}


static status_t
genet_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	genet_info* info = (genet_info*)cookie;

	switch (op) {
		case ETHER_GETADDR:
			return user_memcpy(buffer, info->macAddress,
				sizeof(info->macAddress));

		case ETHER_INIT:
			return B_OK;

		case ETHER_GETFRAMESIZE:
		{
			uint32 frameSize = ETHER_MAX_FRAME_SIZE;
			return user_memcpy(buffer, &frameSize, sizeof(frameSize));
		}

		case ETHER_SETPROMISC:
			// the controller does not filter at all
			return B_OK;

		case ETHER_NONBLOCK:
		{
			int32 value;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			info->nonblocking = value != 0;
			return B_OK;
		}

		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			return B_OK;

		case ETHER_SET_LINK_STATE_SEM:
		{
			sem_id semaphore;
			if (user_memcpy(&semaphore, buffer, sizeof(semaphore)) != B_OK)
				return B_BAD_ADDRESS;
			info->linkChangeSemaphore = semaphore;
			return B_OK;
		}

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			state.media = IFM_ETHER;
			if (info->linkUp) {
				state.media |= IFM_ACTIVE
					| (info->fullDuplex ? IFM_FULL_DUPLEX : IFM_HALF_DUPLEX)
					| (info->linkSpeed == 1000 ? IFM_1000_T
						: info->linkSpeed == 100 ? IFM_100_TX : IFM_10_T);
			}
			state.speed = (uint64)info->linkSpeed * 1000000;
			state.quality = 1000;
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case ETHER_SEND_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return genet_send(info, (net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return genet_receive(info, (net_buffer**)buffer);
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static float
genet_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "brcm,bcm2711-genet-v5") != 0) {
		return 0.0f;
	}

	return 1.0f;
}


static status_t
genet_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "Broadcom GENET Ethernet"}},
		{}
	};

	return sDeviceManager->register_node(parent, GENET_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
genet_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	sDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	genet_info* info = (genet_info*)calloc(1, sizeof(genet_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;

	uint64 interrupt;
	if (!fdt->get_reg(device, 0, &info->registersBase, &info->registersSize)
		|| !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		free(info);
		return B_BAD_DATA;
	}
	info->interrupt = interrupt;

	int length;
	const void* property = fdt->get_prop(device, "local-mac-address", &length);
	if (property != NULL && length == 6)
		memcpy(info->macAddress, property, 6);
	else {
		// locally administered, so that the board at least gets on the net
		const uint8 fallback[6] = {0x02, 0xa1, 0x05, 0x00, 0x00, 0x01};
		memcpy(info->macAddress, fallback, 6);
	}

	// "rgmii-rxid": the PHY delays the receive clock; "rgmii": the board does
	property = fdt->get_prop(device, "phy-mode", &length);
	info->rxDelayInPhy = property != NULL
		&& (strcmp((const char*)property, "rgmii-rxid") == 0
			|| strcmp((const char*)property, "rgmii-id") == 0);

	// The Pi's boards all have the PHY at address 1 (the "reg" of the node
	// that "phy-handle" names).
	info->phyAddress = 1;

	*_cookie = info;
	return B_OK;
}


static void
genet_uninit_driver(void* cookie)
{
	free(cookie);
}


static status_t
genet_register_child_devices(void* cookie)
{
	genet_info* info = (genet_info*)cookie;

	int32 id = sDeviceManager->create_id(GENET_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/bcm_genet/%" B_PRId32, id);

	return sDeviceManager->publish_device(info->node, name,
		GENET_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule},
	{}
};

static device_module_info sGenetDevice = {
	{
		GENET_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	genet_init_device,
	genet_uninit_device,
	NULL,	// removed
	genet_open,
	genet_close,
	genet_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	genet_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sGenetDriver = {
	{
		GENET_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	genet_supports_device,
	genet_register_device,
	genet_init_driver,
	genet_uninit_driver,
	genet_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sGenetDriver,
	(module_info*)&sGenetDevice,
	NULL
};
