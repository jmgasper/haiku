/*
 * Frames of our own, out and in.
 *
 * A frame does not travel on the ring. What travels is a sixty-four byte
 * description of it - half telling the radio how to send it, half saying
 * where it is - and the card fetches the frame from the address that
 * description carries.
 *
 * Coming the other way, every frame arrives behind a description of itself
 * whose length depends on which parts it chose to include, so where the frame
 * proper begins has to be worked out rather than assumed.
 *
 * Distributed under the terms of the MIT License.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
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
mtk_rd32(const uint8_t* from)
{
	return from[0] | ((uint32_t)from[1] << 8) | ((uint32_t)from[2] << 16)
		| ((uint32_t)from[3] << 24);
}


static void
mtk_wr32(uint8_t* where, uint32_t value)
{
	where[0] = value & 0xff;
	where[1] = (value >> 8) & 0xff;
	where[2] = (value >> 16) & 0xff;
	where[3] = (value >> 24) & 0xff;
}


/* Hand one frame to the card. The description is built in the slot that
 * matches where we are on the ring, so nothing is overwritten while the card
 * is still reading it.
 */
int
mtk_send_frame(struct mtk_softc* sc, struct mbuf* m)
{
	struct mtk_ring* ring = &sc->sc_txq;
	struct ieee80211_frame* frame;
	uint8_t* slot;
	uint8_t* where;
	bus_addr_t slotPhysical;
	uint32_t* desc;
	uint16_t next, token;
	size_t length = m->m_pkthdr.len;
	uint8_t subtype;

	if (length < MTK_MGMT_HEADER || length > MTK_TXBUF_SIZE - MTK_TXD_SIZE)
		return EINVAL;

	next = (ring->head + 1) % ring->count;
	if (next == ring->tail) {
		/* Let the card catch up before deciding the ring is full. */
		uint32_t at = mtk_read(sc, ring->regs + MTK_RING_DMA_INDEX);
		if (at < ring->count)
			ring->tail = at;
		if (next == ring->tail)
			return ENOBUFS;
	}

	slot = (uint8_t*)sc->sc_txbuf.addr + (size_t)ring->head * MTK_TXBUF_SIZE;
	slotPhysical = sc->sc_txbuf.paddr + (bus_addr_t)ring->head * MTK_TXBUF_SIZE;

	m_copydata(m, 0, length, (caddr_t)(slot + MTK_TXD_SIZE));
	frame = (struct ieee80211_frame*)(slot + MTK_TXD_SIZE);
	subtype = (frame->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK)
		>> IEEE80211_FC0_SUBTYPE_SHIFT;

	memset(slot, 0, MTK_TXD_SIZE);

	/* How much is being sent counts the first half of this description but
	 * not the second, which has no outward logic to it.
	 */
	mtk_wr32(slot, ((uint32_t)MTK_LMAC_ALTX0 << 25)
		| (uint32_t)(length + MTK_TXD_HEADER));
	mtk_wr32(slot + 4, MTK_TXD1_LONG_FORMAT
		| ((uint32_t)MTK_TXD1_TID_MGMT << 20)
		| ((uint32_t)MTK_HDR_FORMAT_802_11 << 16)
		| ((uint32_t)(MTK_MGMT_HEADER / 2) << 11)
		| sc->sc_peer);

	/* Anything that is not ordinary traffic goes at a rate we choose, there
	 * being no history yet to choose from.
	 */
	mtk_wr32(slot + 8, MTK_TXD2_FIX_RATE | MTK_TXD2_HTC_VLD | subtype);
	mtk_wr32(slot + 12, MTK_TXD3_BA_DISABLE | ((uint32_t)15 << 11));
	mtk_wr32(slot + 24, MTK_TXD6_FIXED_BW);
	mtk_wr32(slot + 28, (uint32_t)subtype << 16);

	/* And where the frame is: immediately behind its description. */
	where = slot + MTK_TXD_HEADER;
	memset(where, 0, MTK_TXD_HEADER);

	token = ++sc->sc_token & 0x7fff;
	where[0] = token & 0xff;
	where[1] = (token >> 8) | 0x80;

	mtk_wr32(where + 8, (uint32_t)(slotPhysical + MTK_TXD_SIZE));
	where[12] = length & 0xff;
	where[13] = (length >> 8) | 0x80;

	desc = (uint32_t*)ring->desc.addr;
	desc[ring->head * 4 + 0] = (uint32_t)slotPhysical;
	desc[ring->head * 4 + 2] = 0;
	desc[ring->head * 4 + 3] = 0;
	wmb();
	desc[ring->head * 4 + 1] = ((uint32_t)MTK_TXD_SIZE << MTK_DMA_CTL_LEN_SHIFT)
		| MTK_DMA_CTL_LAST_SEC0;

	ring->head = next;
	wmb();
	mtk_write(sc, ring->regs + MTK_RING_CPU_INDEX, ring->head);

	return 0;
}


/* One frame off the air, handed up to the stack that knows what to do with
 * it. Everything about what it means is decided there.
 */
void
mtk_receive_frame(struct mtk_softc* sc, const uint8_t* data, size_t got,
	int which, int slot)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct mbuf* m;
	uint32_t word0, word1, word2, type;
	size_t at, length;

	if (got < 64)
		return;

	word0 = mtk_rd32(data);
	word1 = mtk_rd32(data + 4);
	word2 = mtk_rd32(data + 8);
	type = (word0 >> 27) & 0x1f;

	if (which >= 0 && which < 3)
		sc->sc_raw[which]++;

	/* An ordinary frame, or one the part has handed over dressed as
	 * something it said itself.
	 */
	if (type != MTK_RX_TYPE_NORMAL && type != MTK_RX_TYPE_NORMAL_MCU) {
		if (type != MTK_RX_TYPE_EVENT || ((word0 >> 16) & 0xf) != 1) {
			/* Not air, so it may be the answer a command is waiting for.
			 * Leave it where that command will find it rather than
			 * letting the command race us for the ring.
			 */
			if (got >= MTK_MCU_RXD_SIZE && data[0x1d] == sc->sc_seq
					&& sc->sc_replyready == 0) {
				size_t keep = got > sizeof(sc->sc_reply)
					? sizeof(sc->sc_reply) : got;

				memcpy(sc->sc_reply, data, keep);
				sc->sc_replylen = keep;
				wmb();
				sc->sc_replyready = 1;
				return;
			}

			/* Refusing a frame and never saying so is how a driver hides
			 * the one kind it needs. Record which types get refused.
			 */
			sc->sc_dropped++;
			sc->sc_typemask |= 1u << type;
			return;
		}
	}
	if ((word1 & MTK_RXD1_FCS_ERROR) != 0)
		return;

	at = MTK_RXD_FIXED;
	if ((word1 & MTK_RXD1_GROUP_4) != 0)
		at += 16;
	if ((word1 & MTK_RXD1_GROUP_1) != 0)
		at += 16;
	if ((word1 & MTK_RXD1_GROUP_2) != 0)
		at += 8;
	if ((word1 & MTK_RXD1_GROUP_3) != 0) {
		at += 8;
		if ((word1 & MTK_RXD1_GROUP_5) != 0)
			at += 72;
	}
	at += 2 * ((word2 >> 14) & 0x3);

	if (at + MTK_MGMT_HEADER > got)
		return;

	length = got - at;

	/* A beacon or probe response is kept by the stack as a list of
	 * information elements, and every scan afterwards walks that list. If
	 * the frame was cut short, the walk runs off the end of it - which is
	 * not the stack's mistake to make, it is ours for handing over
	 * something that does not describe itself properly. Check the elements
	 * fit inside the frame before letting it go any further.
	 */
	{
		uint8_t kind = (data + at)[0];

		if ((kind & 0x0c) == 0x00
				&& ((kind & 0xf0) == 0x80 || (kind & 0xf0) == 0x50)) {
			size_t ie = MTK_MGMT_HEADER + 12;	/* fixed fields */

			while (ie + 2 <= length) {
				size_t next = ie + 2 + (data + at)[ie + 1];

				if (next > length)
					break;
				ie = next;
			}

			if (ie != length) {
				sc->sc_ragged++;
				return;
			}
		}
	}

	m = m_get2(length, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return;

	memcpy(mtod(m, void*), data + at, length);
	m->m_pkthdr.len = m->m_len = length;

	/* Which frames actually arrive is the question the scan list cannot
	 * answer, so count them here by what the air says they are.
	 */
	{
		const uint8_t* wh = data + at;
		uint8_t fc = wh[0];

		if ((fc & 0x0c) == 0x00) {
			sc->sc_mgmt++;
			sc->sc_subtype[(fc >> 4) & 0xf]++;
		} else if ((fc & 0x0c) == 0x08)
			sc->sc_data++;
		else
			sc->sc_ctrl++;

		/* Naming a subtype that never arrives proves nothing, so print
		 * every one of them and let the numbers say which is missing.
		 */
		/* Per ring, because the two carry different things and a shared
		 * budget only ever shows the busier one.
		 */
		if (which >= 0 && which < 3 && sc->sc_shown[which] < 4) {
			sc->sc_shown[which]++;
			device_printf(sc->sc_dev, "ring %d slot %d: rxd %08x %08x %08x,"
				" %zu bytes at %zu: %02x %02x %02x %02x %02x %02x %02x %02x"
				" %02x %02x %02x %02x %02x %02x %02x %02x\n",
				which, slot, word0, word1, word2, length, at,
				wh[0], wh[1], wh[2], wh[3], wh[4], wh[5], wh[6], wh[7],
				wh[8], wh[9], wh[10], wh[11], wh[12], wh[13], wh[14], wh[15]);
		}

		if (++sc->sc_received % 500 == 0) {
			device_printf(sc->sc_dev, "rings %u/%u/%u; %u beacons,"
				" %u probe resp; dropped %u of types"
				" %#x, %u ragged; in: %u mgmt, %u data, %u ctrl;"
				" subtypes %u %u %u %u %u %u %u %u %u %u %u %u"
				" %u %u %u %u\n",
				sc->sc_raw[0], sc->sc_raw[1], sc->sc_raw[2],
				sc->sc_subtype[8], sc->sc_subtype[5],
				sc->sc_dropped, sc->sc_typemask, sc->sc_ragged,
				sc->sc_mgmt, sc->sc_data, sc->sc_ctrl,
				sc->sc_subtype[0], sc->sc_subtype[1], sc->sc_subtype[2],
				sc->sc_subtype[3], sc->sc_subtype[4], sc->sc_subtype[5],
				sc->sc_subtype[6], sc->sc_subtype[7], sc->sc_subtype[8],
				sc->sc_subtype[9], sc->sc_subtype[10], sc->sc_subtype[11],
				sc->sc_subtype[12], sc->sc_subtype[13], sc->sc_subtype[14],
				sc->sc_subtype[15]);
		}
	}

	/* The stack wants this counted upwards from the noise floor in half
	 * decibels, not as a negative dBm: anything under STA_RSSI_MIN it
	 * refuses to associate with, so a plausible-looking -30 made every
	 * network in earshot unusable and the scan repeat for ever.
	 */
	ieee80211_input_all(ic, m, MTK_RSSI, MTK_NOISE_FLOOR);
}


/* Everything the rings hold. */
void
mtk_receive(struct mtk_softc* sc)
{
	struct mtk_ring* rings[3];
	uint8_t* frame = sc->sc_frame;
	int r;

	rings[0] = &sc->sc_dataq;
	rings[1] = &sc->sc_lateq;

	/* The part announces networks on the ring it also answers commands on.
	 * Draining only the data rings leaves every beacon sitting there, which
	 * looks exactly like a radio that cannot hear them. mtk_tick skips this
	 * entirely while a command is in flight, so no reply is taken here.
	 */
	rings[2] = &sc->sc_eventq;

	for (r = 0; r < 3; r++) {
		struct mtk_ring* ring = rings[r];
		uint32_t* desc = (uint32_t*)ring->desc.addr;
		int guard;

		if (ring->count == 0)
			continue;

		for (guard = 0; guard < ring->count; guard++) {
			size_t got;

			if ((desc[ring->tail * 4 + 1] & MTK_DMA_CTL_DMA_DONE) == 0)
				break;

			rmb();
			got = (desc[ring->tail * 4 + 1] & MTK_DMA_CTL_LEN_MASK)
				>> MTK_DMA_CTL_LEN_SHIFT;
			if (got > MTK_RX_BUFFER_SIZE)
				got = MTK_RX_BUFFER_SIZE;

			memcpy(frame, (const uint8_t*)ring->buffers.addr
				+ (size_t)ring->tail * MTK_RX_BUFFER_SIZE, got);

			/* Give the descriptor back before looking at what was in
			 * it, so the card is never waiting on us.
			 */
			desc[ring->tail * 4 + 2] = 0;
			desc[ring->tail * 4 + 3] = 0;
			wmb();
			desc[ring->tail * 4 + 1] = (uint32_t)MTK_RX_BUFFER_SIZE
				<< MTK_DMA_CTL_LEN_SHIFT;

			ring->head = ring->tail;
			ring->tail = (ring->tail + 1) % ring->count;

			wmb();
			mtk_write(sc, ring->regs + MTK_RING_CPU_INDEX, ring->head);

			mtk_receive_frame(sc, frame, got, r, (int)ring->head);
		}
	}
}
