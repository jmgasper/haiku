/*
 * MediaTek MT7922 and its relatives.
 *
 * Distributed under the terms of the MIT License.
 */
#ifndef _MT7922_H_
#define _MT7922_H_

#include <KernelExport.h>
#include <PCI.h>
#include <lock.h>


#define MT7922_VENDOR_ID	0x14c3

/* The parts this reaches. They are one design with different radios on it,
 * and they differ here only in which firmware they ask for.
 */
#define MT7922_CHIP_7922	0x7922
#define MT7922_CHIP_7921	0x7961
#define MT7922_CHIP_7920	0x7920
#define MT7922_CHIP_7902	0x7902


/* Addresses inside the chip, which are not where they appear in the window.
 * See mt7922_translate() for how each of these is reached.
 */
#define MT_HW_CHIPID		0x70010200
#define MT_HW_REV		0x70010204
#define MT_HW_BOUND		0x70010020

/* Whose registers these are. The chip's own firmware holds them until the
 * host asks, and asks are made here.
 */
#define MT_CONN_ON_LPCTL	0x7c060010
#define LPCTL_HOST_SET_OWN	(1 << 0)
#define LPCTL_HOST_CLR_OWN	(1 << 1)
#define LPCTL_HOST_OWN_SYNC	(1 << 2)
	/* Set means the firmware has them; clear means we do. */

#define MT_CONN_ON_MISC		0x7c0600f0
#define MT_TOP_MISC_FW_STATE	0x00000007
#define MT_TOP_MISC2_FW_PWR_ON	(1 << 0)
#define MT_TOP_MISC2_FW_N9_RDY	0x00000003

/* The Wi-Fi subsystem's own reset. */
#define MT_WFSYS_SW_RST_B	0x18000140
#define WFSYS_SW_RST_B		(1 << 0)
#define WFSYS_SW_INIT_DONE	(1 << 4)

/* The window, and the register that points it. The upper half of the address
 * being reached goes into the LOWER half of this register, which is the one
 * thing here that is easy to get backwards.
 */
#define MT_HIF_REMAP_L1		0x000fe24c
#define MT_HIF_REMAP_BASE_L1	0x00040000
#define MT_HIF_REMAP_L1_MASK	0x0000ffff

/* Anything below this is already where it appears in the window. */
#define MT_DIRECT_LIMIT		0x00100000


struct mt7922_dev {
	pci_info	pci;
	char		name[32];

	area_id		registersArea;
	volatile uint8*	registers;
	size_t		registersSize;

	/* There is one window for the whole chip, so pointing it and then
	 * reading through it has to happen without anyone else moving it.
	 */
	mutex		windowLock;

	uint32		chipId;
	uint32		revision;

	bool		owned;
};


extern pci_module_info* gPci;

status_t mt7922_setup(mt7922_dev* device);
void mt7922_teardown(mt7922_dev* device);

uint32 mt7922_read32(mt7922_dev* device, uint32 address);
void mt7922_write32(mt7922_dev* device, uint32 address, uint32 value);

#endif	/* _MT7922_H_ */
