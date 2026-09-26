/*
 * MediaTek MT7922 wireless.
 *
 * This is the same hardware knowledge as the native driver beside it, in the
 * shape the wireless stack wants: a device that probes and attaches, so that
 * net80211 can be told about it and the things above net80211 - the ones that
 * already know how to join a network and keep a key - can drive it.
 *
 * Distributed under the terms of the MIT License.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>

#include <driver_settings.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ratectl.h>
#include <net80211/ieee80211_scan.h>

#include "if_mtkvar.h"


/* Blocks of the chip that appear at a fixed place in the window, needing no
 * window to be aimed. That the register which hands over ownership is one of
 * these is what makes it reachable before anything else is.
 */
struct mtk_fixed {
	uint32_t	physical;
	uint32_t	mapped;
	uint32_t	size;
};

static void mtk_tick(void* arg);
static void mtk_work(void* arg, int pending);
static void mtk_rxwork(void* arg, int pending);


/* Linux's table for this family (mt7921/pci.c, __mt7921_reg_addr), entry for
 * entry. Everything the driver touches above the first megabyte is either
 * here or in one of the three ranges the L1 window is for; nothing else is
 * ever sent through the window.
 */
static const struct mtk_fixed mtk_fixed_map[] = {
	{ 0x820d0000, 0x30000, 0x10000 },	/* WF_LMAC_TOP (WF_WTBLON) */
	{ 0x820ed000, 0x24800, 0x00800 },	/* BN0 WF_MIB */
	{ 0x820e4000, 0x21000, 0x00400 },	/* BN0 WF_TMAC */
	{ 0x820e7000, 0x21e00, 0x00200 },	/* BN0 WF_DMA */
	{ 0x820eb000, 0x24200, 0x00400 },	/* BN0 WF_LPON */
	{ 0x820e2000, 0x20800, 0x00400 },	/* BN0 WF_AGG */
	{ 0x820e3000, 0x20c00, 0x00400 },	/* BN0 WF_ARB */
	{ 0x820e5000, 0x21400, 0x00800 },	/* BN0 WF_RMAC */
	{ 0x00400000, 0x80000, 0x10000 },	/* WF_MCU_SYSRAM */
	{ 0x00410000, 0x90000, 0x10000 },	/* WF_MCU_SYSRAM (configure) */
	{ 0x40000000, 0x70000, 0x10000 },	/* WF_UMAC_SYSRAM */
	{ 0x54000000, 0x02000, 0x01000 },	/* WFDMA PCIE0 MCU DMA0 */
	{ 0x55000000, 0x03000, 0x01000 },	/* WFDMA PCIE0 MCU DMA1 */
	{ 0x58000000, 0x06000, 0x01000 },	/* WFDMA PCIE1 MCU DMA0 */
	{ 0x59000000, 0x07000, 0x01000 },	/* WFDMA PCIE1 MCU DMA1 */
	{ 0x7c000000, 0xf0000, 0x10000 },	/* CONN_INFRA */
	{ 0x7c020000, 0xd0000, 0x10000 },	/* CONN_INFRA, WFDMA */
	{ 0x7c060000, 0xe0000, 0x10000 },	/* CONN_INFRA, conn_host_csr_top */
	{ 0x80020000, 0xb0000, 0x10000 },	/* WF_TOP_MISC_OFF */
	{ 0x81020000, 0xc0000, 0x10000 },	/* WF_TOP_MISC_ON */
	{ 0x820c0000, 0x08000, 0x04000 },	/* WF_UMAC_TOP (PLE) */
	{ 0x820c8000, 0x0c000, 0x02000 },	/* WF_UMAC_TOP (PSE) */
	{ 0x820cc000, 0x0e000, 0x01000 },	/* WF_UMAC_TOP (PP) */
	{ 0x820cd000, 0x0f000, 0x01000 },	/* WF_MDP_TOP */
	{ 0x74030000, 0x10000, 0x10000 },	/* PCIE_MAC_IREG */
	{ 0x820ce000, 0x21c00, 0x00200 },	/* WF_LMAC_TOP (WF_SEC) */
	{ 0x820cf000, 0x22000, 0x01000 },	/* WF_LMAC_TOP (WF_PF) */
	{ 0x820e0000, 0x20000, 0x00400 },	/* BN0 WF_CFG */
	{ 0x820e1000, 0x20400, 0x00200 },	/* BN0 WF_TRB */
	{ 0x820e9000, 0x23400, 0x00200 },	/* BN0 WF_WTBLOFF */
	{ 0x820ea000, 0x24000, 0x00200 },	/* BN0 WF_ETBF */
	{ 0x820ec000, 0x24600, 0x00200 },	/* BN0 WF_INT */
	{ 0x820f0000, 0xa0000, 0x00400 },	/* BN1 WF_CFG */
	{ 0x820f1000, 0xa0600, 0x00200 },	/* BN1 WF_TRB */
	{ 0x820f2000, 0xa0800, 0x00400 },	/* BN1 WF_AGG */
	{ 0x820f3000, 0xa0c00, 0x00400 },	/* BN1 WF_ARB */
	{ 0x820f4000, 0xa1000, 0x00400 },	/* BN1 WF_TMAC */
	{ 0x820f5000, 0xa1400, 0x00800 },	/* BN1 WF_RMAC */
	{ 0x820f7000, 0xa1e00, 0x00200 },	/* BN1 WF_DMA */
	{ 0x820f9000, 0xa3400, 0x00200 },	/* BN1 WF_WTBLOFF */
	{ 0x820fa000, 0xa4000, 0x00200 },	/* BN1 WF_ETBF */
	{ 0x820fb000, 0xa4200, 0x00400 },	/* BN1 WF_LPON */
	{ 0x820fc000, 0xa4600, 0x00200 },	/* BN1 WF_INT */
	{ 0x820fd000, 0xa4800, 0x00800 },	/* BN1 WF_MIB */
};

static const struct mtk_part {
	uint16_t	vendor;
	uint16_t	device;
	const char*	name;
} mtk_parts[] = {
	{ MTK_VENDOR_MEDIATEK, 0x7922, "MediaTek MT7922 802.11ax" },
	{ MTK_VENDOR_MEDIATEK, 0x7961, "MediaTek MT7921 802.11ax" },
	{ MTK_VENDOR_MEDIATEK, 0x0616, "MediaTek MT7922 802.11ax" },
	{ 0, 0, NULL }
};


static uint32_t
mtk_raw_read(struct mtk_softc* sc, uint32_t offset)
{
	return bus_space_read_4(sc->sc_st, sc->sc_sh, offset);
}


static void
mtk_raw_write(struct mtk_softc* sc, uint32_t offset, uint32_t value)
{
	bus_space_write_4(sc->sc_st, sc->sc_sh, offset, value);
}


/* Where in the window an address of the chip's can be reached, aiming the
 * window if that is what it takes.
 */
static uint32_t
mtk_translate(struct mtk_softc* sc, uint32_t address)
{
	uint32_t control;
	int i;

	if (address < MTK_DIRECT_LIMIT)
		return address;

	for (i = 0; i < (int)nitems(mtk_fixed_map); i++) {
		uint32_t offset;

		if (address < mtk_fixed_map[i].physical)
			continue;
		offset = address - mtk_fixed_map[i].physical;
		if (offset >= mtk_fixed_map[i].size)
			continue;

		return mtk_fixed_map[i].mapped + offset;
	}

	/* The window is for these three ranges and nothing else. Aiming it
	 * anywhere else is not something the part is known to survive, so an
	 * address with no place here is refused, as Linux refuses it, and the
	 * harmless register at offset zero is read instead.
	 */
	if (!((address >= 0x18000000 && address < 0x18c00000)
			|| (address >= 0x70000000 && address < 0x78000000)
			|| (address >= 0x7c000000 && address < 0x7c400000))) {
		if (sc->sc_badaddr++ < 8) {
			device_printf(sc->sc_dev, "no way to reach %#x; not trying\n",
				address);
		}
		return 0;
	}

	control = mtk_raw_read(sc, MTK_HIF_REMAP_L1);
	control = (control & ~MTK_HIF_REMAP_L1_MASK) | (address >> 16);
	mtk_raw_write(sc, MTK_HIF_REMAP_L1, control);

	/* The write that moves the window is posted, and reading through one
	 * that has not finished moving reads the old one.
	 */
	(void)mtk_raw_read(sc, MTK_HIF_REMAP_L1);

	return MTK_HIF_REMAP_BASE_L1 + (address & 0xffff);
}


uint32_t
mtk_read(struct mtk_softc* sc, uint32_t address)
{
	uint32_t value;

	MTK_LOCK(sc);
	value = mtk_raw_read(sc, mtk_translate(sc, address));
	MTK_UNLOCK(sc);

	return value;
}


void
mtk_write(struct mtk_softc* sc, uint32_t address, uint32_t value)
{
	MTK_LOCK(sc);
	mtk_raw_write(sc, mtk_translate(sc, address), value);
	MTK_UNLOCK(sc);
}


static int
mtk_poll(struct mtk_softc* sc, uint32_t address, uint32_t mask, uint32_t want,
	int milliseconds)
{
	int i;

	for (i = 0; i <= milliseconds; i++) {
		if ((mtk_read(sc, address) & mask) == want)
			return 1;
		DELAY(1000);
	}

	return 0;
}


/* Take the registers from the chip's own firmware. The chip is first pushed
 * into firmware-own whatever state it was found in, so what follows is a
 * handover from a known place rather than a guess about where it started.
 */
static int
mtk_take_ownership(struct mtk_softc* sc)
{
	int i;

	for (i = 0; i < 10; i++) {
		mtk_write(sc, MTK_CONN_ON_LPCTL, MTK_LPCTL_SET_OWN);
		if (mtk_poll(sc, MTK_CONN_ON_LPCTL, MTK_LPCTL_OWN_SYNC,
				MTK_LPCTL_OWN_SYNC, 50)) {
			break;
		}
	}
	if (i == 10) {
		device_printf(sc->sc_dev,
			"the firmware would not take its registers back\n");
		return ETIMEDOUT;
	}

	for (i = 0; i < 10; i++) {
		mtk_write(sc, MTK_CONN_ON_LPCTL, MTK_LPCTL_CLR_OWN);

		/* A link allowed to sleep needs a moment before it answers. */
		DELAY(3000);

		if (mtk_poll(sc, MTK_CONN_ON_LPCTL, MTK_LPCTL_OWN_SYNC, 0, 50)) {
			sc->sc_owned = 1;
			return 0;
		}
	}

	device_printf(sc->sc_dev,
		"the firmware would not hand its registers over\n");
	return ETIMEDOUT;
}


/* Check we still have the registers before touching any of them. The chip's
 * firmware can take them back - a reconfiguration puts the vap through
 * ieee80211_init, and the first command after that is where the machine
 * stops. A command sent to a part that is not listening never completes:
 * the read waiting on it does not fail, it simply never returns, which
 * stops the processor with interrupts off and the whole machine with it.
 *
 * This register is safe to read at any time. It sits in the fixed part of
 * the map, on the host side of the interface, so it answers whether or not
 * the rest of the chip is awake - which is exactly what makes it usable as
 * the thing to ask first.
 */
static int
mtk_ensure_owned(struct mtk_softc* sc)
{
	int i;

	if ((mtk_read(sc, MTK_CONN_ON_LPCTL) & MTK_LPCTL_OWN_SYNC) == 0)
		return 0;

	device_printf(sc->sc_dev, "the firmware has taken its registers back;"
		" asking for them again\n");

	for (i = 0; i < 10; i++) {
		mtk_write(sc, MTK_CONN_ON_LPCTL, MTK_LPCTL_CLR_OWN);
		DELAY(3000);
		if (mtk_poll(sc, MTK_CONN_ON_LPCTL, MTK_LPCTL_OWN_SYNC, 0, 50)) {
			device_printf(sc->sc_dev, "and has them back\n");
			return 0;
		}
	}

	device_printf(sc->sc_dev, "it will not hand them over\n");
	return ETIMEDOUT;
}


static void
mtk_release_ownership(struct mtk_softc* sc)
{
	int i;

	if (!sc->sc_owned)
		return;

	for (i = 0; i < 10; i++) {
		mtk_write(sc, MTK_CONN_ON_LPCTL, MTK_LPCTL_SET_OWN);
		if (mtk_poll(sc, MTK_CONN_ON_LPCTL, MTK_LPCTL_OWN_SYNC,
				MTK_LPCTL_OWN_SYNC, 50)) {
			break;
		}
	}

	sc->sc_owned = 0;
}


/* The channels this part will work on: every 20 MHz channel of both bands.
 * Which of them may actually be used is decided above this driver. The part
 * sweeps them all in one go and says on each frame which channel it heard
 * it on, so there is no longer any reason to keep the list short.
 */
static void
mtk_getradiocaps(struct ieee80211com* ic, int maxchans, int* nchans,
	struct ieee80211_channel chans[])
{
	uint8_t bands[IEEE80211_MODE_BYTES];

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	ieee80211_add_channel_list_2ghz(chans, maxchans, nchans,
		mtk_channels_2ghz, MTK_CHANNELS_2GHZ, bands, 0);

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11A);
	ieee80211_add_channel_list_5ghz(chans, maxchans, nchans,
		mtk_channels_5ghz, MTK_CHANNELS_5GHZ, bands, 0);
}


static int
mtk_newstate(struct ieee80211vap* vap, enum ieee80211_state state, int arg)
{
	struct mtk_vap* mvp = MTK_VAP(vap);
	struct mtk_softc* sc = vap->iv_ic->ic_softc;
	int error;

	device_printf(sc->sc_dev, "state now %s\n",
		ieee80211_state_name[state]);

	/* Leaving the scan behind: stop sweeping before the stack starts
	 * talking to the network it has chosen, or the part wanders off the
	 * channel mid-handshake.
	 */
	if (state != IEEE80211_S_SCAN && state != IEEE80211_S_INIT) {
		/* Not from here: this is the stack's thread, and the command
		 * buffer belongs to ours.
		 */
		sc->sc_want_awake = 1;
		taskqueue_enqueue(sc->sc_tq, &sc->sc_work);
	}

	error = mvp->newstate(vap, state, arg);

	device_printf(sc->sc_dev, "state %s settled (%d)\n",
		ieee80211_state_name[state], error);

	return error;
}


static struct ieee80211vap*
mtk_vap_create(struct ieee80211com* ic, const char name[IFNAMSIZ], int unit,
	enum ieee80211_opmode opmode, int flags,
	const uint8_t bssid[IEEE80211_ADDR_LEN],
	const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct mtk_vap* mvp;
	struct ieee80211vap* vap;

	if (!TAILQ_EMPTY(&ic->ic_vaps))
		return NULL;		/* one at a time is enough for now */

	mvp = malloc(sizeof(struct mtk_vap), M_80211_VAP, M_WAITOK | M_ZERO);
	vap = &mvp->vap;

	if (ieee80211_vap_setup(ic, vap, name, unit, opmode, flags, bssid) != 0) {
		free(mvp, M_80211_VAP);
		return NULL;
	}

	/* Only what a state change says, and nothing per frame or per scan
	 * entry. The stack prints those a character at a time, and the flood
	 * blocks the thread writing it while it holds the stack's own lock -
	 * which stops every card in the machine, not just this one.
	 */
	vap->iv_debug = IEEE80211_MSG_STATE | IEEE80211_MSG_AUTH
		| IEEE80211_MSG_ASSOC;

	/* The firmware sweeps the band and probes on its own; the stack is
	 * not to step through channels or send probe requests of its own.
	 */
	vap->iv_flags_ext |= IEEE80211_FEXT_SCAN_OFFLOAD;

	/* Watch the comings and goings, but let the stack decide them. */
	mvp->newstate = vap->iv_newstate;
	vap->iv_newstate = mtk_newstate;

	/* Pick a rate control scheme. Joining a network calls through
	 * vap->iv_rate->ir_node_init, which without this is a null pointer:
	 * the stack faults the moment it settles on a network to join, taking
	 * its own lock down with it. Every other card's driver does this here.
	 */
	ieee80211_ratectl_init(vap);

	ieee80211_vap_attach(vap, ieee80211_media_change, ieee80211_media_status,
		mac);
	ic->ic_opmode = opmode;

	return vap;
}


static void
mtk_vap_delete(struct ieee80211vap* vap)
{
	struct mtk_vap* mvp = MTK_VAP(vap);

	ieee80211_vap_detach(vap);
	free(mvp, M_80211_VAP);
}


/* Everything from here down is where the hardware work already done beside
 * this driver gets connected. Until it is, each says what was asked of it so
 * the order the stack asks in can be seen.
 */
static void
mtk_parent(struct ieee80211com* ic)
{
	struct mtk_softc* sc = ic->ic_softc;
	int wanted = ic->ic_nrunning > 0;

	if (wanted == sc->sc_running)
		return;

	device_printf(sc->sc_dev, "asked to be %s\n", wanted ? "up" : "down");
	sc->sc_running = wanted;

	if (wanted) {
		sc->sc_draining = 1;
		callout_reset(&sc->sc_poll, hz / 100, mtk_tick, sc);

		/* Not from here. This runs as the stack's parent task, and the
		 * stack waits for that task to finish while holding the very
		 * lock ieee80211_start_all wants. Doing it inline stops the
		 * whole stack, which takes the wired card down with it.
		 */
		sc->sc_startall = 1;
		taskqueue_enqueue(sc->sc_tq, &sc->sc_work);
	} else {
		sc->sc_draining = 0;
		sc->sc_startall = 0;
		callout_stop(&sc->sc_poll);
		device_printf(sc->sc_dev, "%u interrupts, %u frames in, %u out,"
			" %u refused\n", sc->sc_interrupts, sc->sc_received,
			sc->sc_sent, sc->sc_refused);
	}
}


/* The stack's scan is the firmware's sweep: one sweep per scan, asked for
 * from our own thread, and the scan ends when the part says the sweep is
 * done (mtk_work, on MTK_MCU_EVENT_SCAN_DONE).
 */
static void
mtk_scan_start(struct ieee80211com* ic)
{
	struct mtk_softc* sc = ic->ic_softc;

	sc->sc_scanning = 1;
	sc->sc_want_scan = 1;
	if (++sc->sc_scan_starts % 10 == 1) {
		device_printf(sc->sc_dev, "the stack has begun %u scans and"
			" finished %u\n", sc->sc_scan_starts, sc->sc_scan_ends);
	}
	taskqueue_enqueue(sc->sc_tq, &sc->sc_work);
}


static void
mtk_scan_end(struct ieee80211com* ic)
{
	struct mtk_softc* sc = ic->ic_softc;

	sc->sc_scanning = 0;
	sc->sc_want_scan = 0;
	sc->sc_scan_ends++;

	/* The stack is done with it even if the part is not, as when a scan
	 * is cancelled: tell the part too, or it carries on sweeping while
	 * the stack tries to talk to whatever it picked.
	 */
	if (sc->sc_hwscanning != 0) {
		sc->sc_want_cancel = 1;
		taskqueue_enqueue(sc->sc_tq, &sc->sc_work);
	}
}


/* The stack steps through its channel list even while the firmware does the
 * scanning; those steps mean nothing to the part and must not reach it.
 */
static void
mtk_scan_curchan(struct ieee80211_scan_state* ss, unsigned long maxdwell)
{
}


static void
mtk_scan_mindwell(struct ieee80211_scan_state* ss)
{
}


static void
mtk_set_channel(struct ieee80211com* ic)
{
	struct mtk_softc* sc = ic->ic_softc;

	/* Recorded, not done. During a scan the channel is the sweep's; after
	 * one it is the channel of the network being joined, which is set up
	 * with the rest of that network when the stack moves to it.
	 */
	if ((ic->ic_flags & IEEE80211_F_SCAN) == 0)
		sc->sc_want_channel = ieee80211_chan2ieee(ic, ic->ic_curchan);
}


static void
mtk_updateslot(struct ieee80211com* ic)
{
}


/* Who owns what, which the two ways in differ on and which getting wrong
 * frees kernel memory twice:
 *
 * - A frame we accept is ours, and so is the node reference that came with
 *   it. The frame is copied onto the ring at once, so it is finished with
 *   there and then, and ieee80211_tx_complete gives both back - which is
 *   also what runs the stack's own completion callbacks, the ones its
 *   authentication and association timeouts hang off.
 * - A frame we refuse through ic_transmit is still the stack's: it reads
 *   the node out of it and frees both itself (ieee80211_parent_xmitpkt).
 * - A frame we refuse through ic_raw_xmit is ours to free, but the node is
 *   not: ieee80211_raw_output lets go of it.
 *
 * This driver used to free the frame and the node on every path, which on
 * a full ring meant each probe request of a scan freed its node twice.
 */
static int
mtk_transmit(struct ieee80211com* ic, struct mbuf* m)
{
	struct mtk_softc* sc = ic->ic_softc;
	struct ieee80211_node* ni = (struct ieee80211_node*)m->m_pkthdr.rcvif;
	int error = mtk_send_frame(sc, m);

	if (error != 0) {
		sc->sc_refused++;
		return error;
	}

	sc->sc_sent++;
	ieee80211_tx_complete(ni, m, 0);
	return 0;
}


static int
mtk_raw_xmit(struct ieee80211_node* ni, struct mbuf* m,
	const struct ieee80211_bpf_params* params)
{
	struct mtk_softc* sc = ni->ni_ic->ic_softc;
	int error = mtk_send_frame(sc, m);

	if (error != 0) {
		sc->sc_refused++;
		m_freem(m);
		return error;
	}

	sc->sc_sent++;
	ieee80211_tx_complete(ni, m, 0);
	return 0;
}


/* A safety net, not the intended path: the rings are known to fill whether
 * or not the card raises a line, so this keeps frames moving while the
 * counters say which of the two actually did the work.
 */
/* Draining, on a thread of its own. */
static void
mtk_rxwork(void* arg, int pending)
{
	struct mtk_softc* sc = arg;

	sc->sc_beat++;
	mtk_receive(sc);
}


/* Our own thread: the one place allowed to wait for the part. */
static void
mtk_work(void* arg, int pending)
{
	struct mtk_softc* sc = arg;
	struct ieee80211com* ic = &sc->sc_ic;

	if (sc->sc_startall != 0) {
		sc->sc_startall = 0;
		ieee80211_start_all(ic);
	}

	/* Only after a state change, which is the one place the firmware
	 * might have taken the registers back. It never has, but it is the
	 * right thing to ask before a command and it costs one direct read of
	 * a register that always answers.
	 */
	if (sc->sc_want_awake != 0) {
		sc->sc_want_awake = 0;
		if (mtk_ensure_owned(sc) != 0)
			return;
		mtk_keep_awake(sc);
	}

	if (sc->sc_want_cancel != 0) {
		sc->sc_want_cancel = 0;
		if (sc->sc_hwscanning != 0) {
			mtk_cancel_scan(sc);
			sc->sc_hwscanning = 0;
		}
	}

	/* The sweep is over, one way or another: end the stack's scan, which
	 * is what lets it pick a network. Taking its lock is fine here - this
	 * thread is ours and holds nothing the stack waits on.
	 */
	if (sc->sc_scan_done != 0) {
		struct ieee80211vap* vap = TAILQ_FIRST(&ic->ic_vaps);

		sc->sc_scan_done = 0;
		sc->sc_hwscanning = 0;
		if (sc->sc_scanning != 0 && vap != NULL)
			ieee80211_scan_done(vap);
	}

	if (sc->sc_want_scan != 0 && sc->sc_hwscanning == 0) {
		sc->sc_want_scan = 0;
		if (sc->sc_scanning != 0) {
			if (mtk_hw_scan(sc) == 0) {
				sc->sc_hwscanning = 1;
				sc->sc_hwscan_at = ticks;
			} else {
				device_printf(sc->sc_dev, "the part would not sweep\n");
				sc->sc_scan_done = 1;
				taskqueue_enqueue(sc->sc_tq, &sc->sc_work);
			}
		}
	}
}


static void
mtk_tick(void* arg)
{
	struct mtk_softc* sc = arg;

	/* Draining the rings is cheap and must happen at a steady rate, so it
	 * stays here. It sends no commands and waits for nothing, so it cannot
	 * stall the thread this shares with every other compat driver.
	 *
	 * Commands go to our own thread instead. They were once enqueued from
	 * here every tick, but a task already queued is not queued twice, so
	 * every tick that landed during a nineteen-channel sweep was lost and
	 * the rings overran.
	 */
	taskqueue_enqueue(sc->sc_rxtq, &sc->sc_rxwork);

	/* A sweep the part never reports finished must not leave the stack
	 * scanning for ever.
	 */
	if (sc->sc_hwscanning != 0 && sc->sc_scan_done == 0
		&& (int)(ticks - sc->sc_hwscan_at) > MTK_HW_SCAN_TIMEOUT * hz) {
		device_printf(sc->sc_dev, "the sweep never said it was done\n");
		sc->sc_scan_done = 1;
		taskqueue_enqueue(sc->sc_tq, &sc->sc_work);
	}

	callout_reset(&sc->sc_poll, hz / 100, mtk_tick, sc);
}


/* The card says when it has something. Everything it has is taken at once,
 * because leaving any of it is how a ring stops moving.
 */
static void
mtk_intr(void* arg)
{
	struct mtk_softc* sc = arg;
	uint32_t status;

	status = mtk_read(sc, MTK_WFDMA0_HOST_INT_STA);
	if (status == 0)
		return;

	if (sc->sc_interrupts++ == 0)
		device_printf(sc->sc_dev, "the card is talking to us (%#x)\n", status);

	mtk_write(sc, MTK_WFDMA0_HOST_INT_STA, status);
	mtk_receive(sc);
}


/* Stay out of the boot unless told otherwise. While this driver can still
 * stop the machine, a boot that loads it can stop every boot after it too:
 * the network preferences, the Deskbar applet and auto configuration all
 * start scanning as soon as the card appears. So in the first two minutes
 * the card is left alone, and it is brought in afterwards by hand with
 * `rescan mt7922wifi`. "attach_at_boot true" in the driver's settings file
 * restores the ordinary behaviour once it has earned it.
 */
static int
mtk_boot_allowed(void)
{
	void* settings;
	bool allowed = false;

	if (ticks >= 120 * hz)
		return 1;

	settings = load_driver_settings("mt7922wifi");
	if (settings != NULL) {
		allowed = get_driver_boolean_parameter(settings, "attach_at_boot",
			false, true);
		unload_driver_settings(settings);
	}

	return allowed;
}


static int
mtk_probe(device_t dev)
{
	const struct mtk_part* part;

	if (!mtk_boot_allowed()) {
		device_printf(dev, "not attaching during boot; `rescan mt7922wifi`"
			" once it is up\n");
		return ENXIO;
	}

	for (part = mtk_parts; part->name != NULL; part++) {
		if (pci_get_vendor(dev) == part->vendor
			&& pci_get_device(dev) == part->device) {
			device_set_desc(dev, part->name);
			return BUS_PROBE_DEFAULT;
		}
	}

	return ENXIO;
}


static int
mtk_attach(device_t dev)
{
	struct mtk_softc* sc = device_get_softc(dev);
	struct ieee80211com* ic = &sc->sc_ic;
	int error, rid;

	sc->sc_dev = dev;
	mtx_init(&sc->sc_cmdmtx, "mtk commands", MTX_NETWORK_LOCK, MTX_DEF);
	mtx_init(&sc->sc_txmtx, "mtk transmit", MTX_NETWORK_LOCK, MTX_DEF);
	callout_init(&sc->sc_poll, 1);
	sc->sc_tq = taskqueue_create_fast("mtk_taskq", M_NOWAIT,
		taskqueue_thread_enqueue, &sc->sc_tq);
	taskqueue_start_threads(&sc->sc_tq, 1, PI_NET, "%s taskq",
		device_get_nameunit(dev));
	TASK_INIT(&sc->sc_work, 0, mtk_work, sc);

	sc->sc_rxtq = taskqueue_create_fast("mtk_rxq", M_NOWAIT,
		taskqueue_thread_enqueue, &sc->sc_rxtq);
	taskqueue_start_threads(&sc->sc_rxtq, 1, PI_NET, "%s rxq",
		device_get_nameunit(dev));
	TASK_INIT(&sc->sc_rxwork, 0, mtk_rxwork, sc);
	mtx_init(&sc->sc_mtx, device_get_nameunit(dev), MTX_NETWORK_LOCK,
		MTX_DEF);

	/* A device nobody has claimed is left switched off. */
	pci_enable_busmaster(dev);

	rid = PCIR_BAR(0);
	sc->sc_mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
		RF_ACTIVE);
	if (sc->sc_mem == NULL) {
		device_printf(dev, "cannot reach the card's registers\n");
		error = ENXIO;
		goto fail;
	}

	sc->sc_st = rman_get_bustag(sc->sc_mem);
	sc->sc_sh = rman_get_bushandle(sc->sc_mem);

	/* Nothing may be read through the window until the registers are ours;
	 * reading one before that does not return at all.
	 */
	error = mtk_take_ownership(sc);
	if (error != 0)
		goto fail;

	sc->sc_chipid = mtk_read(sc, MTK_HW_CHIPID) & 0xffff;
	sc->sc_rev = mtk_read(sc, MTK_HW_REV);

	device_printf(dev, "MT%04x, revision %#x\n", sc->sc_chipid,
		sc->sc_rev & 0xff);

	/* Until the firmware says what address this radio answers to, one is
	 * made up from the part itself, marked as locally chosen. The real one
	 * replaces it below.
	 */
	sc->sc_macaddr[0] = 0x02;
	sc->sc_macaddr[1] = 0x00;
	sc->sc_macaddr[2] = (sc->sc_chipid >> 8) & 0xff;
	sc->sc_macaddr[3] = sc->sc_chipid & 0xff;
	sc->sc_macaddr[4] = (sc->sc_rev >> 8) & 0xff;
	sc->sc_macaddr[5] = sc->sc_rev & 0xff;

	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(dev);
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_phytype = IEEE80211_T_OFDM;

	ic->ic_caps = IEEE80211_C_STA
		| IEEE80211_C_SHPREAMBLE
		| IEEE80211_C_SHSLOT
		| IEEE80211_C_WPA
		| IEEE80211_C_WME;

	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->sc_macaddr);

	mtk_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans, ic->ic_channels);

	ieee80211_ifattach(ic);

	ic->ic_getradiocaps = mtk_getradiocaps;
	ic->ic_scan_start = mtk_scan_start;
	ic->ic_scan_end = mtk_scan_end;
	ic->ic_set_channel = mtk_set_channel;
	ic->ic_scan_curchan = mtk_scan_curchan;
	ic->ic_scan_mindwell = mtk_scan_mindwell;
	ic->ic_updateslot = mtk_updateslot;
	ic->ic_raw_xmit = mtk_raw_xmit;
	ic->ic_transmit = mtk_transmit;
	ic->ic_parent = mtk_parent;
	ic->ic_vap_create = mtk_vap_create;
	ic->ic_vap_delete = mtk_vap_delete;

	error = mtk_dma_setup(sc);
	if (error != 0)
		goto fail;

	error = mtk_firmware_start(sc);
	if (error != 0)
		goto fail;

	error = mtk_radio_init(sc);
	if (error != 0)
		goto fail;

	/* Now that the card will say when it has something, listen for it. */
	rid = 0;
	sc->sc_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
		RF_ACTIVE | RF_SHAREABLE);
	if (sc->sc_irq == NULL) {
		device_printf(dev, "the card has no way to get our attention\n");
		error = ENXIO;
		goto fail;
	}

	error = bus_setup_intr(dev, sc->sc_irq, INTR_TYPE_NET | INTR_MPSAFE,
		NULL, mtk_intr, sc, &sc->sc_ih);
	if (error != 0) {
		device_printf(dev, "cannot listen for it: %d\n", error);
		goto fail;
	}

	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->sc_macaddr);

	device_printf(dev, "attached to the wireless stack\n");

	/* Next: the transfer rings and the firmware, behind these. */
	return 0;

fail:
	if (sc->sc_mem != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, PCIR_BAR(0), sc->sc_mem);
		sc->sc_mem = NULL;
	}
	mtx_destroy(&sc->sc_cmdmtx);
	mtx_destroy(&sc->sc_txmtx);
	mtx_destroy(&sc->sc_mtx);
	return error;
}


static int
mtk_detach(device_t dev)
{
	struct mtk_softc* sc = device_get_softc(dev);

	if (sc->sc_ic.ic_softc == sc)
		ieee80211_ifdetach(&sc->sc_ic);

	callout_drain(&sc->sc_poll);
	if (sc->sc_tq != NULL) {
		taskqueue_drain(sc->sc_tq, &sc->sc_work);
		taskqueue_free(sc->sc_tq);
		sc->sc_tq = NULL;
	}
	if (sc->sc_rxtq != NULL) {
		taskqueue_drain(sc->sc_rxtq, &sc->sc_rxwork);
		taskqueue_free(sc->sc_rxtq);
		sc->sc_rxtq = NULL;
	}

	if (sc->sc_ih != NULL) {
		bus_teardown_intr(dev, sc->sc_irq, sc->sc_ih);
		sc->sc_ih = NULL;
	}
	if (sc->sc_irq != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->sc_irq);
		sc->sc_irq = NULL;
	}

	mtk_dma_teardown(sc);

	if (sc->sc_mem != NULL) {
		mtk_release_ownership(sc);
		bus_release_resource(dev, SYS_RES_MEMORY, PCIR_BAR(0), sc->sc_mem);
		sc->sc_mem = NULL;
	}

	mtx_destroy(&sc->sc_cmdmtx);
	mtx_destroy(&sc->sc_txmtx);
	mtx_destroy(&sc->sc_mtx);
	return 0;
}


static device_method_t mtk_methods[] = {
	DEVMETHOD(device_probe,		mtk_probe),
	DEVMETHOD(device_attach,	mtk_attach),
	DEVMETHOD(device_detach,	mtk_detach),

	DEVMETHOD_END
};

static driver_t mtk_driver = {
	"mtk",
	mtk_methods,
	sizeof(struct mtk_softc)
};

DRIVER_MODULE(mtk, pci, mtk_driver, NULL, NULL);
