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

#include <machine/bus.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

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

static const struct mtk_fixed mtk_fixed_map[] = {
	{ 0x7c000000, 0x0f0000, 0x10000 },
	{ 0x7c020000, 0x0d0000, 0x10000 },
	{ 0x7c060000, 0x0e0000, 0x10000 },
	{ 0x00400000, 0x080000, 0x10000 },
	{ 0x00410000, 0x090000, 0x10000 },
	{ 0x40000000, 0x070000, 0x10000 },
	{ 0x54000000, 0x002000, 0x01000 },
	{ 0x74030000, 0x010000, 0x10000 },
	{ 0x820cd000, 0x00f000, 0x01000 },
	{ 0x820d4000, 0x034000, 0x01000 },
	{ 0x820e2000, 0x020800, 0x00400 },
	{ 0x820e3000, 0x020c00, 0x00400 },
	{ 0x820e4000, 0x021000, 0x01000 },
	{ 0x820e5000, 0x021400, 0x01000 },
	{ 0x820e7000, 0x021e00, 0x01000 },
	{ 0x820e9000, 0x023400, 0x01000 },
	{ 0x820ed000, 0x024800, 0x01000 },
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


/* The channels this part will work on. What it may actually use is decided
 * above this driver, from what the networks it hears say about where they are.
 */
static void
mtk_getradiocaps(struct ieee80211com* ic, int maxchans, int* nchans,
	struct ieee80211_channel chans[])
{
	uint8_t bands[IEEE80211_MODE_BYTES];

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	ieee80211_add_channels_default_2ghz(chans, maxchans, nchans, bands, 0);
}


static int
mtk_newstate(struct ieee80211vap* vap, enum ieee80211_state state, int arg)
{
	struct mtk_vap* mvp = MTK_VAP(vap);
	struct mtk_softc* sc = vap->iv_ic->ic_softc;

	device_printf(sc->sc_dev, "state now %s\n",
		ieee80211_state_name[state]);

	return mvp->newstate(vap, state, arg);
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

	/* Watch the comings and goings, but let the stack decide them. */
	mvp->newstate = vap->iv_newstate;
	vap->iv_newstate = mtk_newstate;

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

	if (wanted)
		ieee80211_start_all(ic);
}


static void
mtk_scan_start(struct ieee80211com* ic)
{
	struct mtk_softc* sc = ic->ic_softc;
	device_printf(sc->sc_dev, "asked to start looking\n");
}


static void
mtk_scan_end(struct ieee80211com* ic)
{
	struct mtk_softc* sc = ic->ic_softc;
	device_printf(sc->sc_dev, "asked to stop looking\n");
}


static void
mtk_set_channel(struct ieee80211com* ic)
{
	struct mtk_softc* sc = ic->ic_softc;

	device_printf(sc->sc_dev, "asked for channel %d\n",
		ieee80211_chan2ieee(ic, ic->ic_curchan));
}


static void
mtk_updateslot(struct ieee80211com* ic)
{
}


static int
mtk_transmit(struct ieee80211com* ic, struct mbuf* m)
{
	struct mtk_softc* sc = ic->ic_softc;

	device_printf(sc->sc_dev, "asked to send ordinary traffic\n");
	m_freem(m);
	return ENXIO;
}


static int
mtk_raw_xmit(struct ieee80211_node* ni, struct mbuf* m,
	const struct ieee80211_bpf_params* params)
{
	struct mtk_softc* sc = ni->ni_ic->ic_softc;

	device_printf(sc->sc_dev, "asked to send a frame of its own\n");
	m_freem(m);
	return ENXIO;
}


static int
mtk_probe(device_t dev)
{
	const struct mtk_part* part;

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

	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->sc_macaddr);

	device_printf(dev, "attached to the wireless stack\n");

	/* Next: the transfer rings and the firmware, behind these. */
	return 0;

fail:
	if (sc->sc_mem != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, PCIR_BAR(0), sc->sc_mem);
		sc->sc_mem = NULL;
	}
	mtx_destroy(&sc->sc_mtx);
	return error;
}


static int
mtk_detach(device_t dev)
{
	struct mtk_softc* sc = device_get_softc(dev);

	if (sc->sc_ic.ic_softc == sc)
		ieee80211_ifdetach(&sc->sc_ic);

	mtk_dma_teardown(sc);

	if (sc->sc_mem != NULL) {
		mtk_release_ownership(sc);
		bus_release_resource(dev, SYS_RES_MEMORY, PCIR_BAR(0), sc->sc_mem);
		sc->sc_mem = NULL;
	}

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
