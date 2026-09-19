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

#include <stdio.h>
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

/* Commands of the other kind. They differ on the wire in one byte only: they
 * say they are a setting rather than saying nothing at all.
 */
#define MCU_CE_CMD_GET_NIC_CAPAB	0x8a
#define MCU_CE_CMD_CHIP_CONFIG		0xca
#define MCU_CE_CMD_SET_CHAN_DOMAIN	0x0f
#define MCU_CE_CMD_START_HW_SCAN	0x03
#define MCU_CE_CMD_SET_RX_FILTER	0x0a

/* Commands with a shorter header of their own. */
#define MCU_UNI_TXD_SIZE		48
#define MCU_UNI_EXT_ACK			7
#define MCU_UNI_CMD_DEV_INFO_UPDATE	0x01
#define MCU_UNI_CMD_BSS_INFO_UPDATE	0x02

#define MCU_EXT_CMD_SET_RX_PATH		0x4e

/* What it says when it has finished looking around. */
#define MCU_EVENT_SCAN_DONE		0x0d

/* Reading a frame off the air. */
#define MT_RXD_FIXED_SIZE		24
#define MT_RX_TYPE_NORMAL		2
#define MT_RX_TYPE_EVENT		7
#define MT_RX_TYPE_NORMAL_MCU		8
#define MT_RXD1_GROUP_1			(1 << 11)
#define MT_RXD1_GROUP_2			(1 << 12)
#define MT_RXD1_GROUP_3			(1 << 13)
#define MT_RXD1_GROUP_4			(1 << 14)
#define MT_RXD1_GROUP_5			(1 << 15)
#define MT_RXD1_FCS_ERROR		(1 << 27)
#define MT_FRAME_BEACON			0x80

#define MT7922_STATION_INDEX		19
#define MT7922_SCAN_REQUEST_SIZE	1186
#define MT7922_SCAN_TIMEOUT		15000000

/* Commands that carry a second identifier beside the first. */
#define MCU_CMD_EXT_CID			0xed
#define MCU_EXT_CMD_EFUSE_BUFFER_MODE	0x21
#define MCU_EXT_CMD_PROTECT_CTRL	0x3e
#define MCU_EXT_CMD_MAC_INIT_CTRL	0x46
#define MCU_Q_SET			1

/* What the firmware may say about itself. */
#define MT_NIC_CAP_MAC_ADDR		0x07
#define MT_NIC_CAP_PHY			0x08

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


static uint32
read_le32(const uint8* from)
{
	return from[0] | ((uint32)from[1] << 8) | ((uint32)from[2] << 16)
		| ((uint32)from[3] << 24);
}


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
static bool
mt7922_ring_take(mt7922_dev* device, mt7922_ring* ring, uint8* buffer,
	size_t* length)
{
	{
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

			return true;
		}
	}

	return false;
}


/* The part answers on one ring while it is being given its firmware and on
 * another once that firmware is running, and nothing says exactly when it
 * changes over. Watching both costs one extra read and removes the question.
 */
static status_t
mt7922_event_read(mt7922_dev* device, uint8* buffer, size_t* length,
	bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		size_t got = *length;
		if (mt7922_ring_take(device, &device->eventRing, buffer, &got)) {
			*length = got;
			return B_OK;
		}

		got = *length;
		if (mt7922_ring_take(device, &device->lateEventRing, buffer, &got)) {
			*length = got;
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
mt7922_mcu_send_etc(mt7922_dev* device, uint8 command, uint8 setQuery,
	const void* payload, size_t payloadLength, uint8* reply,
	size_t* replyLength)
{
	bool wantAnswer = reply != NULL;

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
	packet[0x26] = setQuery;
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
		uint8 event[1024];
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

		size_t copy = got < *replyLength ? got : *replyLength;
		memcpy(reply, event, copy);
		*replyLength = got;
		return B_OK;
	}

	return B_TIMED_OUT;
}


static status_t
mt7922_mcu_send(mt7922_dev* device, uint8 command, const void* payload,
	size_t payloadLength, bool wantAnswer, uint8* answer)
{
	uint8 reply[256];
	size_t replyLength = sizeof(reply);

	status_t status = mt7922_mcu_send_etc(device, command, MCU_Q_NA, payload,
		payloadLength, wantAnswer ? reply : NULL, &replyLength);

	if (status == B_OK && answer != NULL) {
		*answer = replyLength > MCU_STATUS_OFFSET
			? reply[MCU_STATUS_OFFSET] : 0;
	}

	return status;
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
	/* A second claim of ownership, different from the one that woke the
	 * registers and no substitute for it: that one is made through a register
	 * always in reach, this one through the moveable window, and it is made
	 * once there are rings and before anything goes over them.
	 */
	TRACE("firmware: claiming ownership\n");
	mt7922_write32(device, MT_TOP_LPCR_HOST_BAND0, LPCR_HOST_DRV_OWN);

	bigtime_t owned = system_time() + 500000;
	while ((mt7922_read32(device, MT_TOP_LPCR_HOST_BAND0)
			& LPCR_HOST_FW_OWN) != 0) {
		if (system_time() >= owned) {
			ERROR("the part would not hand over before firmware\n");
			return B_TIMED_OUT;
		}
		snooze(MCU_POLL_INTERVAL);
	}

	/* Say which mode the firmware is to come up in. This has to be said
	 * before it is sent, not after.
	 */
	TRACE("firmware: owned, setting mode\n");
	mt7922_write32(device, MT_SWDEF_MODE, MT_SWDEF_NORMAL_MODE);

	/* Tell the part to restart its processor before handing it anything. */
	TRACE("firmware: restarting the part's processor\n");
	uint8 power[4] = { 1, 0, 0, 0 };
	status_t powerStatus = mt7922_mcu_send(device, MCU_CMD_NIC_POWER_CTRL,
		power, sizeof(power), false, NULL);
	TRACE("firmware: that command %s\n",
		powerStatus == B_OK ? "was taken" : strerror(powerStatus));

	bigtime_t deadline = system_time() + 1000000;
	while (system_time() < deadline) {
		uint32 state = mt7922_read32(device, MT_CONN_ON_MISC);
		if ((state & MT_TOP_MISC_FW_STATE) == MT_TOP_MISC2_FW_PWR_ON)
			break;
		snooze(MCU_POLL_INTERVAL);
	}

	TRACE("firmware: the part reports %#" B_PRIx32 ", sending the patch\n",
		mt7922_read32(device, MT_CONN_ON_MISC));

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


/* Ask the running firmware what it is.
 *
 * The answer is a count and then that many pieces, each headed by what it is
 * and how long it is. The length counts the piece and not its header, which is
 * the sort of thing that is only wrong once.
 *
 * Among the pieces is the address this radio answers to - the Wi-Fi side's
 * equivalent of the Bluetooth half's, and the first thing this part says about
 * itself that is of any use to anyone.
 */
status_t
mt7922_mcu_read_capability(mt7922_dev* device)
{
	uint8 reply[1024];
	size_t length = sizeof(reply);

	status_t status = mt7922_mcu_send_etc(device, MCU_CE_CMD_GET_NIC_CAPAB,
		MCU_Q_SET, NULL, 0, reply, &length);
	if (status != B_OK) {
		ERROR("the firmware would not say what it is: %s\n", strerror(status));
		return status;
	}

	if (length < MCU_RXD_SIZE + 4) {
		ERROR("what it said about itself is too short (%" B_PRIuSIZE ")\n",
			length);
		return B_IO_ERROR;
	}

	uint32 count = reply[MCU_RXD_SIZE] | (reply[MCU_RXD_SIZE + 1] << 8);
	size_t at = MCU_RXD_SIZE + 4;

	TRACE("the firmware says %" B_PRIu32 " things about itself\n", count);

	for (uint32 i = 0; i < count && at + 8 <= length; i++) {
		uint32 tag = reply[at] | ((uint32)reply[at + 1] << 8)
			| ((uint32)reply[at + 2] << 16) | ((uint32)reply[at + 3] << 24);
		uint32 size = reply[at + 4] | ((uint32)reply[at + 5] << 8)
			| ((uint32)reply[at + 6] << 16) | ((uint32)reply[at + 7] << 24);

		at += 8;
		if (at + size > length)
			break;

		switch (tag) {
			case MT_NIC_CAP_MAC_ADDR:
				if (size >= 6) {
					memcpy(device->address, reply + at, 6);
					device->hasAddress = true;
				}
				break;

			case MT_NIC_CAP_PHY:
				if (size >= 12) {
					device->streams = reply[at + 4];
					device->bands = reply[at + 10];
				}
				break;
		}

		at += size;
	}

	if (!device->hasAddress) {
		ERROR("the firmware never said what address this radio answers to\n");
		return B_ERROR;
	}

	TRACE("radio %02x:%02x:%02x:%02x:%02x:%02x, %u stream%s, %s%s\n",
		device->address[0], device->address[1], device->address[2],
		device->address[3], device->address[4], device->address[5],
		device->streams, device->streams == 1 ? "" : "s",
		(device->bands & 1) != 0 ? "2.4 GHz" : "",
		(device->bands & 2) != 0 ? " and 5 GHz" : "");

	return B_OK;
}


/* A third kind of command. These carry a second identifier beside the first,
 * and say so twice: once by setting it, and once by acknowledging that they
 * have.
 */
static status_t
mt7922_mcu_send_ext(mt7922_dev* device, uint8 extended, const void* payload,
	size_t payloadLength, bool wantAnswer)
{
	uint8 reply[256];
	size_t replyLength = sizeof(reply);

	/* Built by hand rather than through the ordinary path, because two of
	 * the header's bytes differ and they are not the ones that usually do.
	 */
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

	uint16 length = (uint16)(total - 32);
	packet[0x20] = length & 0xff;
	packet[0x21] = length >> 8;

	uint16 port = (1 << 15) | (MT_TX_MCU_PORT_RX_Q0 << 10);
	packet[0x22] = port & 0xff;
	packet[0x23] = port >> 8;

	packet[0x24] = MCU_CMD_EXT_CID;
	packet[0x25] = MCU_PKT_ID;
	packet[0x26] = MCU_Q_SET;
	packet[0x27] = device->sequence;
	packet[0x29] = extended;
	packet[0x2a] = MCU_S2D_H2N;
	packet[0x2b] = 1;		/* it is acknowledged */

	if (payloadLength > 0)
		memcpy(packet + MCU_TXD_SIZE, payload, payloadLength);

	status_t status = mt7922_ring_submit(device, &device->commandRing,
		device->commandBuffer.physical, total);
	if (status != B_OK)
		return status;

	status = mt7922_ring_drain(device, &device->commandRing,
		MCU_RESPONSE_TIMEOUT);
	if (status != B_OK || !wantAnswer)
		return status;

	bigtime_t deadline = system_time() + MCU_RESPONSE_TIMEOUT;
	while (system_time() < deadline) {
		uint8 event[1024];
		size_t got = sizeof(event);

		status = mt7922_event_read(device, event, &got, MCU_RESPONSE_TIMEOUT);
		if (status != B_OK) {
			ERROR("extended command %#x went unanswered\n", extended);
			return status;
		}
		if (got >= MCU_RXD_SIZE && event[0x1d] == device->sequence)
			return B_OK;
	}

	return B_TIMED_OUT;
}


/* Get the part from "running firmware" to "willing to look around".
 *
 * Each of these is a small thing the firmware wants said before it will do
 * anything useful, and none of them is interesting on its own.
 */
status_t
mt7922_mcu_prepare(mt7922_dev* device)
{
	/* Read the calibration data out of the part's own store. */
	uint8 buffer[4] = { 0 /* from the store */, 1 /* all of it */, 0, 0 };
	status_t status = mt7922_mcu_send_ext(device, MCU_EXT_CMD_EFUSE_BUFFER_MODE,
		buffer, sizeof(buffer), true);
	if (status != B_OK) {
		ERROR("it would not read its calibration: %s\n", strerror(status));
		return status;
	}

	/* When to bother protecting a frame. */
	uint8 protect[12];
	memset(protect, 0, sizeof(protect));
	protect[0] = 1;
	write_le32(protect + 4, 0x92b);
	write_le32(protect + 8, 0x2);
	status = mt7922_mcu_send_ext(device, MCU_EXT_CMD_PROTECT_CTRL, protect,
		sizeof(protect), true);
	if (status != B_OK) {
		ERROR("it would not take its protection settings: %s\n",
			strerror(status));
		return status;
	}

	/* Keep the part awake. The power saving it would otherwise do brings a
	 * handshake with it that nothing here is ready for.
	 */
	uint8 config[328];
	memset(config, 0, sizeof(config));
	strcpy((char*)config + 8, "KeepFullPwr 1");
	status = mt7922_mcu_send_etc(device, MCU_CE_CMD_CHIP_CONFIG, MCU_Q_SET,
		config, sizeof(config), NULL, NULL);
	if (status != B_OK) {
		ERROR("it would not stay awake: %s\n", strerror(status));
		return status;
	}

	/* And start the part of it that deals with the air. */
	uint8 mac[4] = { 1 /* on */, 0 /* first radio */, 0, 0 };
	status = mt7922_mcu_send_ext(device, MCU_EXT_CMD_MAC_INIT_CTRL, mac,
		sizeof(mac), true);
	if (status != B_OK) {
		ERROR("it would not start its radio: %s\n", strerror(status));
		return status;
	}

	/* And stop throwing away what it hears. Until this is said the radio is
	 * listening but discarding, which is indistinguishable from deafness.
	 */
	TRACE("the receive filter starts at %#" B_PRIx32 "\n",
		mt7922_read32(device, MT_WF_RFCR));

	uint8 filter[68];
	memset(filter, 0, sizeof(filter));
	filter[4] = 1;			/* by rule, not by bit */

	/* Hear other people's networks as well as our own. Without that a scan
	 * turns up only the traffic of a network we have not joined, which is
	 * exactly as useless as it sounds.
	 */
	write_le32(filter + 8, MT7922_FILTER_ENABLE | MT7922_FILTER_OTHER_BSS);

	status = mt7922_mcu_send_etc(device, MCU_CE_CMD_SET_RX_FILTER, MCU_Q_SET,
		filter, sizeof(filter), NULL, NULL);
	if (status != B_OK) {
		ERROR("it would not open its receive filter: %s\n", strerror(status));
		return status;
	}

	/* And specifically stop throwing away the announcements of networks we
	 * are not part of, which are the whole point of looking around.
	 */
	memset(filter, 0, sizeof(filter));
	filter[4] = 2;			/* by bit, not by rule */
	write_le32(filter + 12, MT_RFCR_DROP_OTHER_BEACON);
	filter[16] = 1 << 1;		/* and the bit is to be cleared */

	status = mt7922_mcu_send_etc(device, MCU_CE_CMD_SET_RX_FILTER, MCU_Q_SET,
		filter, sizeof(filter), NULL, NULL);
	if (status != B_OK) {
		ERROR("it would not stop dropping beacons: %s\n", strerror(status));
		return status;
	}

	snooze(20000);

	/* The firmware leaves bits set here whose meaning is not written down
	 * anywhere reachable, and some of them are evidently discarding every
	 * announcement while letting ordinary traffic through. Since this
	 * register is reachable directly, say plainly what is wanted: discard
	 * nothing, and let the driver decide what to ignore.
	 */
	TRACE("the receive filter is %#" B_PRIx32 ", clearing it\n",
		mt7922_read32(device, MT_WF_RFCR));

	mt7922_write32(device, MT_WF_RFCR, 0);
	snooze(1000);

	TRACE("the receive filter is now %#" B_PRIx32 "\n",
		mt7922_read32(device, MT_WF_RFCR));

	TRACE("the radio is prepared\n");
	return B_OK;
}


/* A fourth kind of command, with a shorter header of its own and its real
 * arguments wrapped in yet another header inside the payload.
 */
static status_t
mt7922_mcu_send_uni(mt7922_dev* device, uint16 command, const void* payload,
	size_t payloadLength)
{
	device->sequence = (device->sequence + 1) & 0xf;
	if (device->sequence == 0)
		device->sequence = 1;

	uint8* packet = (uint8*)device->commandBuffer.address;
	size_t total = MCU_UNI_TXD_SIZE + payloadLength;

	memset(packet, 0, MCU_UNI_TXD_SIZE);
	write_le32(packet, (uint32)(total & MT_TXD0_TX_BYTES_MASK)
		| ((uint32)MT_TX_TYPE_CMD << MT_TXD0_PKT_FMT_SHIFT)
		| ((uint32)MT_TX_MCU_PORT_RX_Q0 << MT_TXD0_Q_IDX_SHIFT));
	write_le32(packet + 4, MT_TXD1_LONG_FORMAT
		| ((uint32)MT_HDR_FORMAT_CMD << MT_TXD1_HDR_FORMAT_SHIFT));

	uint16 length = (uint16)(total - 32);
	packet[0x20] = length & 0xff;
	packet[0x21] = length >> 8;
	packet[0x22] = command & 0xff;
	packet[0x23] = command >> 8;
	packet[0x25] = MCU_PKT_ID;
	packet[0x27] = device->sequence;
	packet[0x2a] = MCU_S2D_H2N;
	packet[0x2b] = MCU_UNI_EXT_ACK;

	memcpy(packet + MCU_UNI_TXD_SIZE, payload, payloadLength);

	status_t status = mt7922_ring_submit(device, &device->commandRing,
		device->commandBuffer.physical, total);
	if (status != B_OK)
		return status;

	status = mt7922_ring_drain(device, &device->commandRing,
		MCU_RESPONSE_TIMEOUT);
	if (status != B_OK)
		return status;

	bigtime_t deadline = system_time() + MCU_RESPONSE_TIMEOUT;
	while (system_time() < deadline) {
		uint8 event[1024];
		size_t got = sizeof(event);

		status = mt7922_event_read(device, event, &got, MCU_RESPONSE_TIMEOUT);
		if (status != B_OK)
			return status;
		if (got >= MCU_RXD_SIZE && event[0x1d] == device->sequence)
			return B_OK;
	}

	return B_TIMED_OUT;
}


/* Which channels exist here at all. Nothing is scanned that is not in this
 * list, so it is the shortest useful one: the channels everybody has.
 */
static const uint8 kChannels2GHz[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8 kChannels5GHz[] = { 36, 40, 44, 48, 149, 153, 157, 161 };


static status_t
mt7922_mcu_set_channels(mt7922_dev* device)
{
	uint8 request[12 + (sizeof(kChannels2GHz) + sizeof(kChannels5GHz)) * 8];
	memset(request, 0, sizeof(request));

	request[0] = '0';
	request[1] = '0';
	request[4] = 0;			/* 20 and 40 MHz down low */
	request[5] = 3;			/* and everything up high */
	request[6] = 3;
	request[8] = sizeof(kChannels2GHz);
	request[9] = sizeof(kChannels5GHz);

	size_t at = 12;
	for (size_t i = 0; i < sizeof(kChannels2GHz); i++, at += 8)
		request[at] = kChannels2GHz[i];
	for (size_t i = 0; i < sizeof(kChannels5GHz); i++, at += 8)
		request[at] = kChannels5GHz[i];

	return mt7922_mcu_send_etc(device, MCU_CE_CMD_SET_CHAN_DOMAIN, MCU_Q_SET,
		request, sizeof(request), NULL, NULL);
}


static status_t
mt7922_mcu_set_channel(mt7922_dev* device)
{
	uint8 request[76];
	memset(request, 0, sizeof(request));

	request[0] = 1;			/* sit on channel one to begin with */
	request[1] = 1;
	request[2] = 0;			/* 20 MHz */
	request[3] = device->streams;
	request[4] = (1 << device->streams) - 1;
	request[5] = 0;			/* no particular reason */
	request[6] = 0;
	request[0x0a] = 0;		/* down low */

	return mt7922_mcu_send_ext(device, MCU_EXT_CMD_SET_RX_PATH, request,
		sizeof(request), true);
}


/* Announce ourselves: first as a radio with an address, then as a station
 * that has not joined anything yet. A scan is asked for on behalf of the
 * second, so it has to exist first.
 */
static status_t
mt7922_mcu_announce(mt7922_dev* device)
{
	uint8 self[16];
	memset(self, 0, sizeof(self));
	self[6] = 12;			/* the length of this piece */
	self[8] = 1;			/* and it is active */
	memcpy(self + 10, device->address, 6);

	status_t status = mt7922_mcu_send_uni(device, MCU_UNI_CMD_DEV_INFO_UPDATE,
		self, sizeof(self));
	if (status != B_OK) {
		ERROR("it would not take our address: %s\n", strerror(status));
		return status;
	}

	uint8 station[36];
	memset(station, 0, sizeof(station));
	station[6] = 32;
	station[8] = 1;			/* active */
	write_le32(station + 0x0c, 0x00010001);	/* a station on an ordinary network */
	station[0x10] = 1;
	station[0x18] = MT7922_STATION_INDEX;
	station[0x1e] = MT7922_STATION_INDEX;

	status = mt7922_mcu_send_uni(device, MCU_UNI_CMD_BSS_INFO_UPDATE, station,
		sizeof(station));
	if (status != B_OK)
		ERROR("it would not take our station: %s\n", strerror(status));

	return status;
}


/* Look around.
 *
 * What comes back of this is one event saying it has finished. What it heard
 * arrives separately, as ordinary received frames, which is a road not yet
 * built - so for now the question this answers is only whether the radio will
 * go and listen when asked.
 */
status_t
mt7922_mcu_scan(mt7922_dev* device)
{
	status_t status = mt7922_mcu_set_channels(device);
	if (status != B_OK) {
		ERROR("it would not take the channel list: %s\n", strerror(status));
		return status;
	}

	status = mt7922_mcu_set_channel(device);
	if (status != B_OK) {
		ERROR("it would not settle on a channel: %s\n", strerror(status));
		return status;
	}

	status = mt7922_mcu_announce(device);
	if (status != B_OK)
		return status;

	uint8 request[MT7922_SCAN_REQUEST_SIZE];
	memset(request, 0, sizeof(request));

	request[0] = 1;			/* this is scan number one */
	request[1] = 0;			/* on behalf of our station */
	request[2] = 0;			/* listening, not asking */
	request[3] = 1;			/* for anyone at all */
	request[6] = 1 << 5;		/* in more than one go */
	request[7] = 1;			/* and the later fields are meant */
	request[0x9e] = 4;		/* these channels, named below */

	size_t count = sizeof(kChannels2GHz) + sizeof(kChannels5GHz);
	request[0x9f] = (uint8)count;

	size_t at = 0xa0;
	for (size_t i = 0; i < sizeof(kChannels2GHz); i++, at += 2) {
		request[at] = 1;	/* down low */
		request[at + 1] = kChannels2GHz[i];
	}
	for (size_t i = 0; i < sizeof(kChannels5GHz); i++, at += 2) {
		request[at] = 2;	/* up high */
		request[at + 1] = kChannels5GHz[i];
	}

	memset(request + 0x456, 0xff, 6);	/* addressed to everyone */

	status = mt7922_mcu_send_etc(device, MCU_CE_CMD_START_HW_SCAN, MCU_Q_SET,
		request, sizeof(request), NULL, NULL);
	if (status != B_OK) {
		ERROR("it would not start looking: %s\n", strerror(status));
		return status;
	}

	TRACE("the radio is listening on %" B_PRIuSIZE " channels\n", count);

	/* It says when it has finished of its own accord, rather than in answer
	 * to anything, so this is recognised by what it is and not by what it
	 * replies to.
	 */
	bigtime_t deadline = system_time() + MT7922_SCAN_TIMEOUT;
	while (system_time() < deadline) {
		uint8 event[1024];
		size_t got = sizeof(event);

		if (mt7922_event_read(device, event, &got, MT7922_SCAN_TIMEOUT)
				!= B_OK) {
			break;
		}

		if (got >= MCU_RXD_SIZE && event[0x1c] == MCU_EVENT_SCAN_DONE) {
			TRACE("the radio finished looking\n");
			return B_OK;
		}
	}

	ERROR("the radio never said it had finished looking\n");
	return B_TIMED_OUT;
}


/* Show what arrived on the air.
 *
 * Each frame is preceded by a description of itself whose length depends on
 * which parts it chose to include, so where the frame proper begins has to be
 * worked out rather than assumed: a fixed opening, then a run of optional
 * pieces named in the second word, then however many bytes of padding the
 * third word admits to.
 */
static int
mt7922_ring_used(mt7922_dev* device, mt7922_ring* ring)
{
	int filled = 0;
	for (uint16 i = 0; i < ring->count; i++) {
		mt7922_desc* descriptor
			= &((mt7922_desc*)ring->descriptors.address)[i];
		if ((descriptor->ctrl & MT_DMA_CTL_DMA_DONE) != 0)
			filled++;
	}
	return filled;
}


static int
mt7922_dump_ring(mt7922_dev* device, mt7922_ring* ring, const char* which,
	int wanted)
{
	int heard = 0;

	for (uint16 i = 0; i < ring->count && heard < wanted; i++) {
		mt7922_desc* descriptor
			= &((mt7922_desc*)ring->descriptors.address)[i];

		if ((descriptor->ctrl & MT_DMA_CTL_DMA_DONE) == 0)
			continue;

		size_t got = (descriptor->ctrl & MT_DMA_CTL_SD_LEN0_MASK)
			>> MT_DMA_CTL_SD_LEN0_SHIFT;
		if (got < 64 || got > MT7922_RX_BUFFER_SIZE)
			continue;

		const uint8* data = (const uint8*)ring->buffers.address
			+ (size_t)i * MT7922_RX_BUFFER_SIZE;

		uint32 word0 = read_le32(data);
		uint32 word1 = read_le32(data + 4);
		uint32 word2 = read_le32(data + 8);
		uint32 word3 = read_le32(data + 12);

		/* Only ordinary frames off the air; the rest is the part talking
		 * about itself.
		 */
		/* An ordinary frame off the air, or one the part has handed over
		 * dressed as something it said itself.
		 */
		uint32 type = (word0 >> 27) & 0x1f;
		device->packetKind[type & 0x1f]++;

		if (type != MT_RX_TYPE_NORMAL && type != MT_RX_TYPE_NORMAL_MCU) {
			if (type != MT_RX_TYPE_EVENT || ((word0 >> 16) & 0xf) != 1)
				continue;
		}
		if ((word1 & MT_RXD1_FCS_ERROR) != 0)
			continue;

		size_t at = MT_RXD_FIXED_SIZE;
		if ((word1 & MT_RXD1_GROUP_4) != 0)
			at += 16;
		if ((word1 & MT_RXD1_GROUP_1) != 0)
			at += 16;
		if ((word1 & MT_RXD1_GROUP_2) != 0)
			at += 8;
		if ((word1 & MT_RXD1_GROUP_3) != 0) {
			at += 8;
			if ((word1 & MT_RXD1_GROUP_5) != 0)
				at += 72;
		}

		at += 2 * ((word2 >> 14) & 0x3);

		if (at + 36 > got)
			continue;

		const uint8* frame = data + at;

		/* Count what kinds arrive. "No beacons" and "no management frames at
		 * all" want different answers, and so does "plenty of both but my
		 * reading of them is wrong".
		 */
		/* By the whole control byte, not half of it: a beacon is 0x80 and a
		 * QoS data frame is 0x88, and bucketing by the top nibble makes the
		 * second look like the first.
		 */
		device->frameKind[frame[0] >> 4]++;
		if (frame[0] == MT_FRAME_BEACON)
			device->beacons++;
		if ((frame[0] & 0x0c) == 0)
			device->management++;

		if (device->framesShown < 4) {
			char line[80];
			for (int k = 0; k < 16; k++)
				sprintf(line + k * 3, "%02x ", frame[k]);
			TRACE("frame type %#x groups %#x pad %" B_PRIu32 " at %" B_PRIuSIZE
				" of %" B_PRIuSIZE ": %s\n", (unsigned)((word0 >> 27) & 0x1f),
				(unsigned)((word1 >> 11) & 0x1f), (word2 >> 14) & 3, at, got,
				line);
			device->framesShown++;
		}

		/* Beacons only: management frames that announce a network. */
		if (frame[0] != MT_FRAME_BEACON)
			continue;

		uint32 channel = (word3 >> 8) & 0xff;

		/* The name is the first thing said after the header and the fixed
		 * fields that follow it.
		 */
		const uint8* elements = frame + 24 + 12;
		size_t remaining = got - at - 24 - 12;
		char name[33];

		strcpy(name, "(hidden)");
		if (remaining >= 2 && elements[0] == 0 && elements[1] > 0
			&& elements[1] < sizeof(name) && (size_t)elements[1] + 2 <= remaining) {
			memcpy(name, elements + 2, elements[1]);
			name[elements[1]] = 0;
		}

		TRACE("heard \"%s\" on channel %" B_PRIu32 " from "
			"%02x:%02x:%02x:%02x:%02x:%02x (%s)\n", name, channel, which,
			frame[16], frame[17], frame[18], frame[19], frame[20], frame[21]);

		heard++;
	}

	return heard;
}


void
mt7922_dump_air(mt7922_dev* device, int wanted)
{
	/* Where the part sends what it hears. Announcements can be routed to the
	 * part's own processor instead of to us, which looks from here exactly
	 * like a radio that hears traffic but never hears a network.
	 */
	TRACE("frames are routed by %#" B_PRIx32 "\n",
		mt7922_read32(device, MT_MDP_BNRCFR0));

	int heard = mt7922_dump_ring(device, &device->dataRing, "the air", wanted);
	heard += mt7922_dump_ring(device, &device->lateEventRing, "the processor",
		wanted - heard);

	if (heard == 0) {
		TRACE("no networks heard; %d frames on the air, %d from the"
			" processor, %d of them management\n",
			mt7922_ring_used(device, &device->dataRing),
			mt7922_ring_used(device, &device->lateEventRing),
			device->management);
		TRACE("%d of them announced a network\n", device->beacons);

		char line[160];
		int at = 0;
		for (int k = 0; k < 16; k++) {
			if (device->frameKind[k] != 0) {
				at += sprintf(line + at, "%x0:%d ", k,
					device->frameKind[k]);
			}
		}
		TRACE("what arrived, by kind: %s\n", at > 0 ? line : "nothing");

		at = 0;
		for (int k = 0; k < 32; k++) {
			if (device->packetKind[k] != 0)
				at += sprintf(line + at, "%d:%d ", k, device->packetKind[k]);
		}
		TRACE("and by what the part called them: %s\n",
			at > 0 ? line : "nothing");
	} else
		TRACE("%d network%s heard\n", heard, heard == 1 ? "" : "s");
}
