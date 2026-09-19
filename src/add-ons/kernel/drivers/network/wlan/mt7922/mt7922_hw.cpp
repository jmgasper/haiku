/*
 * Reaching, waking and resetting a MediaTek MT7922.
 *
 * Three things about this part make it awkward to talk to, and each of them
 * looks like one of the others when it goes wrong.
 *
 * Its window onto the bus can sit above four gigabytes, so it is written as
 * two registers rather than one. Take only the first and the address that
 * comes out looks entirely reasonable, points into memory instead of at the
 * card, and reads back as zeroes.
 *
 * Its registers belong to its own firmware until the host asks for them, and
 * a chip that has not been asked also reads back as zeroes. Worse, reading
 * through the remapping window before asking does not return at all, and takes
 * the machine with it - so the asking has to come first, and is done through a
 * register that is always reachable without the window.
 *
 * And most of the chip is not in the window at all. Addresses are reached
 * three ways: directly, through a fixed table of blocks, or by pointing a
 * single moveable window at them - which means one lock around the whole of
 * point-then-read.
 *
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>

#include "mt7922.h"


#define TRACE(x...)	dprintf("mt7922: " x)
#define ERROR(x...)	dprintf("mt7922: " x)


/* Blocks of the chip that appear at a fixed place in the window, needing no
 * window to be pointed. That the register which hands over ownership is one of
 * these is what makes it safe to reach first.
 */
struct fixed_block {
	uint32	physical;
	uint32	mapped;
	uint32	size;
};

static const fixed_block kFixedBlocks[] = {
	{ 0x7c000000, 0x0f0000, 0x10000 },	/* CONN_INFRA */
	{ 0x7c020000, 0x0d0000, 0x10000 },	/* CONN_INFRA, WFDMA */
	{ 0x7c060000, 0x0e0000, 0x10000 },	/* CONN_INFRA, host CSR */
	{ 0x00400000, 0x080000, 0x10000 },	/* MCU system RAM */
	{ 0x00410000, 0x090000, 0x10000 },	/* MCU system RAM, configuration */
	{ 0x40000000, 0x070000, 0x10000 },	/* UMAC system RAM */
	{ 0x54000000, 0x002000, 0x01000 },	/* WFDMA, MCU DMA 0 */
	{ 0x74030000, 0x010000, 0x10000 },	/* PCIe MAC */
};


static inline uint32
raw_read32(mt7922_dev* device, uint32 offset)
{
	return *(volatile uint32*)(device->registers + offset);
}


static inline void
raw_write32(mt7922_dev* device, uint32 offset, uint32 value)
{
	*(volatile uint32*)(device->registers + offset) = value;
}


/* Where in the window an address of the chip's can be reached, pointing the
 * window at it if that is what it takes. The caller holds the window lock for
 * as long as it intends to use what comes back.
 */
static uint32
mt7922_translate(mt7922_dev* device, uint32 address)
{
	if (address < MT_DIRECT_LIMIT)
		return address;

	for (size_t i = 0; i < B_COUNT_OF(kFixedBlocks); i++) {
		if (address < kFixedBlocks[i].physical)
			continue;

		uint32 offset = address - kFixedBlocks[i].physical;
		if (offset >= kFixedBlocks[i].size)
			continue;

		return kFixedBlocks[i].mapped + offset;
	}

	/* Everything else is reached by moving the window. Its upper half names
	 * the block and goes into the low half of the control register; its lower
	 * half is the offset within the block.
	 */
	uint32 control = raw_read32(device, MT_HIF_REMAP_L1);
	control = (control & ~MT_HIF_REMAP_L1_MASK) | (address >> 16);
	raw_write32(device, MT_HIF_REMAP_L1, control);

	/* The write that moves the window is posted, and reading through a window
	 * that has not finished moving reads the old one. Reading the control
	 * register back is what makes it land.
	 */
	(void)raw_read32(device, MT_HIF_REMAP_L1);

	return MT_HIF_REMAP_BASE_L1 + (address & 0xffff);
}


uint32
mt7922_read32(mt7922_dev* device, uint32 address)
{
	MutexLocker locker(device->windowLock);
	return raw_read32(device, mt7922_translate(device, address));
}


void
mt7922_write32(mt7922_dev* device, uint32 address, uint32 value)
{
	MutexLocker locker(device->windowLock);
	raw_write32(device, mt7922_translate(device, address), value);
}


/* Wait for some bits of a register to say what we want. The window may move
 * between reads, so each read translates afresh.
 */
static bool
mt7922_poll(mt7922_dev* device, uint32 address, uint32 mask, uint32 wanted,
	bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		if ((mt7922_read32(device, address) & mask) == wanted)
			return true;
		if (system_time() >= deadline)
			return false;
		snooze(1000);
	}
}


/* Take the registers from the chip's firmware.
 *
 * The chip is first pushed into firmware-own whatever state it was found in,
 * so what follows is a handover from a known place rather than a guess about
 * where it started - it may have been left anywhere by a previous driver, or
 * by the Bluetooth half of the same die.
 */
static status_t
mt7922_take_ownership(mt7922_dev* device)
{
	const int kRetries = 10;
	int i;

	for (i = 0; i < kRetries; i++) {
		mt7922_write32(device, MT_CONN_ON_LPCTL, LPCTL_HOST_SET_OWN);
		if (mt7922_poll(device, MT_CONN_ON_LPCTL, LPCTL_HOST_OWN_SYNC,
				LPCTL_HOST_OWN_SYNC, 50000)) {
			break;
		}
	}
	if (i == kRetries) {
		ERROR("the firmware would not take its registers back\n");
		return B_TIMED_OUT;
	}

	for (i = 0; i < kRetries; i++) {
		mt7922_write32(device, MT_CONN_ON_LPCTL, LPCTL_HOST_CLR_OWN);

		/* A link allowed to go to sleep needs a moment before it answers. */
		snooze(3000);

		if (mt7922_poll(device, MT_CONN_ON_LPCTL, LPCTL_HOST_OWN_SYNC, 0,
				50000)) {
			device->owned = true;
			return B_OK;
		}
	}

	ERROR("the firmware would not hand its registers over\n");
	return B_TIMED_OUT;
}


static void
mt7922_release_ownership(mt7922_dev* device)
{
	if (!device->owned)
		return;

	for (int i = 0; i < 10; i++) {
		mt7922_write32(device, MT_CONN_ON_LPCTL, LPCTL_HOST_SET_OWN);
		if (mt7922_poll(device, MT_CONN_ON_LPCTL, LPCTL_HOST_OWN_SYNC,
				LPCTL_HOST_OWN_SYNC, 50000)) {
			break;
		}
	}
	device->owned = false;
}


/* Put the Wi-Fi subsystem through its own reset. This touches only the Wi-Fi
 * side: the Bluetooth half of the die keeps working across it.
 */
static status_t
mt7922_reset_subsystem(mt7922_dev* device)
{
	uint32 value = mt7922_read32(device, MT_WFSYS_SW_RST_B);

	mt7922_write32(device, MT_WFSYS_SW_RST_B, value & ~WFSYS_SW_RST_B);
	snooze(50000);
	mt7922_write32(device, MT_WFSYS_SW_RST_B,
		mt7922_read32(device, MT_WFSYS_SW_RST_B) | WFSYS_SW_RST_B);

	if (!mt7922_poll(device, MT_WFSYS_SW_RST_B, WFSYS_SW_INIT_DONE,
			WFSYS_SW_INIT_DONE, 500000)) {
		ERROR("the Wi-Fi subsystem never said it had started\n");
		return B_TIMED_OUT;
	}

	return B_OK;
}


status_t
mt7922_setup(mt7922_dev* device)
{
	pci_info& pci = device->pci;

	mutex_init(&device->windowLock, "mt7922 register window");

	/* A device nobody has claimed is left switched off: it answers no reads
	 * of its registers without memory space, and moves nothing by itself
	 * without being allowed to drive the bus.
	 */
	uint16 command = gPci->read_pci_config(pci.bus, pci.device, pci.function,
		PCI_command, 2);
	uint16 wanted = command | PCI_command_memory | PCI_command_master;
	if (wanted != command) {
		gPci->write_pci_config(pci.bus, pci.device, pci.function, PCI_command,
			2, wanted);
	}

	/* The window can sit above four gigabytes, and then it is two registers
	 * wide, with the upper half in the slot after the lower.
	 */
	phys_addr_t address = pci.u.h0.base_registers[0];
	if ((pci.u.h0.base_register_flags[0] & PCI_address_type)
			== PCI_address_type_64) {
		address |= (uint64)pci.u.h0.base_registers[1] << 32;
	}

	device->registersSize = pci.u.h0.base_register_sizes[0];
	device->registersArea = map_physical_memory("mt7922 registers", address,
		device->registersSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&device->registers);

	if (device->registersArea < B_OK) {
		ERROR("cannot reach the card's registers: %s\n",
			strerror(device->registersArea));
		mutex_destroy(&device->windowLock);
		return device->registersArea;
	}

	TRACE("registers at %#" B_PRIxPHYSADDR ", %" B_PRIuSIZE " bytes\n",
		address, device->registersSize);

	/* Nothing may be read through the window until this has been done. */
	status_t status = mt7922_take_ownership(device);
	if (status != B_OK)
		goto unmap;

	device->chipId = mt7922_read32(device, MT_HW_CHIPID) & 0xffff;
	device->revision = mt7922_read32(device, MT_HW_REV);

	TRACE("MT%04" B_PRIx32 ", revision %#" B_PRIx32 "\n", device->chipId,
		device->revision & 0xff);

	if (device->chipId != MT7922_CHIP_7922 && device->chipId != MT7922_CHIP_7921
		&& device->chipId != MT7922_CHIP_7920
		&& device->chipId != MT7922_CHIP_7902) {
		ERROR("MT%04" B_PRIx32 " is not a part this knows\n", device->chipId);
		status = B_DEV_INVALID_PIPE;
		goto release;
	}

	status = mt7922_reset_subsystem(device);
	if (status != B_OK)
		goto release;

	TRACE("the Wi-Fi subsystem is reset and running\n");

	/* Next: the transfer rings, and the firmware that goes over them. Until
	 * that is here the part is awake and addressable but does nothing.
	 */
	return B_OK;

release:
	mt7922_release_ownership(device);
unmap:
	delete_area(device->registersArea);
	device->registersArea = -1;
	device->registers = NULL;
	mutex_destroy(&device->windowLock);
	return status;
}


void
mt7922_teardown(mt7922_dev* device)
{
	if (device->registers == NULL)
		return;

	mt7922_release_ownership(device);

	delete_area(device->registersArea);
	device->registersArea = -1;
	device->registers = NULL;
	mutex_destroy(&device->windowLock);
}
