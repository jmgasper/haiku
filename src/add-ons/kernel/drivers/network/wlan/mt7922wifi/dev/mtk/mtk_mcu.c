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


static void mtk_modify(struct mtk_softc* sc, uint32_t address, uint32_t mask,
	uint32_t value);


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


/* Wait for the answer to the command just sent, draining every ring meanwhile
 * the way the receive thread does - which is held off while this runs, so
 * there is exactly one reader. mtk_receive_frame leaves an event carrying
 * the command's sequence number in sc_reply and handles everything else as
 * it always does.
 *
 * All three rings, not only the two answers usually come on: reading just
 * those left the data ring untouched for as long as a command waited, and
 * whatever the part said there was seen by nobody until the wait was over.
 */
static int
mtk_wait_reply(struct mtk_softc* sc, uint8_t* buffer, size_t* length,
	int milliseconds)
{
	int i, error = ETIMEDOUT;

	mtx_lock(&sc->sc_rxmtx);
	for (i = 0; i <= milliseconds; i++) {
		mtk_receive(sc);

		if (sc->sc_replyready != 0) {
			size_t got = sc->sc_replylen < *length
				? sc->sc_replylen : *length;
			uint32_t word0;

			rmb();
			memcpy(buffer, sc->sc_reply, got);
			*length = got;
			error = 0;

			word0 = mtk_le32(buffer);
			if (sc->sc_replies_shown < 12 || i >= 200) {
				sc->sc_replies_shown++;
				device_printf(sc->sc_dev, "answer after %d ms: type %u"
					" flag %u eid %#x seq %u option %#x\n", i,
					(word0 >> 27) & 0x1f, (word0 >> 16) & 0xf, buffer[0x1c],
					buffer[0x1d], buffer[0x1e]);
			}
			break;
		}

		DELAY(1000);
	}
	mtx_unlock(&sc->sc_rxmtx);

	return error;
}


/* One command. The two lengths in the header overlap - one counts everything
 * after it, the other counts from partway in - which is easy to get wrong.
 */
static int
mtk_mcu_send_locked(struct mtk_softc* sc, uint8_t command, uint8_t setQuery,
	const void* payload, size_t payloadLength, uint8_t* reply,
	size_t* replyLength)
{
	int waited;
	uint8_t* packet = (uint8_t*)sc->sc_cmdbuf.addr;
	size_t total = MTK_MCU_TXD_SIZE + payloadLength;
	uint16_t length, port;
	int error, i;

	if (total > sc->sc_cmdbuf.size)
		return EINVAL;

	sc->sc_seq = (sc->sc_seq + 1) & 0xf;
	if (sc->sc_seq == 0)
		sc->sc_seq = 1;

	/* Whatever answer is still lying where replies are left belongs to an
	 * earlier command - one sent without waiting - and must not be taken
	 * for this one's. The new sequence number keeps any later stragglers
	 * out.
	 */
	sc->sc_replyready = 0;

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

	/* The answer is left for us by whoever is draining the rings. */
	{
		size_t got = *replyLength;

		if (mtk_wait_reply(sc, reply, &got, 2000) != 0)
			return ETIMEDOUT;

		*replyLength = got;
		return 0;
	}

}


/* A unified command (MCU_UNI_CMD in Linux): the same hardware descriptor,
 * then a sixteen-byte header of a different shape, and the arguments at 48
 * rather than 64. The part always answers these (the option asks it to), so
 * the answer is always collected, and its status - the UNI_EVENT_ID_CMD_RESULT
 * body, le32 after the command id - is what is returned.
 */
static int
mtk_mcu_send_uni_locked(struct mtk_softc* sc, uint16_t command,
	const void* payload, size_t payloadLength, int wait)
{
	uint8_t* packet = (uint8_t*)sc->sc_cmdbuf.addr;
	size_t total = MTK_MCU_UNI_TXD_SIZE + payloadLength;
	uint8_t reply[128];
	size_t got = sizeof(reply);
	uint16_t length;
	int error;

	if (total > sc->sc_cmdbuf.size)
		return EINVAL;

	sc->sc_seq = (sc->sc_seq + 1) & 0xf;
	if (sc->sc_seq == 0)
		sc->sc_seq = 1;
	sc->sc_replyready = 0;

	memset(packet, 0, MTK_MCU_UNI_TXD_SIZE);
	mtk_put32(packet, (uint32_t)(total & 0xffff)
		| ((uint32_t)MTK_TX_TYPE_CMD << 23)
		| ((uint32_t)MTK_TX_MCU_PORT_RX_Q0 << 25));
	mtk_put32(packet + 4, MTK_TXD1_LONG_FORMAT
		| ((uint32_t)MTK_HDR_FORMAT_CMD << 16));

	length = (uint16_t)(total - 32);
	packet[0x20] = length & 0xff;
	packet[0x21] = length >> 8;
	packet[0x22] = command & 0xff;
	packet[0x23] = command >> 8;
	packet[0x25] = MTK_MCU_PKT_ID;
	packet[0x27] = sc->sc_seq;
	packet[0x2a] = MTK_MCU_S2D_H2N;
	packet[0x2b] = MTK_MCU_UNI_OPTION_SET_ACK;

	if (payloadLength > 0)
		memcpy(packet + MTK_MCU_UNI_TXD_SIZE, payload, payloadLength);

	error = mtk_ring_submit(sc, &sc->sc_cmdq, sc->sc_cmdbuf.paddr, total);
	if (error != 0)
		return error;

	error = mtk_ring_drain(sc, &sc->sc_cmdq, 3000);
	if (error != 0 || !wait)
		return error;

	if (mtk_wait_reply(sc, reply, &got, 3000) != 0) {
		device_printf(sc->sc_dev, "no answer to unified command %#x\n",
			command);
		return ETIMEDOUT;
	}

	/* The result event names the command it answers and how it went. */
	if (got >= MTK_MCU_RXD_SIZE + 8) {
		uint32_t status = mtk_le32(reply + MTK_MCU_RXD_SIZE + 4);

		if (reply[MTK_MCU_RXD_SIZE] != (command & 0xff) || status != 0) {
			device_printf(sc->sc_dev, "unified command %#x: answer for"
				" %#x, status %#x\n", command, reply[MTK_MCU_RXD_SIZE],
				status);
			return EIO;
		}
	}

	return 0;
}


int
mtk_mcu_send_uni(struct mtk_softc* sc, uint16_t command, const void* payload,
	size_t payloadLength, int wait)
{
	int error;

	mtx_lock(&sc->sc_cmdmtx);
	sc->sc_mcu_busy++;
	error = mtk_mcu_send_uni_locked(sc, command, payload, payloadLength,
		wait);
	sc->sc_mcu_busy--;
	mtx_unlock(&sc->sc_cmdmtx);

	return error;
}


/* Firmware payload carries no header: the raw bytes go on a ring of their own
 * and nothing is expected back.
 */
static int mtk_mcu_send_ext_locked(struct mtk_softc* sc, uint8_t extended,
	const void* payload, size_t payloadLength);


static int
mtk_mcu_send(struct mtk_softc* sc, uint8_t command, uint8_t setQuery,
	const void* payload, size_t payloadLength, uint8_t* reply,
	size_t* replyLength)
{
	int error;

	mtx_lock(&sc->sc_cmdmtx);
	sc->sc_mcu_busy++;
	error = mtk_mcu_send_locked(sc, command, setQuery, payload,
		payloadLength, reply, replyLength);
	sc->sc_mcu_busy--;
	mtx_unlock(&sc->sc_cmdmtx);

	return error;
}


static int
mtk_mcu_send_ext(struct mtk_softc* sc, uint8_t extended, const void* payload,
	size_t payloadLength)
{
	int error;

	mtx_lock(&sc->sc_cmdmtx);
	sc->sc_mcu_busy++;
	error = mtk_mcu_send_ext_locked(sc, extended, payload, payloadLength);
	sc->sc_mcu_busy--;
	mtx_unlock(&sc->sc_cmdmtx);

	return error;
}


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
mtk_probe_reply(struct mtk_softc* sc)
{
	uint8_t reply[1024];
	size_t length = sizeof(reply);

	return mtk_mcu_send(sc, MTK_MCU_CE_GET_NIC_CAPAB, MTK_MCU_Q_SET, NULL, 0,
		reply, &length);
}


/* A command carrying a second identifier beside the first, which says so
 * twice: once by setting it and once by acknowledging that it has.
 */
static int
mtk_mcu_send_ext_locked(struct mtk_softc* sc, uint8_t extended,
	const void* payload, size_t payloadLength)
{
	int waited;
	uint8_t* packet = (uint8_t*)sc->sc_cmdbuf.addr;
	size_t total = MTK_MCU_TXD_SIZE + payloadLength;
	uint16_t length, port;
	int error, i;

	if (total > sc->sc_cmdbuf.size)
		return EINVAL;

	sc->sc_seq = (sc->sc_seq + 1) & 0xf;
	if (sc->sc_seq == 0)
		sc->sc_seq = 1;

	/* Whatever answer is still lying where replies are left belongs to an
	 * earlier command - one sent without waiting - and must not be taken
	 * for this one's. The new sequence number keeps any later stragglers
	 * out.
	 */
	sc->sc_replyready = 0;

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
	packet[0x24] = MTK_MCU_CMD_EXT_CID;
	packet[0x25] = MTK_MCU_PKT_ID;
	packet[0x26] = MTK_MCU_Q_SET;
	packet[0x27] = sc->sc_seq;
	packet[0x29] = extended;
	packet[0x2a] = MTK_MCU_S2D_H2N;
	packet[0x2b] = 1;

	if (payloadLength > 0)
		memcpy(packet + MTK_MCU_TXD_SIZE, payload, payloadLength);

	error = mtk_ring_submit(sc, &sc->sc_cmdq, sc->sc_cmdbuf.paddr, total);
	if (error != 0)
		return error;

	error = mtk_ring_drain(sc, &sc->sc_cmdq, 3000);
	if (error != 0)
		return error;

	{
		uint8_t event[1024];
		size_t got = sizeof(event);

		if (mtk_wait_reply(sc, event, &got, 2000) != 0)
			return ETIMEDOUT;

		return 0;
	}

	return ETIMEDOUT;
}


static void
mtk_modify(struct mtk_softc* sc, uint32_t address, uint32_t mask,
	uint32_t value)
{
	mtk_write(sc, address, (mtk_read(sc, address) & ~mask) | value);
}


/* Tell the radio what to do with what it hears. The one that matters is the
 * longest frame it will accept, kept in two places and left at zero by the
 * firmware - at which every frame is over-length and thrown away before
 * anyone sees it, which looks exactly like a radio that hears nothing.
 */
static void
mtk_mac_init(struct mtk_softc* sc)
{
	const uint32_t frame = 1536 << 3;
	const uint32_t frameMask = 0xfff8;
	uint32_t band, i;

	mtk_modify(sc, MTK_MDP_DCR1, frameMask, frame);
	mtk_modify(sc, MTK_MDP_DCR0, 0, MTK_MDP_DCR0_DAMSDU_EN);

	for (i = 0; i < MTK_STATION_COUNT; i++) {
		int wait;

		mtk_modify(sc, MTK_WTBL_UPDATE, MTK_WTBL_UPDATE_INDEX_MASK,
			i | MTK_WTBL_UPDATE_CLEAR);
		for (wait = 0; wait < 500; wait++) {
			if ((mtk_read(sc, MTK_WTBL_UPDATE) & MTK_WTBL_UPDATE_BUSY) == 0)
				break;
			DELAY(10);
		}
	}

	for (band = 0; band < 2; band++) {
		uint32_t at = band * MTK_BAND_STRIDE;

		mtk_modify(sc, MTK_TMAC_CTCR0 + at, 0x3f, 0x3f);
		mtk_modify(sc, MTK_TMAC_CTCR0 + at, 0,
			MTK_TMAC_CTCR0_VHT_SMPDU_EN | MTK_TMAC_CTCR0_DDLMT_EN);
		mtk_modify(sc, MTK_RMAC_MIB_TIME0 + at, 0, MTK_RMAC_MIB_RXTIME_EN);
		mtk_modify(sc, MTK_RMAC_MIB_AIRTIME0 + at, 0, MTK_RMAC_MIB_RXTIME_EN);
		mtk_modify(sc, MTK_MIB_SCR1 + at, 0,
			MTK_MIB_TXDUR_EN | MTK_MIB_RXDUR_EN);
		mtk_modify(sc, MTK_DMA_DCR0 + at, frameMask, frame);
		mtk_modify(sc, MTK_DMA_DCR0 + at, MTK_DMA_DCR0_RXD_G5_EN, 0);
		mtk_modify(sc, MTK_WTBLOFF_TOP_RSCR + at, 0xc3000000, 0x03000000);
	}
}


/* Set the timings the air runs on, and in doing so let the radio transmit and
 * receive at all: the two bits that allow it are held down for the duration
 * and released at the end, and nothing else here clears them.
 */
static void
mtk_set_timing(struct mtk_softc* sc)
{
	mtk_modify(sc, MTK_ARB_SCR, 0,
		MTK_ARB_SCR_TX_DISABLE | MTK_ARB_SCR_RX_DISABLE);
	DELAY(1);

	mtk_write(sc, MTK_TMAC_CDTR, 0x003000e7);
	mtk_write(sc, MTK_TMAC_ODTR, 0x001c003c);
	mtk_write(sc, MTK_TMAC_ICR0, 0x090a0968);
	mtk_modify(sc, MTK_AGG_ACR0, 0x3fff, 0x0049);

	mtk_modify(sc, MTK_ARB_SCR,
		MTK_ARB_SCR_TX_DISABLE | MTK_ARB_SCR_RX_DISABLE, 0);
}


/* The channel the radio starts on, as mt7921_mcu_set_chan_info builds it for
 * SET_RX_PATH: control and centre channel, 20 MHz, the number of transmit
 * streams, and the receive aerials as a mask.
 */
static int
mtk_set_rx_path(struct mtk_softc* sc, uint8_t channel)
{
	uint8_t request[76];
	uint8_t streams = sc->sc_streams != 0 ? sc->sc_streams : 1;

	memset(request, 0, sizeof(request));
	request[0] = channel;
	request[1] = channel;
	request[2] = 0;				/* 20 MHz */
	request[3] = streams;
	request[4] = (1 << streams) - 1;	/* aerials, as a mask */
	request[5] = 0;				/* CH_SWITCH_NORMAL */
	request[6] = 0;				/* band 0 */
	request[10] = channel > 14 ? 1 : 0;	/* nl80211 band */

	return mtk_mcu_send_ext(sc, MTK_EXT_CMD_SET_RX_PATH, request,
		sizeof(request));
}


/* Where to listen. Said twice, in two commands taking the same description of
 * a channel and differing in one field: the first arranges the aerials and
 * wants them as a mask, the second tunes the radio and wants how many there
 * are. The wrong way round is silently accepted.
 */
int
mtk_tune(struct mtk_softc* sc, uint8_t channel)
{
	uint8_t request[76];
	int error;

	memset(request, 0, sizeof(request));
	request[0] = channel;
	request[1] = channel;
	request[3] = sc->sc_streams;
	request[4] = (1 << sc->sc_streams) - 1;

	error = mtk_mcu_send_ext(sc, MTK_EXT_CMD_SET_RX_PATH, request,
		sizeof(request));
	if (error != 0)
		return error;

	request[4] = sc->sc_streams;
	error = mtk_mcu_send_ext(sc, MTK_EXT_CMD_CHANNEL_SWITCH, request,
		sizeof(request));
	if (error != 0)
		return error;

	mtk_set_timing(sc);
	return 0;
}


/* The channels the part is told exist, and the ones the stack is told it
 * may use (mtk_getradiocaps): every 20 MHz channel of both bands, the world
 * over. Where a given one is actually allowed is decided above the driver.
 */
const uint8_t mtk_channels_2ghz[MTK_CHANNELS_2GHZ] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
};
const uint8_t mtk_channels_5ghz[MTK_CHANNELS_5GHZ] = {
	36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
	132, 136, 140, 144, 149, 153, 157, 161, 165
};


/* Which channels exist at all (mt76_connac_mcu_set_channel_domain). Without
 * this the part has no list to look through and answers a scan request by
 * doing nothing. Each channel is its number and flags, eight bytes, all of
 * the 2.4 GHz ones first.
 */
static int
mtk_set_channels(struct mtk_softc* sc)
{
	uint8_t request[12 + (MTK_CHANNELS_2GHZ + MTK_CHANNELS_5GHZ) * 8];
	size_t i, at;

	memset(request, 0, sizeof(request));
	request[0] = '0';
	request[1] = '0';
	request[4] = 0;			/* 20 and 40 MHz down low */
	request[5] = 3;			/* and everything up high */
	request[6] = 3;
	request[8] = MTK_CHANNELS_2GHZ;
	request[9] = MTK_CHANNELS_5GHZ;

	at = 12;
	for (i = 0; i < MTK_CHANNELS_2GHZ; i++, at += 8)
		request[at] = mtk_channels_2ghz[i];
	for (i = 0; i < MTK_CHANNELS_5GHZ; i++, at += 8)
		request[at] = mtk_channels_5ghz[i];

	return mtk_mcu_send(sc, MTK_MCU_CE_SET_CHAN_DOMAIN, MTK_MCU_Q_SET,
		request, sizeof(request), NULL, NULL);
}


/* Ask rather than wait. The part hands over ordinary traffic and answers to
 * authentication without being told anything, but it keeps announcements to
 * itself until a scan is running - so a driver that leaves the looking to the
 * stack hears thousands of frames and not one beacon.
 */
/* Say again that the part is not to sleep. It is told this once during
 * radio setup, but leaving a scan behind is where the firmware would other-
 * wise start saving power - and a register read while it is asleep does not
 * come back, which stops the processor with interrupts off and takes the
 * whole machine with it, debugger and keyboard included.
 */
int
mtk_keep_awake(struct mtk_softc* sc)
{
	uint8_t config[328];

	memset(config, 0, sizeof(config));
	strcpy((char*)config + 8, "KeepFullPwr 1");

	return mtk_mcu_send(sc, MTK_MCU_CE_CHIP_CONFIG, MTK_MCU_Q_SET, config,
		sizeof(config), NULL, NULL);
}


/* One sweep of every channel the part knows, the way Linux asks for one
 * (mt76_connac_mcu_hw_scan): active, for any network, each channel probed
 * twice, split so the part can come back to its own channel in between. The
 * part says it is done with a SCAN_DONE event; until then nothing else may
 * start a sweep or retune the radio.
 */
int
mtk_hw_scan(struct mtk_softc* sc)
{
	uint8_t request[MTK_SCAN_REQUEST_SIZE];

	memset(request, 0, sizeof(request));

	sc->sc_hwscan_seq = (sc->sc_hwscan_seq + 1) & 0x7f;
	if (sc->sc_hwscan_seq == 0)
		sc->sc_hwscan_seq = 1;

	request[0] = sc->sc_hwscan_seq;	/* band 0, so the top bit is clear */
	request[1] = 0;			/* on behalf of our station */
	request[2] = 1;			/* active */
	request[3] = 1 << 0;		/* for any network at all */
	request[4] = 0;			/* no names to ask after */
	request[5] = 2;			/* probed twice per channel */
	request[6] = 1 << 5;		/* split, as the MT7921 family wants */
	request[7] = 1;			/* and the later fields are meant */

	request[0x9e] = 0;		/* every channel the part knows */
	request[0x9f] = 0;

	memset(request + 0x456, 0xff, 6);	/* whichever network answers */

	return mtk_mcu_send(sc, MTK_MCU_CE_START_HW_SCAN, MTK_MCU_Q_SET,
		request, sizeof(request), NULL, NULL);
}


/* Stop the sweep that is running (mt76_connac_mcu_cancel_hw_scan). */
int
mtk_cancel_scan(struct mtk_softc* sc)
{
	uint8_t request[4];

	memset(request, 0, sizeof(request));
	request[0] = sc->sc_hwscan_seq;

	return mtk_mcu_send(sc, MTK_MCU_CE_CANCEL_HW_SCAN, MTK_MCU_Q_SET,
		request, sizeof(request), NULL, NULL);
}


int
mtk_radio_init(struct mtk_softc* sc)
{
	uint8_t buffer[4] = { 0, 1, 0, 0 };
	uint8_t protect[12];
	uint8_t config[328];
	uint8_t mac[4] = { 1, 0, 0, 0 };
	uint8_t filter[68];
	int error;

	mtk_mac_init(sc);

	error = mtk_mcu_send_ext(sc, MTK_EXT_CMD_EFUSE_BUFFER_MODE, buffer,
		sizeof(buffer));
	if (error != 0)
		return error;

	memset(protect, 0, sizeof(protect));
	protect[0] = 1;
	mtk_put32(protect + 4, 0x92b);
	mtk_put32(protect + 8, 0x2);
	error = mtk_mcu_send_ext(sc, MTK_EXT_CMD_PROTECT_CTRL, protect,
		sizeof(protect));
	if (error != 0)
		return error;

	/* Keep the part awake: the power saving it would otherwise do brings a
	 * handshake with it that nothing here is ready for.
	 */
	memset(config, 0, sizeof(config));
	strcpy((char*)config + 8, "KeepFullPwr 1");
	mtk_mcu_send(sc, MTK_MCU_CE_CHIP_CONFIG, MTK_MCU_Q_SET, config,
		sizeof(config), NULL, NULL);

	error = mtk_mcu_send_ext(sc, MTK_EXT_CMD_MAC_INIT_CTRL, mac, sizeof(mac));
	if (error != 0)
		return error;

	/* And stop throwing away what it hears. Until this is said the radio is
	 * listening but discarding, which is indistinguishable from deafness.
	 */
	memset(filter, 0, sizeof(filter));
	filter[4] = 1;
	mtk_put32(filter + 8, MTK_FILTER_ENABLE | MTK_FILTER_OTHER_BSS);
	mtk_mcu_send(sc, MTK_MCU_CE_SET_RX_FILTER, MTK_MCU_Q_SET, filter,
		sizeof(filter), NULL, NULL);

	/* And specifically stop throwing away the announcements of networks we
	 * are not part of. Without this the radio hears management frames by
	 * the thousand and not one beacon, so there is nothing to scan.
	 */
	memset(filter, 0, sizeof(filter));
	filter[4] = 2;			/* by bit, not by rule */
	mtk_put32(filter + 12, MTK_RFCR_DROP_OTHER_BEACON);
	filter[16] = 1 << 1;		/* and the bit is to be cleared */
	mtk_mcu_send(sc, MTK_MCU_CE_SET_RX_FILTER, MTK_MCU_Q_SET, filter,
		sizeof(filter), NULL, NULL);

	DELAY(20000);

	/* The firmware leaves bits set here whose meaning is not written down,
	 * and some of them discard every announcement while letting ordinary
	 * traffic through. Say plainly what is wanted: discard nothing.
	 */
	mtk_write(sc, MTK_WF_RFCR, 0);

	mtk_set_channels(sc);

	/* Start the radio on a home channel (__mt7921_start: SET_RX_PATH with
	 * the aerials as a mask), and set the air timings, which is what opens
	 * the radio's transmit and receive gate. Until this the firmware has a
	 * radio it has not been told to use, and it answers nothing about
	 * interfaces on it - DEV_INFO_UPDATE went unanswered without it.
	 */
	error = mtk_set_rx_path(sc, MTK_HOME_CHANNEL);
	if (error != 0)
		device_printf(sc->sc_dev, "the radio would not start: %d\n", error);
	mtk_set_timing(sc);

	device_printf(sc->sc_dev, "radio ready, filter %#x\n",
		mtk_read(sc, MTK_WF_RFCR));
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

	/* No L0s on the link, as Linux asks before every firmware load
	 * (mt7921e_mcu_init). Power states the link can drop into by itself
	 * are one more way for a read to stop coming back.
	 */
	mtk_modify(sc, MTK_PCIE_MAC_PM, 0, MTK_PCIE_MAC_PM_L0S_DIS);

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
