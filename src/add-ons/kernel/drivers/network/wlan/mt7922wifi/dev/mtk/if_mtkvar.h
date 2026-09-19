/*
 * MediaTek MT7922 and relatives, in the shape the wireless stack expects.
 *
 * Distributed under the terms of the MIT License.
 */
#ifndef _IF_MTKVAR_H_
#define _IF_MTKVAR_H_

#define MTK_VENDOR_MEDIATEK	0x14c3

/* Inside the chip, which is not where these appear in the window. */
#define MTK_HW_CHIPID		0x70010200
#define MTK_HW_REV		0x70010204

#define MTK_CONN_ON_LPCTL	0x7c060010
#define MTK_LPCTL_SET_OWN	(1 << 0)
#define MTK_LPCTL_CLR_OWN	(1 << 1)
#define MTK_LPCTL_OWN_SYNC	(1 << 2)

/* The window, and the register that aims it. The upper half of the address
 * being reached goes into the LOWER half of this register.
 */
#define MTK_HIF_REMAP_L1	0x000fe24c
#define MTK_HIF_REMAP_BASE_L1	0x00040000
#define MTK_HIF_REMAP_L1_MASK	0x0000ffff

#define MTK_DIRECT_LIMIT	0x00100000


struct mtk_vap {
	struct ieee80211vap	vap;
	int			(*newstate)(struct ieee80211vap *,
					enum ieee80211_state, int);
};
#define MTK_VAP(vap)	((struct mtk_vap *)(vap))


struct mtk_softc {
	struct ieee80211com	sc_ic;
	device_t		sc_dev;
	struct resource*	sc_mem;
	struct resource*	sc_irq;
	void*			sc_ih;
	bus_space_tag_t		sc_st;
	bus_space_handle_t	sc_sh;

	/* There is one window for the whole chip, so aiming it and then reading
	 * through it must not be interrupted by anyone else aiming it.
	 */
	struct mtx		sc_mtx;

	uint8_t			sc_macaddr[6];
	int			sc_running;

	uint32_t		sc_chipid;
	uint32_t		sc_rev;
	int			sc_owned;
};

#define MTK_LOCK(sc)		mtx_lock(&(sc)->sc_mtx)
#define MTK_UNLOCK(sc)		mtx_unlock(&(sc)->sc_mtx)

#endif	/* _IF_MTKVAR_H_ */
