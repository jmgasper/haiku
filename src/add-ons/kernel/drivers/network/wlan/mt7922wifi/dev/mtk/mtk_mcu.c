/*
 * Talking to the MT7922's own processor, and handing it its firmware.
 *
 * Commands travel as packets on a ring: a sixty-four byte header and then the
 * command's arguments. Answers arrive on another ring behind thirty-six
 * bytes, tied to what was asked by a sequence number that is never zero -
 * zero being what the part uses for things it says unprompted.
 *
 * Firmware payload is the exception that catches people out: it carries no
 * header at all, and goes on a ring of its own with nothing expected back.
 *
 * Distributed under the terms of the MIT License.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/firmware.h>
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


static uint32_t
mtk_le32(const uint8_t* from)
{
	return from[0] | ((uint32_t)from[1] << 8) | ((uint32_t)from[2] << 16)
		| ((uint32_t)from[3] << 24);
}


static uint32_t
mtk_be32(const uint8_t* from)
{
	return ((uint32_t)from[0] << 24) | ((uint32_t)from[1] << 16)
		| ((uint32_t)from[2] << 8) | from[3];
}


static void
mtk_put32(uint8_t* where, uint32_t value)
{
	where[0] = value & 0xff;
	where[1] = (value >> 8) & 0xff;
	where[2] = (value >> 16) & 0xff;
	where[3] = (value >> 24) & 0xff;
}


/* Put one buffer on an outbound ring and tell the card it is there. The
 * control word is written last because writing it is what hands the
 * descriptor over.
 */
static int
mtk_ring_submit(struct mtk_softc* sc, struct mtk_ring* ring, bus_addr_t buffer,
	size_t length)
{
	uint32_t* desc = (uint32_t*)ring->desc.addr;
	uint16_t next = (ring->head + 1) % ring->count;

	if (next == ring->tail)
		return EBUSY;

	desc[ring->head * 4 + 0] = (uint32_t)buffer;
	desc[ring->head * 4 + 2] = 0;
	desc[ring->head * 4 + 3] = 0;
	wmb();
	desc[ring->head * 4 + 1] = ((uint32_t)length << MTK_DMA_CTL_LEN_SHIFT)
		| MTK_DMA_CTL_LAST_SEC0;

	ring->head = next;

	wmb();
	mtk_write(sc, ring->regs + MTK_RING_CPU_INDEX, ring->head);

	return 0;
}


static int
mtk_ring_drain(struct mtk_softc* sc, struct mtk_ring* ring, int milliseconds)
{
	int i;

	for (i = 0; i <= milliseconds; i++) {
		uint32_t where = mtk_read(sc, ring->regs + MTK_RING_DMA_INDEX);

		if (where < ring->count)
			ring->tail = where;
		if (ring->tail == ring->head)
			return 0;

		DELAY(1000);
	}

	return ETIMEDOUT;
}


/* Take one thing off a receiving ring, and give the descriptor back. */
static int
mtk_ring_take(struct mtk_softc* sc, struct mtk_ring* ring, uint8_t* buffer,
	size_t* length)
{
	uint32_t* desc = (uint32_t*)ring->desc.addr;
	size_t got;

	if (ring->count == 0)
		return 0;
	if ((desc[ring->tail * 4 + 1] & MTK_DMA_CTL_DMA_DONE) == 0)
		return 0;

	rmb();

	got = (desc[ring->tail * 4 + 1] & MTK_DMA_CTL_LEN_MASK)
		>> MTK_DMA_CTL_LEN_SHIFT;
	if (got > *length)
		got = *length;

	memcpy(buffer, (const uint8_t*)ring->buffers.addr
		+ (size_t)ring->tail * MTK_RX_BUFFER_SIZE, got);
	*length = got;

	desc[ring->tail * 4 + 2] = 0;
	desc[ring->tail * 4 + 3] = 0;
	wmb();
	desc[ring->tail * 4 + 1] = (uint32_t)MTK_RX_BUFFER_SIZE
		<< MTK_DMA_CTL_LEN_SHIFT;

	ring->head = ring->tail;
	ring->tail = (ring->tail + 1) % ring->count;

	wmb();
	mtk_write(sc, ring->regs + MTK_RING_CPU_INDEX, ring->head);

	return 1;
}


/* The part answers on one ring before its firmware is running and another
 * afterwards, and nothing says exactly when it changes over. Watching both
 * costs one extra read and removes the question.
 */
static int
mtk_event_read(struct mtk_softc* sc, uint8_t* buffer, size_t* length,
	int milliseconds)
{
	int i;

	for (i = 0; i <= milliseconds; i++) {
		size_t got = *length;
		if (mtk_ring_take(sc, &sc->sc_eventq, buffer, &got)) {
			*length = got;
			return 0;
		}

		got = *length;
		if (mtk_ring_take(sc, &sc->sc_lateq, buffer, &got)) {
			*length = got;
			return 0;
		}

		DELAY(1000);
	}

	return ETIMEDOUT;
}


/* One command. The two lengths in the header overlap - one counts everything
 * after it, the other counts from partway in - which is easy to get wrong.
 */
static int
mtk_mcu_send(struct mtk_softc* sc, uint8_t command, uint8_t setQuery,
	const void* payload, size_t payloadLength, uint8_t* reply,
	size_t* replyLength)
{
	uint8_t* packet = (uint8_t*)sc->sc_cmdbuf.addr;
	size_t total = MTK_MCU_TXD_SIZE + payloadLength;
	uint16_t length, port;
	int error, i;

	if (total > sc->sc_cmdbuf.size)
		return EINVAL;

	sc->sc_seq = (sc->sc_seq + 1) & 0xf;
	if (sc->sc_seq == 0)
		sc->sc_seq = 1;

	memset(packet, 0, MTK_MCU_TXD_SIZE);
	mtk_put32(packet, (uint32_t)(total & 0xffff)
		| ((uint32_t)MTK_TX_TYPE_CMD << 23)
		| ((uint32_t)MTK_TX_MCU_PORT_RX_Q0 << 25));
	mtk_put32(packet + 4, MTK_TXD1_LONG_FORMAT
		| ((uint32_t)MTK_HDR_FORMAT_CMD << 16));

	length = (uint16_t)(total - 32);
	packet[0x20] = length & 0xff;
	packet[0x21] = length >> 8;

	port = (1 << 15) | (MTK_TX_MCU_PORT_RX_Q0 << 10);
	packet[0x22] = port & 0xff;
	packet[0x23] = port >> 8;

	packet[0x24] = command;
	packet[0x25] = MTK_MCU_PKT_ID;
	packet[0x26] = setQuery;
	packet[0x27] = sc->sc_seq;
	packet[0x2a] = MTK_MCU_S2D_H2N;

	if (payloadLength > 0)
		memcpy(packet + MTK_MCU_TXD_SIZE, payload, payloadLength);

	error = mtk_ring_submit(sc, &sc->sc_cmdq, sc->sc_cmdbuf.paddr, total);
	if (error != 0)
		return error;

	error = mtk_ring_drain(sc, &sc->sc_cmdq, 3000);
	if (error != 0 || reply == NULL)
		return error;

	/* Answers to other things may arrive first; keep reading until one
	 * carries our sequence number.
	 */
	for (i = 0; i < 3000; i++) {
		uint8_t event[1024];
		size_t got = sizeof(event);

		if (mtk_event_read(sc, event, &got, 100) != 0)
			continue;
		if (got < MTK_MCU_RXD_SIZE || event[0x1d] != sc->sc_seq)
			continue;

		if (got < *replyLength)
			*replyLength = got;
		memcpy(reply, event, *replyLength);
		return 0;
	}

	return ETIMEDOUT;
}


/* Firmware payload carries no header: the raw bytes go on a ring of their own
 * and nothing is expected back.
 */
static int
mtk_mcu_send_firmware(struct mtk_softc* sc, const uint8_t* data, size_t length)
{
	while (length > 0) {
		size_t piece = length < MTK_FIRMWARE_CHUNK
			? length : MTK_FIRMWARE_CHUNK;
		int error;

		memcpy(sc->sc_fwbuf.addr, data, piece);
		wmb();

		error = mtk_ring_submit(sc, &sc->sc_fwq, sc->sc_fwbuf.paddr, piece);
		if (error != 0)
			return error;

		/* The buffer is reused, so it cannot be refilled until the card
		 * has finished reading it.
		 */
		error = mtk_ring_drain(sc, &sc->sc_fwq, 3000);
		if (error != 0)
			return error;

		data += piece;
		length -= piece;
	}

	return 0;
}


static int
mtk_mcu_start_download(struct mtk_softc* sc, uint32_t address, uint32_t length,
	uint32_t mode)
{
	uint8_t request[12];
	uint8_t reply[256];
	size_t replyLength = sizeof(reply);
	uint8_t command;

	mtk_put32(request, address);
	mtk_put32(request + 4, length);
	mtk_put32(request + 8, mode);

	/* Two addresses are the patch's own and are announced differently. */
	command = (address == 0x200000 || address == 0x900000)
		? MTK_MCU_CMD_PATCH_START : MTK_MCU_CMD_TARGET_ADDRESS_LEN;

	return mtk_mcu_send(sc, command, MTK_MCU_Q_NA, request, sizeof(request),
		reply, &replyLength);
}


/* How a piece of the patch was encrypted, and so how to ask for it back. */
static uint32_t
mtk_patch_mode(uint32_t info)
{
	uint32_t mode = MTK_DL_MODE_NEED_RSP;

	if (info == 0xffffffff)
		return mode;

	switch (info >> 24) {
		case 0:
			break;
		case 1:
			mode |= MTK_DL_MODE_ENCRYPT | MTK_DL_MODE_RESET_SEC_IV;
			mode |= ((info & 0xff) << 1) & 0x6;
			break;
		case 2:
			mode |= MTK_DL_MODE_ENCRYPT | MTK_DL_CONFIG_ENCRY_MODE_SEL
				| MTK_DL_MODE_RESET_SEC_IV;
			break;
	}

	return mode;
}


static uint32_t
mtk_ram_mode(uint32_t features)
{
	uint32_t mode = MTK_DL_MODE_NEED_RSP;

	if ((features & MTK_FW_FEATURE_ENCRYPT) != 0)
		mode |= MTK_DL_MODE_ENCRYPT | MTK_DL_MODE_RESET_SEC_IV;
	if ((features & MTK_FW_FEATURE_ENCRY_MODE) != 0)
		mode |= MTK_DL_CONFIG_ENCRY_MODE_SEL;

	mode |= ((features & 0x06) >> 1 << 1) & 0x6;
	return mode;
}


/* The patch the bootloader takes first. Its numbers are big-endian, alone
 * among everything else here.
 */
static int
mtk_load_patch(struct mtk_softc* sc)
{
	const struct firmware* image;
	const uint8_t* data;
	uint8_t request[4], reply[256];
	size_t replyLength;
	uint32_t count, version, i;
	uint64_t accounted;
	int error = 0;
	uint8_t answer;

	image = firmware_get(MTK_PATCH_NAME);
	if (image == NULL) {
		device_printf(sc->sc_dev, "no %s to be found\n", MTK_PATCH_NAME);
		return ENOENT;
	}

	data = (const uint8_t*)image->data;
	if (image->datasize < MTK_PATCH_HEADER + MTK_PATCH_SECTION) {
		error = EINVAL;
		goto done;
	}

	version = mtk_be32(data + 0x14);
	count = mtk_be32(data + 0x2c);

	if (count == 0 || count > 16
		|| MTK_PATCH_HEADER + count * MTK_PATCH_SECTION > image->datasize) {
		device_printf(sc->sc_dev, "the patch says it has %u sections\n",
			count);
		error = EINVAL;
		goto done;
	}

	if ((version >> 16) != (sc->sc_rev & 0xffff)) {
		device_printf(sc->sc_dev, "patch is for revision %#x, part is %#x\n",
			version >> 16, sc->sc_rev & 0xffff);
		error = EINVAL;
		goto done;
	}

	/* The pieces have to account for the whole file; a layout read wrongly
	 * does not add up, and these bytes cannot be checked any other way.
	 */
	accounted = MTK_PATCH_HEADER + (uint64_t)count * MTK_PATCH_SECTION;
	for (i = 0; i < count; i++) {
		const uint8_t* section = data + MTK_PATCH_HEADER
			+ i * MTK_PATCH_SECTION;
		accounted += mtk_be32(section + 16);
	}
	if (accounted != image->datasize) {
		device_printf(sc->sc_dev, "the patch does not add up\n");
		error = EINVAL;
		goto done;
	}

	/* Ask whether it is already aboard; the answer to asking is also the
	 * answer to whether it is needed.
	 */
	mtk_put32(request, MTK_PATCH_SEM_GET);
	replyLength = sizeof(reply);
	error = mtk_mcu_send(sc, MTK_MCU_CMD_PATCH_SEM, MTK_MCU_Q_NA, request,
		sizeof(request), reply, &replyLength);
	if (error != 0)
		goto done;

	answer = replyLength > 32 ? reply[32] : 0;
	if (answer == MTK_PATCH_IS_DL) {
		device_printf(sc->sc_dev, "the part already holds its patch\n");
		goto done;
	}
	if (answer != MTK_PATCH_SEM_SUCCESS) {
		device_printf(sc->sc_dev, "the patch semaphore says %u\n", answer);
		error = EBUSY;
		goto done;
	}

	for (i = 0; i < count; i++) {
		const uint8_t* section = data + MTK_PATCH_HEADER
			+ i * MTK_PATCH_SECTION;
		uint32_t offset = mtk_be32(section + 4);
		uint32_t address = mtk_be32(section + 12);
		uint32_t length = mtk_be32(section + 16);
		uint32_t mode = mtk_patch_mode(mtk_be32(section + 20));

		error = mtk_mcu_start_download(sc, address, length, mode);
		if (error != 0)
			goto release;

		error = mtk_mcu_send_firmware(sc, data + offset, length);
		if (error != 0)
			goto release;
	}

	{
		uint8_t finish[4] = { 0, 0, 0, 0 };
		replyLength = sizeof(reply);
		error = mtk_mcu_send(sc, MTK_MCU_CMD_PATCH_FINISH, MTK_MCU_Q_NA,
			finish, sizeof(finish), reply, &replyLength);
	}

release:
	mtk_put32(request, MTK_PATCH_SEM_RELEASE);
	replyLength = sizeof(reply);
	mtk_mcu_send(sc, MTK_MCU_CMD_PATCH_SEM, MTK_MCU_Q_NA, request,
		sizeof(request), reply, &replyLength);

done:
	firmware_put(image, FIRMWARE_UNLOAD);
	return error;
}


/* The code the part runs, written back to front: its table of contents is the
 * last thirty-six bytes of the file, little-endian, with the regions it counts
 * immediately before it and their contents from the beginning.
 */
static int
mtk_load_ram(struct mtk_softc* sc)
{
	const struct firmware* image;
	const uint8_t* data;
	uint32_t count, override = 0, offset = 0, i;
	int error = 0;

	image = firmware_get(MTK_RAM_NAME);
	if (image == NULL) {
		device_printf(sc->sc_dev, "no %s to be found\n", MTK_RAM_NAME);
		return ENOENT;
	}

	data = (const uint8_t*)image->data;
	if (image->datasize < MTK_RAM_TRAILER + MTK_RAM_REGION) {
		error = EINVAL;
		goto done;
	}

	count = data[image->datasize - MTK_RAM_TRAILER + 2];
	if (count == 0 || count > 16
		|| MTK_RAM_TRAILER + (uint64_t)count * MTK_RAM_REGION
			> image->datasize) {
		device_printf(sc->sc_dev, "the image says it has %u regions\n", count);
		error = EINVAL;
		goto done;
	}

	for (i = 0; i < count; i++) {
		const uint8_t* region = data + image->datasize - MTK_RAM_TRAILER
			- (uint64_t)(count - i) * MTK_RAM_REGION;
		uint32_t address = mtk_le32(region + 16);
		uint32_t length = mtk_le32(region + 20);
		uint8_t features = region[24];

		if ((features & MTK_FW_FEATURE_OVERRIDE) != 0)
			override = address;

		/* A region marked as not to be sent still takes up its place in
		 * the file; it is skipped, not removed.
		 */
		if ((features & MTK_FW_FEATURE_NOT_SENT) == 0) {
			error = mtk_mcu_start_download(sc, address, length,
				mtk_ram_mode(features));
			if (error != 0)
				goto done;

			error = mtk_mcu_send_firmware(sc, data + offset, length);
			if (error != 0)
				goto done;
		}

		offset += length;
	}

	{
		uint8_t start[8];
		uint8_t reply[256];
		size_t replyLength = sizeof(reply);

		mtk_put32(start, override != 0 ? MTK_FW_START_OVERRIDE : 0);
		mtk_put32(start + 4, override);

		error = mtk_mcu_send(sc, MTK_MCU_CMD_FW_START, MTK_MCU_Q_NA, start,
			sizeof(start), reply, &replyLength);
	}

done:
	firmware_put(image, FIRMWARE_UNLOAD);
	return error;
}


/* Ask the running firmware what it is. Among the things it says is the
 * address this radio answers to.
 */
static int
mtk_read_capability(struct mtk_softc* sc)
{
	uint8_t reply[1024];
	size_t length = sizeof(reply);
	uint32_t count, at;
	int error, i;

	error = mtk_mcu_send(sc, MTK_MCU_CE_GET_NIC_CAPAB, MTK_MCU_Q_SET, NULL, 0,
		reply, &length);
	if (error != 0)
		return error;

	if (length < MTK_MCU_RXD_SIZE + 4)
		return EIO;

	count = reply[MTK_MCU_RXD_SIZE] | (reply[MTK_MCU_RXD_SIZE + 1] << 8);
	at = MTK_MCU_RXD_SIZE + 4;

	for (i = 0; i < (int)count && at + 8 <= length; i++) {
		uint32_t tag = mtk_le32(reply + at);
		uint32_t size = mtk_le32(reply + at + 4);

		at += 8;
		if (at + size > length)
			break;

		if (tag == MTK_NIC_CAP_MAC_ADDR && size >= 6)
			memcpy(sc->sc_macaddr, reply + at, 6);
		else if (tag == MTK_NIC_CAP_PHY && size >= 12)
			sc->sc_streams = reply[at + 4];

		at += size;
	}

	return 0;
}


int
mtk_firmware_start(struct mtk_softc* sc)
{
	uint8_t power[4] = { 1, 0, 0, 0 };
	int error, i;

	/* A second claim of ownership, made through the moveable window once
	 * the rings exist. Not the one that woke the registers, and no
	 * substitute for it.
	 */
	mtk_write(sc, MTK_TOP_LPCR_HOST_BAND0, MTK_LPCR_HOST_DRV_OWN);
	for (i = 0; i < 500; i++) {
		if ((mtk_read(sc, MTK_TOP_LPCR_HOST_BAND0) & MTK_LPCR_HOST_FW_OWN)
				== 0) {
			break;
		}
		DELAY(1000);
	}

	/* Which mode the firmware comes up in, said before it is sent. */
	mtk_write(sc, MTK_SWDEF_MODE, 0);

	mtk_mcu_send(sc, MTK_MCU_CMD_NIC_POWER_CTRL, MTK_MCU_Q_NA, power,
		sizeof(power), NULL, NULL);

	for (i = 0; i < 1000; i++) {
		if ((mtk_read(sc, MTK_CONN_ON_MISC) & 0x7) == 1)
			break;
		DELAY(1000);
	}

	error = mtk_load_patch(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "the patch did not go in\n");
		return error;
	}

	error = mtk_load_ram(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "the firmware did not go in\n");
		return error;
	}

	for (i = 0; i < 1500; i++) {
		if ((mtk_read(sc, MTK_CONN_ON_MISC) & 0x3) == 0x3)
			break;
		DELAY(1000);
	}
	if (i == 1500) {
		device_printf(sc->sc_dev, "the firmware never said it had started\n");
		return ETIMEDOUT;
	}

	error = mtk_read_capability(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "the firmware would not say what it is\n");
		return error;
	}

	/* Spelled out rather than with the format this environment does not
	 * carry, which printed the letter rather than the address.
	 */
	device_printf(sc->sc_dev, "firmware running; radio "
		"%02x:%02x:%02x:%02x:%02x:%02x, %d streams\n",
		sc->sc_macaddr[0], sc->sc_macaddr[1], sc->sc_macaddr[2],
		sc->sc_macaddr[3], sc->sc_macaddr[4], sc->sc_macaddr[5],
		sc->sc_streams);

	return 0;
}
