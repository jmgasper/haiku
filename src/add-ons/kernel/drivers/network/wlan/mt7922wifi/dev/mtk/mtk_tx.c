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
#include <net80211/ieee80211_proto.h>

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


/* The rate a frame with no rate history goes at: the lowest the band has,
 * as Linux picks for this family (mt76_connac2_mac_tx_rate_val, the first
 * basic rate) - CCK 1 Mbit/s low, OFDM 6 Mbit/s high.
 */
static uint32_t
mtk_fixed_rate(struct mtk_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;

	if (ic->ic_curchan != NULL && IEEE80211_IS_CHAN_5GHZ(ic->ic_curchan))
		return (MTK_PHY_TYPE_OFDM << MTK_TX_RATE_MODE_SHIFT) | 11;

	return (MTK_PHY_TYPE_CCK << MTK_TX_RATE_MODE_SHIFT) | 0;
}


/* Which station entry a frame goes out through: the access point's, once the
 * firmware has a record of it and the frame is addressed to it, and our own
 * interface's for everything else (mt792x_tx).
 */
static uint16_t
mtk_tx_wcid(struct mtk_softc* sc, const struct ieee80211_frame* frame)
{
	if (sc->sc_ap_added
		&& IEEE80211_ADDR_EQ(frame->i_addr1, sc->sc_ap_bssid))
		return MTK_WCID_AP;

	return sc->sc_dev_added ? MTK_WCID_OWN : MTK_WCID_GLOBAL;
}


/* The description that goes ahead of a frame, as mt76_connac2_mac_write_txwi
 * writes it for an 802.11 frame with no hardware key:
 *
 * - management and control frames on the ALTX0 queue, data on the queue of
 *   its access category (the part numbers them the other way round);
 * - the header's own length and the frame's own type, not a management
 *   frame's regardless;
 * - addressed to the station entry the frame is for, from our own address;
 * - every frame at a fixed rate for now: the part does rate control only
 *   for stations it has been told the rates of.
 */
static void
mtk_write_txd(struct mtk_softc* sc, uint8_t* txd,
	const struct ieee80211_frame* frame, size_t length, int ac)
{
	uint8_t type = (frame->i_fc[0] & IEEE80211_FC0_TYPE_MASK)
		>> IEEE80211_FC0_TYPE_SHIFT;
	uint8_t subtype = (frame->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK)
		>> IEEE80211_FC0_SUBTYPE_SHIFT;
	int multicast = IEEE80211_IS_MULTICAST(frame->i_addr1);
	int data = type == (IEEE80211_FC0_TYPE_DATA >> IEEE80211_FC0_TYPE_SHIFT);
	uint32_t queue, headerLength, tid = 0;

	headerLength = ieee80211_anyhdrsize(frame);
	if (data) {
		if (ac < 0 || ac > 3)
			ac = WME_AC_BE;
		queue = MTK_LMAC_AC00 + (3 - ac);
		if (IEEE80211_QOS_HAS_SEQ(frame)) {
			const uint8_t* qos = ieee80211_getqos(__DECONST(void*, frame));
			tid = qos[0] & IEEE80211_QOS_TID;
		}
	} else
		queue = MTK_LMAC_ALTX0;

	mtk_wr32(txd, ((uint32_t)queue << 25)
		| ((uint32_t)MTK_TX_TYPE_CT << 23)
		| (uint32_t)(length + MTK_TXD_HEADER));
	mtk_wr32(txd + 4, MTK_TXD1_LONG_FORMAT
		| ((uint32_t)sc->sc_omac << 24)
		| ((tid & 0x7) << 20)
		| ((uint32_t)MTK_HDR_FORMAT_802_11 << 16)
		| (((headerLength / 2) & 0x1f) << 11)
		| (mtk_tx_wcid(sc, frame) & 0x3ff));

	/* A fixed rate carries HT control itself: the part adds none to
	 * management and control frames.
	 */
	mtk_wr32(txd + 8, MTK_TXD2_FIX_RATE | MTK_TXD2_HTC_VLD
		| (multicast ? MTK_TXD2_MULTICAST : 0)
		| ((uint32_t)(type & 0x3) << 4) | (subtype & 0xf));
	mtk_wr32(txd + 12, MTK_TXD3_BA_DISABLE | ((uint32_t)15 << 11));
	mtk_wr32(txd + 24, MTK_TXD6_FIXED_BW | (mtk_fixed_rate(sc) << 16));
	mtk_wr32(txd + 28, ((uint32_t)(type & 0x3) << 20)
		| ((uint32_t)(subtype & 0xf) << 16));
}


/* Hand one frame to the card. The description is built in the slot that
 * matches where we are on the ring, so nothing is overwritten while the card
 * is still reading it.
 */
static int mtk_send_frame_locked(struct mtk_softc*, struct mbuf*);


int
mtk_send_frame(struct mtk_softc* sc, struct mbuf* m)
{
	int error;

	mtx_lock(&sc->sc_txmtx);
	error = mtk_send_frame_locked(sc, m);
	mtx_unlock(&sc->sc_txmtx);

	return error;
}


static int
mtk_send_frame_locked(struct mtk_softc* sc, struct mbuf* m)
{
	struct mtk_ring* ring = &sc->sc_txq;
	struct ieee80211_frame* frame;
	uint8_t* slot;
	uint8_t* where;
	bus_addr_t slotPhysical;
	uint32_t* desc;
	uint16_t next, token;
	size_t length = m->m_pkthdr.len;

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

	memset(slot, 0, MTK_TXD_SIZE);
	mtk_write_txd(sc, slot, frame, length, M_WME_GETAC(m));

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
	uint32_t word0, word1, word2, word3, type;
	struct ieee80211_rx_stats rxs;
	size_t at, length;
	int dbm = MTK_NOISE_FLOOR + MTK_RSSI / 2;
	uint8_t channel;

	if (got < 64)
		return;

	word0 = mtk_rd32(data);
	word1 = mtk_rd32(data + 4);
	word2 = mtk_rd32(data + 8);
	word3 = mtk_rd32(data + 12);
	type = (word0 >> 27) & 0x1f;

	if (which >= 0 && which < 3)
		sc->sc_raw[which]++;

	/* An ordinary frame, or one the part has handed over dressed as
	 * something it said itself.
	 */
	if (type != MTK_RX_TYPE_NORMAL && type != MTK_RX_TYPE_NORMAL_MCU) {
		if (type != MTK_RX_TYPE_EVENT || ((word0 >> 16) & 0xf) != 1) {
			/* The end of a sweep is said unprompted, and may carry
			 * the sequence number of the command that started it -
			 * so it is recognised by what it is before it can be
			 * mistaken for the answer to some later command.
			 */
			/* The grant of a remain-on-channel: a unified event,
			 * unsolicited, whose body starts four bytes into the
			 * payload (mt7921_mcu_uni_roc_event).
			 */
			if (type == MTK_RX_TYPE_EVENT && got >= MTK_MCU_RXD_SIZE + 12
					&& data[0x1c] == MTK_MCU_UNI_EVENT_ROC
					&& (data[0x1e] & MTK_MCU_UNI_UNSOLICITED) != 0) {
				const uint8_t* grant = data + MTK_MCU_RXD_SIZE + 4;

				if (grant[5] == sc->sc_roc_token)
					sc->sc_roc_granted = 1;
				return;
			}

			if (type == MTK_RX_TYPE_EVENT && got >= MTK_MCU_RXD_SIZE
					&& data[0x1c] == MTK_MCU_EVENT_SCAN_DONE) {
				sc->sc_scan_done = 1;
				taskqueue_enqueue(sc->sc_tq, &sc->sc_work);
				return;
			}

			/* Not air, so it may be the answer a command is waiting for.
			 * Leave it where that command will find it rather than
			 * letting the command race us for the ring. Only an event
			 * can be one: transmit-done notices and transmit status
			 * arrive on the same rings, in another layout, and a byte
			 * of theirs that happens to equal the sequence number is
			 * not an answer.
			 */
			if (type == MTK_RX_TYPE_EVENT && got >= MTK_MCU_RXD_SIZE
					&& data[0x1d] == sc->sc_seq
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
		/* The first of the receive vectors carries the signal as heard
		 * by each aerial (RCPI, in half decibels above -110 dBm).
		 */
		if (at + 8 <= got) {
			uint32_t rcpi = mtk_rd32(data + at + 4) & 0xff;

			if (rcpi != 0 && rcpi != 0xff)
				dbm = ((int)rcpi - 220) / 2;
		}
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

	/* Which channel it was heard on, from the descriptor itself: the part
	 * sweeps the band on its own, so where the stack thinks the radio is
	 * says nothing about where a frame came from.
	 *
	 * And the signal, which the stack wants counted upwards from the noise
	 * floor in half decibels rather than as a negative dBm: anything under
	 * STA_RSSI_MIN it refuses to associate with, so a plausible-looking
	 * -30 once made every network in earshot unusable.
	 */
	memset(&rxs, 0, sizeof(rxs));
	rxs.r_flags = IEEE80211_R_NF | IEEE80211_R_RSSI;
	rxs.c_nf = MTK_NOISE_FLOOR;
	if (dbm < MTK_NOISE_FLOOR)
		dbm = MTK_NOISE_FLOOR;
	rxs.c_rssi = (dbm - MTK_NOISE_FLOOR) * 2;

	channel = (word3 >> 8) & 0xff;
	if (channel != 0 && channel <= 14) {
		rxs.r_flags |= IEEE80211_R_FREQ | IEEE80211_R_IEEE | IEEE80211_R_BAND;
		rxs.c_ieee = channel;
		rxs.c_band = IEEE80211_CHAN_2GHZ;
		rxs.c_freq = ieee80211_ieee2mhz(channel, IEEE80211_CHAN_2GHZ);
	} else if (channel > 14 && channel <= 180) {
		rxs.r_flags |= IEEE80211_R_FREQ | IEEE80211_R_IEEE | IEEE80211_R_BAND;
		rxs.c_ieee = channel;
		rxs.c_band = IEEE80211_CHAN_5GHZ;
		rxs.c_freq = ieee80211_ieee2mhz(channel, IEEE80211_CHAN_5GHZ);
	}

	if (!ieee80211_add_rx_params(m, &rxs)) {
		m_freem(m);
		return;
	}
	ieee80211_input_mimo_all(ic, m);
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
