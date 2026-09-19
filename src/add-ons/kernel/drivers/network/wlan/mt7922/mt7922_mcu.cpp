/*
 * Talking to the MT7922's own processor, and handing it its firmware.
 *
 * NOT YET RUN. None of this has met the hardware - the workstation became
 * unreachable before it could. It is written from the protocol rather than
 * from observation, and the parts of that protocol which are easy to get
 * wrong are called out in comments where they arise.
 *
 * Everything the part is told goes over a ring as a packet: a sixty-four byte
 * header and then the command's own arguments. Everything it says back
 * arrives on another ring, headed by thirty-six bytes, and is matched to what
 * was asked by a sequence number that is never zero - because zero is what the
 * part uses for things it says unprompted.
 *
 * Firmware payload is the exception that catches people out. It carries no
 * header at all: the raw bytes go on a ring of their own and nothing is
 * expected back.
 *
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

#include <util/AutoLock.h>

#include "mt7922.h"


#define TRACE(x...)	dprintf("mt7922: " x)
#define ERROR(x...)	dprintf("mt7922: " x)


/* What the part's processor is being asked to do. */
#define MCU_CMD_TARGET_ADDRESS_LEN	0x01
#define MCU_CMD_FW_START		0x02
#define MCU_CMD_NIC_POWER_CTRL		0x04
#define MCU_CMD_PATCH_START		0x05
#define MCU_CMD_PATCH_FINISH		0x07
#define MCU_CMD_PATCH_SEM_CONTROL	0x10
#define MCU_CMD_FW_SCATTER		0xee

/* The header on an outbound command. */
#define MT_TXD0_TX_BYTES_MASK		0x0000ffff
#define MT_TX_TYPE_CMD			2
#define MT_TXD0_PKT_FMT_SHIFT		23
#define MT_TX_MCU_PORT_RX_Q0		0x20
#define MT_TXD0_Q_IDX_SHIFT		25
#define MT_TXD1_LONG_FORMAT		(1u << 31)
#define MT_HDR_FORMAT_CMD		1
#define MT_TXD1_HDR_FORMAT_SHIFT	16

#define MCU_PKT_ID			0xa0
#define MCU_Q_NA			3
#define MCU_S2D_H2N			0

#define MCU_TXD_SIZE			64
#define MCU_RXD_SIZE			36
#define MCU_STATUS_OFFSET		32
	/* Where the patch commands put their one byte of answer - four bytes
	 * before an ordinary payload starts, not at the end of the header.
	 */

/* The semaphore that says whether the patch is already aboard. */
#define PATCH_SEM_RELEASE		0
#define PATCH_SEM_GET			1

#define PATCH_NOT_DL_SEM_FAIL		0
#define PATCH_IS_DL			1
#define PATCH_NOT_DL_SEM_SUCCESS	2
#define PATCH_REL_SEM_SUCCESS		3

/* How a piece of firmware is to be treated on the way in. */
#define DL_MODE_ENCRYPT			(1u << 0)
#define DL_MODE_KEY_IDX_MASK		0x00000006
#define DL_MODE_KEY_IDX_SHIFT		1
#define DL_MODE_RESET_SEC_IV		(1u << 3)
#define DL_CONFIG_ENCRY_MODE_SEL	(1u << 6)
#define DL_MODE_NEED_RSP		(1u << 31)

#define FW_START_OVERRIDE		(1u << 0)

#define FW_FEATURE_ENCRYPT		(1 << 0)
#define FW_FEATURE_KEY_IDX_MASK		0x06
#define FW_FEATURE_KEY_IDX_SHIFT	1
#define FW_FEATURE_ENCRY_MODE		(1 << 4)
#define FW_FEATURE_OVERRIDE		(1 << 5)
#define FW_FEATURE_NOT_DOWNLOADED	(1 << 6)

/* How a patch section says it was encrypted. */
#define PATCH_SEC_ENC_TYPE_SHIFT	24
#define PATCH_SEC_ENC_TYPE_PLAIN	0x00
#define PATCH_SEC_ENC_TYPE_AES		0x01
#define PATCH_SEC_ENC_TYPE_SCRAMBLE	0x02
#define PATCH_SEC_NOT_SUPPORT		0xffffffff

#define FIRMWARE_CHUNK			4096

#define MCU_RESPONSE_TIMEOUT		3000000
#define MCU_POLL_INTERVAL		1000


static void
write_le32(uint8* where, uint32 value)
{
	where[0] = value & 0xff;
	where[1] = (value >> 8) & 0xff;
	where[2] = (value >> 16) & 0xff;
	where[3] = (value >> 24) & 0xff;
}


/* Put one buffer on an outbound ring and tell the card it is there.
 *
 * The control word is written after the other three because writing it is
 * what hands the descriptor over, and the whole descriptor has to have landed
 * in memory before the card is told how far we have got.
 */
static status_t
mt7922_ring_submit(mt7922_dev* device, mt7922_ring* ring, phys_addr_t buffer,
	size_t length)
{
	uint16 next = (ring->head + 1) % ring->count;
	if (next == ring->tail) {
		ERROR("the %s ring is full\n", "outbound");
		return B_BUSY;
	}

	mt7922_desc* descriptor
		= &((mt7922_desc*)ring->descriptors.address)[ring->head];

	descriptor->buf0 = (uint32)buffer;
	descriptor->buf1 = 0;
	descriptor->info = 0;
	memory_write_barrier();
	descriptor->ctrl = ((uint32)length << MT_DMA_CTL_SD_LEN0_SHIFT)
		| MT_DMA_CTL_LAST_SEC0;

	ring->head = next;

	memory_write_barrier();
	mt7922_write32(device, ring->registers + MT_RING_CPU_INDEX, ring->head);

	return B_OK;
}


/* Wait until the card has walked past everything we put on the ring. Outbound
 * progress is read from the card's own index rather than from the descriptors.
 */
static status_t
mt7922_ring_drain(mt7922_dev* device, mt7922_ring* ring, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		uint32 where = mt7922_read32(device,
			ring->registers + MT_RING_DMA_INDEX);

		if (where < ring->count)
			ring->tail = where;

		if (ring->tail == ring->head)
			return B_OK;

		if (system_time() >= deadline) {
			ERROR("the card stopped taking what it was given\n");
			return B_TIMED_OUT;
		}
		snooze(MCU_POLL_INTERVAL);
	}
}


/* Collect one thing the part has said. A receiving descriptor is the card's
 * until it marks it done; taking it back means clearing that mark again.
 */
static status_t
mt7922_event_read(mt7922_dev* device, uint8* buffer, size_t* length,
	bigtime_t timeout)
{
	mt7922_ring* ring = &device->eventRing;
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		mt7922_desc* descriptor
			= &((mt7922_desc*)ring->descriptors.address)[ring->tail];

		if ((descriptor->ctrl & MT_DMA_CTL_DMA_DONE) != 0) {
			memory_read_barrier();

			size_t got = (descriptor->ctrl & MT_DMA_CTL_SD_LEN0_MASK)
				>> MT_DMA_CTL_SD_LEN0_SHIFT;
			if (got > *length)
				got = *length;

			const uint8* from = (const uint8*)ring->buffers.address
				+ (size_t)ring->tail * MT7922_RX_BUFFER_SIZE;
			memcpy(buffer, from, got);
			*length = got;

			/* Give the descriptor back and let the card know. */
			descriptor->info = 0;
			descriptor->buf1 = 0;
			memory_write_barrier();
			descriptor->ctrl = (uint32)MT7922_RX_BUFFER_SIZE
				<< MT_DMA_CTL_SD_LEN0_SHIFT;

			ring->head = ring->tail;
			ring->tail = (ring->tail + 1) % ring->count;

			memory_write_barrier();
			mt7922_write32(device, ring->registers + MT_RING_CPU_INDEX,
				ring->head);

			return B_OK;
		}

		if (system_time() >= deadline)
			return B_TIMED_OUT;

		snooze(MCU_POLL_INTERVAL);
	}
}


/* Send one command and, if asked, wait for the answer to it.
 *
 * The sequence number is what ties the two together, and it never takes the
 * value zero: the part uses zero for things it says of its own accord, so a
 * zero would make an announcement look like an answer.
 */
static status_t
mt7922_mcu_send(mt7922_dev* device, uint8 command, const void* payload,
	size_t payloadLength, bool wantAnswer, uint8* answer)
{
	if (payloadLength + MCU_TXD_SIZE > device->commandBuffer.size)
		return B_BAD_VALUE;

	device->sequence = (device->sequence + 1) & 0xf;
	if (device->sequence == 0)
		device->sequence = 1;

	uint8* packet = (uint8*)device->commandBuffer.address;
	size_t total = MCU_TXD_SIZE + payloadLength;

	memset(packet, 0, MCU_TXD_SIZE);

	write_le32(packet, (uint32)(total & MT_TXD0_TX_BYTES_MASK)
		| ((uint32)MT_TX_TYPE_CMD << MT_TXD0_PKT_FMT_SHIFT)
		| ((uint32)MT_TX_MCU_PORT_RX_Q0 << MT_TXD0_Q_IDX_SHIFT));
	write_le32(packet + 4, MT_TXD1_LONG_FORMAT
		| ((uint32)MT_HDR_FORMAT_CMD << MT_TXD1_HDR_FORMAT_SHIFT));

	/* The length here counts from after the hardware's part of the header,
	 * not from the start of the packet.
	 */
	uint16 length = (uint16)(total - 32);
	packet[0x20] = length & 0xff;
	packet[0x21] = length >> 8;

	uint16 port = (1 << 15) | (MT_TX_MCU_PORT_RX_Q0 << 10);
	packet[0x22] = port & 0xff;
	packet[0x23] = port >> 8;

	packet[0x24] = command;
	packet[0x25] = MCU_PKT_ID;
	packet[0x26] = MCU_Q_NA;
	packet[0x27] = device->sequence;
	packet[0x2a] = MCU_S2D_H2N;

	if (payloadLength > 0)
		memcpy(packet + MCU_TXD_SIZE, payload, payloadLength);

	status_t status = mt7922_ring_submit(device, &device->commandRing,
		device->commandBuffer.physical, total);
	if (status != B_OK)
		return status;

	status = mt7922_ring_drain(device, &device->commandRing,
		MCU_RESPONSE_TIMEOUT);
	if (status != B_OK)
		return status;

	if (!wantAnswer)
		return B_OK;

	/* Answers to other things may arrive first; keep reading until one
	 * carries our sequence number.
	 */
	bigtime_t deadline = system_time() + MCU_RESPONSE_TIMEOUT;

	while (system_time() < deadline) {
		uint8 event[512];
		size_t got = sizeof(event);

		status = mt7922_event_read(device, event, &got, MCU_RESPONSE_TIMEOUT);
		if (status != B_OK) {
			ERROR("command %#x went unanswered\n", command);
			return status;
		}

		if (got < MCU_RXD_SIZE)
			continue;
		if (event[0x1d] != device->sequence)
			continue;

		if (answer != NULL)
			*answer = got > MCU_STATUS_OFFSET ? event[MCU_STATUS_OFFSET] : 0;

		return B_OK;
	}

	return B_TIMED_OUT;
}


/* Firmware payload carries no header of its own: the bytes go as they are,
 * on their own ring, and nothing is expected back.
 */
static status_t
mt7922_mcu_send_firmware(mt7922_dev* device, const uint8* data, size_t length)
{
	while (length > 0) {
		size_t piece = length < FIRMWARE_CHUNK ? length : FIRMWARE_CHUNK;

		memcpy(device->firmwareBuffer.address, data, piece);
		memory_write_barrier();

		status_t status = mt7922_ring_submit(device, &device->firmwareRing,
			device->firmwareBuffer.physical, piece);
		if (status != B_OK)
			return status;

		/* One piece at a time: the buffer is reused, so it cannot be refilled
		 * until the card has finished reading it.
		 */
		status = mt7922_ring_drain(device, &device->firmwareRing,
			MCU_RESPONSE_TIMEOUT);
		if (status != B_OK)
			return status;

		data += piece;
		length -= piece;
	}

	return B_OK;
}


/* Say where a piece is going and how it should be treated on the way. */
static status_t
mt7922_mcu_start_download(mt7922_dev* device, uint32 address, uint32 length,
	uint32 mode)
{
	uint8 request[12];
	write_le32(request, address);
	write_le32(request + 4, length);
	write_le32(request + 8, mode);

	/* Two addresses are the patch's own and are announced differently from
	 * everything else.
	 */
	uint8 command = (address == 0x200000 || address == 0x900000)
		? MCU_CMD_PATCH_START : MCU_CMD_TARGET_ADDRESS_LEN;

	return mt7922_mcu_send(device, command, request, sizeof(request), true,
		NULL);
}


static uint32
mt7922_patch_mode(uint32 info)
{
	uint32 mode = DL_MODE_NEED_RSP;

	if (info == PATCH_SEC_NOT_SUPPORT)
		return mode;

	switch (info >> PATCH_SEC_ENC_TYPE_SHIFT) {
		case PATCH_SEC_ENC_TYPE_PLAIN:
			break;

		case PATCH_SEC_ENC_TYPE_AES:
			mode |= DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV;
			mode |= ((info & 0xff) << DL_MODE_KEY_IDX_SHIFT)
				& DL_MODE_KEY_IDX_MASK;
			break;

		case PATCH_SEC_ENC_TYPE_SCRAMBLE:
			mode |= DL_MODE_ENCRYPT | DL_CONFIG_ENCRY_MODE_SEL
				| DL_MODE_RESET_SEC_IV;
			break;

		default:
			ERROR("a patch section is encrypted in a way this does not know\n");
			break;
	}

	return mode;
}


static uint32
mt7922_ram_mode(uint32 features)
{
	uint32 mode = DL_MODE_NEED_RSP;

	if ((features & FW_FEATURE_ENCRYPT) != 0)
		mode |= DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV;
	if ((features & FW_FEATURE_ENCRY_MODE) != 0)
		mode |= DL_CONFIG_ENCRY_MODE_SEL;

	mode |= (((features & FW_FEATURE_KEY_IDX_MASK) >> FW_FEATURE_KEY_IDX_SHIFT)
		<< DL_MODE_KEY_IDX_SHIFT) & DL_MODE_KEY_IDX_MASK;

	return mode;
}


static status_t
mt7922_load_patch(mt7922_dev* device)
{
	mt7922_firmware patch;
	memset(&patch, 0, sizeof(patch));

	status_t status = mt7922_firmware_read_patch(device, &patch);
	if (status != B_OK)
		return status;

	/* Ask whether it is already aboard. Only one host may be loading it, and
	 * the answer to asking is also the answer to whether it is needed.
	 */
	uint8 request[4];
	uint8 answer = 0;

	write_le32(request, PATCH_SEM_GET);
	status = mt7922_mcu_send(device, MCU_CMD_PATCH_SEM_CONTROL, request,
		sizeof(request), true, &answer);
	if (status != B_OK)
		goto done;

	if (answer == PATCH_IS_DL) {
		TRACE("the part already holds its patch\n");
		status = B_OK;
		goto done;
	}

	if (answer != PATCH_NOT_DL_SEM_SUCCESS) {
		ERROR("the part will not give up its patch semaphore (%u)\n", answer);
		status = B_BUSY;
		goto done;
	}

	for (uint32 i = 0; i < patch.count; i++) {
		const mt7922_region& region = patch.region[i];
		uint32 mode = mt7922_patch_mode(region.keyIndex);

		TRACE("patch section %" B_PRIu32 ": %" B_PRIu32 " bytes to %#" B_PRIx32
			", mode %#" B_PRIx32 "\n", i, region.length, region.address, mode);

		status = mt7922_mcu_start_download(device, region.address,
			region.length, mode);
		if (status != B_OK)
			goto release;

		status = mt7922_mcu_send_firmware(device,
			patch.data + region.offset, region.length);
		if (status != B_OK)
			goto release;
	}

	{
		uint8 finish[4] = { 0, 0, 0, 0 };
		status = mt7922_mcu_send(device, MCU_CMD_PATCH_FINISH, finish,
			sizeof(finish), true, &answer);
		if (status == B_OK && answer != 0) {
			ERROR("the part refused the patch it was given (%u)\n", answer);
			status = B_ERROR;
		}
	}

release:
	write_le32(request, PATCH_SEM_RELEASE);
	mt7922_mcu_send(device, MCU_CMD_PATCH_SEM_CONTROL, request,
		sizeof(request), true, &answer);

done:
	mt7922_firmware_free(&patch);
	return status;
}


static status_t
mt7922_load_ram(mt7922_dev* device)
{
	mt7922_firmware ram;
	memset(&ram, 0, sizeof(ram));

	status_t status = mt7922_firmware_read_ram(device, &ram);
	if (status != B_OK)
		return status;

	uint32 override = 0;

	for (uint32 i = 0; i < ram.count; i++) {
		const mt7922_region& region = ram.region[i];

		if ((region.features & FW_FEATURE_OVERRIDE) != 0)
			override = region.address;

		/* A region marked as not to be sent still takes up its place in the
		 * file; it is skipped, not removed.
		 */
		if ((region.features & FW_FEATURE_NOT_DOWNLOADED) != 0)
			continue;

		uint32 mode = mt7922_ram_mode(region.features);

		TRACE("RAM region %" B_PRIu32 ": %" B_PRIu32 " bytes to %#" B_PRIx32
			", mode %#" B_PRIx32 "\n", i, region.length, region.address, mode);

		status = mt7922_mcu_start_download(device, region.address,
			region.length, mode);
		if (status != B_OK)
			goto done;

		status = mt7922_mcu_send_firmware(device, ram.data + region.offset,
			region.length);
		if (status != B_OK)
			goto done;
	}

	{
		uint8 start[8];
		write_le32(start, override != 0 ? FW_START_OVERRIDE : 0);
		write_le32(start + 4, override);

		status = mt7922_mcu_send(device, MCU_CMD_FW_START, start,
			sizeof(start), true, NULL);
	}

done:
	mt7922_firmware_free(&ram);
	return status;
}


status_t
mt7922_mcu_start_firmware(mt7922_dev* device)
{
	/* Tell the part to restart its processor before handing it anything. */
	uint8 power[4] = { 1, 0, 0, 0 };
	mt7922_mcu_send(device, MCU_CMD_NIC_POWER_CTRL, power, sizeof(power),
		false, NULL);

	bigtime_t deadline = system_time() + 1000000;
	while (system_time() < deadline) {
		uint32 state = mt7922_read32(device, MT_CONN_ON_MISC);
		if ((state & MT_TOP_MISC_FW_STATE) == MT_TOP_MISC2_FW_PWR_ON)
			break;
		snooze(MCU_POLL_INTERVAL);
	}

	status_t status = mt7922_load_patch(device);
	if (status != B_OK) {
		ERROR("the patch did not go in: %s\n", strerror(status));
		return status;
	}

	status = mt7922_load_ram(device);
	if (status != B_OK) {
		ERROR("the firmware did not go in: %s\n", strerror(status));
		return status;
	}

	/* The part says when what it was given is running. */
	deadline = system_time() + 1500000;
	while (true) {
		uint32 state = mt7922_read32(device, MT_CONN_ON_MISC);
		if ((state & MT_TOP_MISC2_FW_N9_RDY) == MT_TOP_MISC2_FW_N9_RDY) {
			TRACE("the firmware is running\n");
			return B_OK;
		}
		if (system_time() >= deadline) {
			ERROR("the firmware never said it had started (%#" B_PRIx32 ")\n",
				state);
			return B_TIMED_OUT;
		}
		snooze(MCU_POLL_INTERVAL);
	}
}
