/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <sys/bus.h>
#include <sys/kernel.h>

#include <dev/pci/pcivar.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>


HAIKU_FBSD_WLAN_DRIVER_GLUE(mt7922wifi, mtk, pci);
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(FBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	/* Never ours. The card's interrupt gate is never opened - frames are
	 * collected by polling - so anything arriving on this line belongs to
	 * whatever else sits on it. Claiming it said we had masked an
	 * interrupt we then never re-enabled, which stops that line for every
	 * other device sharing it.
	 */
	return 0;
}
