/*
 * What the MT7922's firmware has to be told before it will carry a
 * conversation with an access point: that our address exists (DEV_INFO), that
 * it belongs to a network (BSS_INFO), which channel that network is on (the
 * RLM part of BSS_INFO), who the access point is (STA_REC, with its WTBL
 * entry), and - while the conversation is being started - that the radio is
 * to stay on that channel (a remain-on-channel of the JOIN kind).
 *
 * Every layout here is Linux's, from mt76_connac_mcu.c and mt7921/mcu.c, for
 * one station interface on band 0, with the stack doing the encryption: no
 * hardware keys, and no header translation, so frames stay 802.11 both ways.
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


/* Unified command ids and TLV tags (mt76_connac_mcu.h). */
#define UNI_DEV_INFO_UPDATE		0x01
#define UNI_BSS_INFO_UPDATE		0x02
#define UNI_STA_REC_UPDATE		0x03
#define UNI_ROC				0x27

#define DEV_INFO_ACTIVE			0
#define UNI_BSS_INFO_BASIC		0
#define UNI_BSS_INFO_RLM		2
#define UNI_BSS_INFO_QBSS		15
#define UNI_ROC_ACQUIRE			0
#define UNI_ROC_ABORT			1

#define STA_REC_BASIC			0
#define STA_REC_RA			1
#define STA_REC_STATE			7
#define STA_REC_WTBL			13
#define STA_REC_PHY			0x15

#define WTBL_GENERIC			0
#define WTBL_RX				1
#define WTBL_HDR_TRANS			6
#define WTBL_RESET_AND_SET		1

#define CONNECTION_INFRA_STA		0x00010001
#define CONNECTION_INFRA_AP		0x00010002
#define CONN_STATE_DISCONNECT		0
#define CONN_STATE_PORT_SECURE		2
#define EXTRA_INFO_VER			(1 << 0)
#define EXTRA_INFO_NEW			(1 << 1)

#define PHY_MODE_A			(1 << 0)
#define PHY_MODE_B			(1 << 1)
#define PHY_MODE_G			(1 << 2)
#define PHY_TYPE_BIT_HR_DSSS		(1 << 0)
#define PHY_TYPE_BIT_ERP		(1 << 1)
#define PHY_TYPE_BIT_OFDM		(1 << 3)

#define ROC_REQ_JOIN			0

/* Where the MAC's station table is cleared of its airtime counts. */
#define MTK_WTBL_UPDATE_BUSY_WAIT	5000


static void
put16(uint8_t* where, uint16_t value)
{
	where[0] = value & 0xff;
	where[1] = value >> 8;
}


static void
put32(uint8_t* where, uint32_t value)
{
	where[0] = value & 0xff;
	where[1] = (value >> 8) & 0xff;
	where[2] = (value >> 16) & 0xff;
	where[3] = value >> 24;
}


static uint32_t
get32(const uint8_t* where)
{
	return where[0] | (where[1] << 8) | (where[2] << 16)
		| ((uint32_t)where[3] << 24);
}


static int
mtk_is_5ghz(struct ieee80211_channel* channel)
{
	return channel != NULL && IEEE80211_IS_CHAN_5GHZ(channel);
}


/* Forget what the MAC counted for a station entry (mt7921_mac_wtbl_update
 * with ADM_COUNT_CLEAR), as Linux does whenever an entry changes hands.
 */
void
mtk_wtbl_clear(struct mtk_softc* sc, uint16_t wcid)
{
	uint32_t value;
	int i;

	value = mtk_read(sc, MTK_WTBL_UPDATE);
	value &= ~MTK_WTBL_UPDATE_INDEX_MASK;
	mtk_write(sc, MTK_WTBL_UPDATE, value | wcid | MTK_WTBL_UPDATE_CLEAR);

	for (i = 0; i < MTK_WTBL_UPDATE_BUSY_WAIT; i++) {
		if ((mtk_read(sc, MTK_WTBL_UPDATE) & MTK_WTBL_UPDATE_BUSY) == 0)
			return;
		DELAY(1);
	}

	device_printf(sc->sc_dev, "station entry %u stayed busy\n", wcid);
}


/* Our own address, and the network it will belong to, as the firmware keeps
 * them (mt76_connac_mcu_uni_add_dev): the device first, then the basic BSS
 * record with our own station entry as its broadcast entry.
 */
int
mtk_dev_add(struct mtk_softc* sc, int enable)
{
	uint8_t dev[16], bss[36];
	int error;

	memset(dev, 0, sizeof(dev));
	dev[0] = sc->sc_omac;
	dev[1] = 0;				/* band 0 */
	put16(dev + 4, DEV_INFO_ACTIVE);
	put16(dev + 6, 12);
	dev[8] = enable ? 1 : 0;
	dev[9] = 0;				/* link 0 */
	memcpy(dev + 10, sc->sc_macaddr, 6);

	memset(bss, 0, sizeof(bss));
	bss[0] = 0;				/* bss_idx */
	put16(bss + 4, UNI_BSS_INFO_BASIC);
	put16(bss + 6, 32);
	bss[8] = enable ? 1 : 0;		/* active */
	bss[9] = sc->sc_omac;			/* omac_idx */
	bss[10] = sc->sc_omac;			/* hw_bss_idx */
	bss[11] = 0;				/* band_idx */
	put32(bss + 12, CONNECTION_INFRA_STA);
	bss[16] = 1;				/* conn_state, as Linux has it */
	bss[17] = 0;				/* wmm_idx */
	put16(bss + 24, MTK_WCID_OWN);		/* bmc_tx_wlan_idx */
	put16(bss + 30, MTK_WCID_OWN);		/* sta_idx */

	if (enable) {
		error = mtk_mcu_send_uni(sc, UNI_DEV_INFO_UPDATE, dev, sizeof(dev), 1);
		if (error == 0)
			error = mtk_mcu_send_uni(sc, UNI_BSS_INFO_UPDATE, bss,
				sizeof(bss), 1);
	} else {
		error = mtk_mcu_send_uni(sc, UNI_BSS_INFO_UPDATE, bss, sizeof(bss), 1);
		if (error == 0)
			error = mtk_mcu_send_uni(sc, UNI_DEV_INFO_UPDATE, dev,
				sizeof(dev), 1);
	}

	MTK_RESULT(sc, error, "our address %s the firmware: %d\n",
		enable ? "given to" : "taken from", error);
	return error;
}


/* The channel the network is on (mt76_connac_mcu_uni_set_chctx), 20 MHz. */
static int
mtk_bss_channel(struct mtk_softc* sc, struct ieee80211_channel* channel)
{
	uint8_t rlm[20];
	uint8_t number = ieee80211_chan2ieee(&sc->sc_ic, channel);

	memset(rlm, 0, sizeof(rlm));
	rlm[0] = 0;				/* bss_idx */
	put16(rlm + 4, UNI_BSS_INFO_RLM);
	put16(rlm + 6, 16);
	rlm[8] = number;			/* control channel */
	rlm[9] = number;			/* centre */
	rlm[10] = 0;
	rlm[11] = 0;				/* 20 MHz */
	rlm[12] = sc->sc_streams != 0 ? sc->sc_streams : 1;
	rlm[13] = (1 << (sc->sc_streams != 0 ? sc->sc_streams : 1)) - 1;
	rlm[14] = 1;				/* short slot */
	rlm[15] = 0;				/* no HT 40 */
	rlm[16] = 0;				/* no secondary channel */
	rlm[17] = mtk_is_5ghz(channel) ? 1 : 0;	/* nl80211 band */

	return mtk_mcu_send_uni(sc, UNI_BSS_INFO_UPDATE, rlm, sizeof(rlm), 1);
}


/* The network we have joined (mt76_connac_mcu_uni_add_bss): who it is, how
 * often it beacons, whether it does QoS, and then its channel. Sent when the
 * association has been accepted.
 */
int
mtk_bss_update(struct mtk_softc* sc, struct ieee80211_node* ni, int enable)
{
	struct ieee80211_channel* channel = ni != NULL ? ni->ni_chan : NULL;
	uint8_t bss[44];
	int error, five = mtk_is_5ghz(channel);

	/* Leaving needs nothing the node has: the network is named by the
	 * address kept when its access point was added, and the node may be
	 * gone - replaced by the next network's - by the time we leave.
	 */
	if (ni == NULL && enable)
		return EINVAL;

	memset(bss, 0, sizeof(bss));
	bss[0] = 0;				/* bss_idx */
	put16(bss + 4, UNI_BSS_INFO_BASIC);
	put16(bss + 6, 32);
	bss[8] = 1;				/* active, as Linux keeps it */
	bss[9] = sc->sc_omac;
	bss[10] = sc->sc_omac;			/* hw_bss_idx */
	bss[11] = 0;
	put32(bss + 12, CONNECTION_INFRA_STA);
	bss[16] = enable ? 0 : 1;		/* conn_state = !enable */
	bss[17] = 0;				/* wmm_idx */
	memcpy(bss + 18, ni != NULL ? ni->ni_bssid : sc->sc_ap_bssid, 6);
	put16(bss + 24, MTK_WCID_OWN);		/* bmc_tx_wlan_idx */
	if (ni != NULL) {
		put16(bss + 26, ni->ni_intval);	/* beacon interval */
		bss[28] = ni->ni_dtim_period != 0 ? ni->ni_dtim_period : 1;
	}
	bss[29] = five ? PHY_MODE_A : (PHY_MODE_B | PHY_MODE_G);
	put16(bss + 30, MTK_WCID_OWN);		/* sta_idx */
	put16(bss + 32, five ? PHY_TYPE_BIT_OFDM
		: (PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP));

	put16(bss + 36, UNI_BSS_INFO_QBSS);
	put16(bss + 38, 8);
	bss[40] = ni != NULL && (ni->ni_flags & IEEE80211_NODE_QOS) != 0;

	error = mtk_mcu_send_uni(sc, UNI_BSS_INFO_UPDATE, bss, sizeof(bss), 1);
	if (error != 0) {
		device_printf(sc->sc_dev, "network record (%s): %d\n",
			enable ? "joining" : "leaving", error);
	} else if (enable) {
		error = mtk_bss_channel(sc, channel);
		if (error != 0) {
			device_printf(sc->sc_dev, "network channel %u: %d\n",
				ieee80211_chan2ieee(&sc->sc_ic, channel), error);
		}
	}

	MTK_RESULT(sc, error, "network %s: %d\n",
		enable ? "joined" : "left", error);
	return error;
}


/* The access point's rates, the stack's way (500 kbit/s units) to the part's
 * (a bit per rate in mt76_rates order: the four CCK ones, then the eight
 * OFDM ones; 5 GHz has only the OFDM ones, from bit 0).
 */
static void
mtk_rate_bitmaps(const struct ieee80211_rateset* rates, int five,
	uint16_t* supported, uint16_t* basic)
{
	static const uint8_t order[12] = {
		2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
	};
	int i, j;

	*supported = 0;
	*basic = 0;

	for (i = 0; i < rates->rs_nrates; i++) {
		uint8_t rate = rates->rs_rates[i] & IEEE80211_RATE_VAL;

		for (j = five ? 4 : 0; j < 12; j++) {
			int bit = five ? j - 4 : j;

			if (order[j] != rate)
				continue;
			*supported |= 1 << bit;
			if ((rates->rs_rates[i] & IEEE80211_RATE_BASIC) != 0)
				*basic |= 1 << bit;
		}
	}
}


/* The access point's station record and its WTBL entry
 * (mt76_connac_mcu_sta_cmd), or, with no node, our own entry's
 * (mt7921_mcu_sta_update with no station, sent once associated).
 */
int
mtk_sta_update(struct mtk_softc* sc, struct ieee80211_node* ni,
	uint16_t wcid, int state, int enable, int newly)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ieee80211vap* vap = TAILQ_FIRST(&ic->ic_vaps);
	uint8_t m[160];
	size_t at, wtbl, header, nested;
	uint16_t tlvs = 0, wtlvs = 0;
	const uint8_t* bssid;
	int error, qos = 0, five;
	uint16_t aid = 0;

	if (vap == NULL || vap->iv_bss == NULL)
		return ENXIO;

	bssid = ni != NULL ? ni->ni_bssid : vap->iv_bss->ni_bssid;
	five = mtk_is_5ghz(ni != NULL ? ni->ni_chan : vap->iv_bss->ni_chan);
	if (ni != NULL) {
		qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
		aid = IEEE80211_AID(ni->ni_associd);
	}

	memset(m, 0, sizeof(m));
	m[0] = 0;				/* bss_idx */
	m[1] = wcid & 0xff;
	m[4] = 1;				/* is_tlv_append */
	m[5] = ni != NULL ? sc->sc_omac : 0xe;	/* muar_idx */
	m[6] = wcid >> 8;
	at = 8;

	if (ni != NULL) {
		put16(m + at, STA_REC_BASIC);
		put16(m + at + 2, 20);
		put32(m + at + 4, CONNECTION_INFRA_AP);
		m[at + 8] = enable ? CONN_STATE_PORT_SECURE : CONN_STATE_DISCONNECT;
		m[at + 9] = qos;
		put16(m + at + 10, aid);
		memcpy(m + at + 12, bssid, 6);
		put16(m + at + 18, EXTRA_INFO_VER
			| (newly && enable ? EXTRA_INFO_NEW : 0));
		at += 20;
		tlvs++;

		if (enable) {
			uint16_t supported, basic;
			int rssi = ic->ic_node_getrssi(ni);
			int dbm = rssi / 2 + MTK_NOISE_FLOOR;

			mtk_rate_bitmaps(&ni->ni_rates, five, &supported, &basic);

			put16(m + at, STA_REC_PHY);
			put16(m + at + 2, 12);
			put16(m + at + 4, basic);
			m[at + 6] = five ? PHY_TYPE_BIT_OFDM
				: (PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP);
			m[at + 9] = (uint8_t)(2 * dbm + 220);	/* rcpi */
			at += 12;
			tlvs++;

			put16(m + at, STA_REC_RA);
			put16(m + at + 2, 16);
			if (five)
				put16(m + at + 4, (supported & 0xff) << 6);
			else {
				put16(m + at + 4, (((supported >> 4) & 0xff) << 6)
					| (supported & 0xf));
			}
			at += 16;
			tlvs++;

			put16(m + at, STA_REC_STATE);
			put16(m + at + 2, 12);
			m[at + 8] = state;
			at += 12;
			tlvs++;
		}
	}

	/* The WTBL entry, nested: a container TLV, a header, then its own
	 * TLVs, with the container's length growing to cover them all.
	 */
	wtbl = at;
	put16(m + at, STA_REC_WTBL);
	at += 4;
	tlvs++;
	header = at;
	m[at] = wcid & 0xff;
	m[at + 1] = WTBL_RESET_AND_SET;
	m[at + 4] = wcid >> 8;
	at += 8;
	nested = at;

	if (enable) {
		put16(m + at, WTBL_GENERIC);
		put16(m + at + 2, 20);
		memcpy(m + at + 4, bssid, 6);
		m[at + 10] = ni != NULL ? sc->sc_omac : 0xe;	/* muar_idx */
		m[at + 13] = qos;
		put16(m + at + 16, aid);			/* partial_aid */
		at += 20;
		wtlvs++;

		put16(m + at, WTBL_RX);
		put16(m + at + 2, 12);
		m[at + 5] = 1;					/* rca1 */
		m[at + 6] = 1;					/* rca2 */
		m[at + 7] = 1;					/* rv */
		at += 12;
		wtlvs++;

		/* No header translation either way: the stack wants 802.11. */
		put16(m + at, WTBL_HDR_TRANS);
		put16(m + at + 2, 8);
		m[at + 4] = 1;					/* to_ds */
		m[at + 6] = 1;					/* no_rx_trans */
		at += 8;
		wtlvs++;
	}

	put16(m + header + 2, wtlvs);
	put16(m + wtbl + 2, (uint16_t)(4 + 8 + (at - nested)));
	put16(m + 2, tlvs);

	error = mtk_mcu_send_uni(sc, UNI_STA_REC_UPDATE, m, at, 1);
	MTK_RESULT(sc, error, "station entry %u: state %d, %s: %d\n", wcid,
		state, enable ? "on" : "off", error);
	return error;
}


/* The access point's record gone (mt7921_mac_sta_remove): a disconnected
 * basic record and an emptied WTBL entry, named by the address it was
 * added under - which, when the stack moves to another access point, is no
 * longer the address of any node it still has.
 */
int
mtk_sta_remove(struct mtk_softc* sc, uint16_t wcid, const uint8_t* bssid)
{
	uint8_t m[40];
	int error;

	memset(m, 0, sizeof(m));
	m[0] = 0;				/* bss_idx */
	m[1] = wcid & 0xff;
	put16(m + 2, 2);			/* two TLVs */
	m[4] = 1;				/* is_tlv_append */
	m[5] = sc->sc_omac;			/* muar_idx */
	m[6] = wcid >> 8;

	put16(m + 8, STA_REC_BASIC);
	put16(m + 10, 20);
	put32(m + 12, CONNECTION_INFRA_AP);
	m[16] = CONN_STATE_DISCONNECT;
	memcpy(m + 20, bssid, 6);
	put16(m + 26, EXTRA_INFO_VER);

	put16(m + 28, STA_REC_WTBL);
	put16(m + 30, 4 + 8);
	m[32] = wcid & 0xff;
	m[33] = WTBL_RESET_AND_SET;
	m[36] = wcid >> 8;

	error = mtk_mcu_send_uni(sc, UNI_STA_REC_UPDATE, m, sizeof(m), 1);
	MTK_RESULT(sc, error, "station entry %u removed: %d\n", wcid, error);
	return error;
}


/* Keep the radio on a channel while a join is being negotiated
 * (mt7921_mcu_set_roc, JOIN): sent without waiting, and granted by an
 * unsolicited ROC event that mtk_receive_frame records.
 */
int
mtk_roc(struct mtk_softc* sc, struct ieee80211_channel* channel,
	uint32_t milliseconds)
{
	uint8_t req[28];
	uint8_t number = ieee80211_chan2ieee(&sc->sc_ic, channel);
	uint32_t result;
	int error, i, acked = 0;

	if (sc->sc_roc_active)
		mtk_roc_abort(sc);

	sc->sc_roc_token++;
	sc->sc_roc_granted = 0;

	memset(req, 0, sizeof(req));
	put16(req + 4, UNI_ROC_ACQUIRE);
	put16(req + 6, 24);
	req[8] = 0;				/* bss_idx */
	req[9] = sc->sc_roc_token;
	req[10] = number;			/* control channel */
	req[11] = 0;				/* no secondary */
	req[12] = mtk_is_5ghz(channel) ? 2 : 1;
	req[13] = 0;				/* 20 MHz */
	req[14] = number;			/* centre */
	req[15] = 0;
	req[16] = 0;				/* 20 MHz, as the AP has it */
	req[17] = number;
	req[18] = 0;
	req[19] = ROC_REQ_JOIN;
	put32(req + 20, milliseconds);
	req[24] = 0xff;				/* either band */

	/* Not waited for as a command: the answer that matters is the grant,
	 * which comes on its own. The command's own result is still worth
	 * seeing, and is picked up below if it comes.
	 */
	error = mtk_mcu_send_uni(sc, UNI_ROC, req, sizeof(req), 0);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not ask to stay on channel %u:"
			" %d\n", number, error);
		return error;
	}
	sc->sc_roc_active = 1;

	/* Nobody else reads the rings while this waits, so read them here;
	 * mtk_receive_frame notes the grant when it goes past.
	 */
	mtx_lock(&sc->sc_rxmtx);
	for (i = 0; i < 1000; i++) {
		mtk_receive(sc);
		if (!acked && sc->sc_replyready != 0) {
			acked = 1;
			if (sc->sc_replylen >= MTK_MCU_RXD_SIZE + 8) {
				result = get32(sc->sc_reply + MTK_MCU_RXD_SIZE + 4);
				MTK_RESULT(sc, result, "stay on channel %u: asked, status"
					" %#x after %d ms\n", number, result, i);
			}
		}
		if (sc->sc_roc_granted != 0)
			break;
		DELAY(1000);
	}
	mtx_unlock(&sc->sc_rxmtx);

	MTK_RESULT(sc, sc->sc_roc_granted == 0,
		"stay on channel %u: %s after %d ms\n", number,
		sc->sc_roc_granted != 0 ? "granted" : "not granted", i);
	if (sc->sc_roc_granted == 0) {
		mtk_roc_abort(sc);
		return ETIMEDOUT;
	}
	return 0;
}


int
mtk_roc_abort(struct mtk_softc* sc)
{
	uint8_t req[16];

	if (!sc->sc_roc_active)
		return 0;

	memset(req, 0, sizeof(req));
	put16(req + 4, UNI_ROC_ABORT);
	put16(req + 6, 12);
	req[8] = 0;				/* bss_idx */
	req[9] = sc->sc_roc_token;
	req[10] = 0xff;

	sc->sc_roc_active = 0;
	sc->sc_roc_granted = 0;
	return mtk_mcu_send_uni(sc, UNI_ROC, req, sizeof(req), 0);
}
