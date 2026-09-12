/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include <Drivers.h>
#include <KernelExport.h>
#include <driver_settings.h>
#include <arch/generic/msi.h>
#include <smp.h>
#include <util/kernel_cpp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "profile.h"

using namespace Rock5MbiProbe;

extern void* gFDT;
int32 api_version = B_CUR_DRIVER_API_VERSION;

static int32 sOpen = 0;
static const char kDeviceName[] = "misc/rock5_mbi_probe";

struct ProbeCookie {
	char report[256];
	size_t length;
};

class Registers {
public:
	Registers(const char* name, phys_addr_t address, size_t size)
	{
		fArea = map_physical_memory(name, address, size,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fBase);
	}
	~Registers()
	{
		if (fArea >= B_OK)
			delete_area(fArea);
	}
	status_t InitCheck() const { return fArea < B_OK ? fArea : B_OK; }
	uint32 Read(unsigned offset) const
	{
		uint32 value = *(volatile uint32*)(fBase + offset);
		memory_full_barrier();
		return value;
	}
	void Write(unsigned offset, uint32 value)
	{
		*(volatile uint32*)(fBase + offset) = value;
		memory_full_barrier();
	}
	bool WaitClear(unsigned offset, uint32 mask)
	{
		bigtime_t deadline = system_time() + 100000;
		while ((Read(offset) & mask) != 0) {
			if (system_time() >= deadline)
				return false;
			snooze(100);
		}
		return true;
	}
private:
	area_id fArea;
	volatile uint8* fBase = nullptr;
};

struct InterruptState {
	sem_id sem;
	int32 count = 0;
	int32 cpu = -1;
};

static int32
HandleInterrupt(void* data)
{
	InterruptState* state = (InterruptState*)data;
	atomic_set(&state->cpu, smp_get_current_cpu());
	atomic_add(&state->count, 1);
	release_sem_etc(state->sem, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}

static status_t
RunProbe(ProbeCookie* cookie)
{
	Registers distributor("ROCK5 MBI test distributor", kDistributor, 0x10000);
	if (distributor.InitCheck() != B_OK)
		return distributor.InitCheck();
	const unsigned groupOffset = 0x80 + (kVector / 32) * 4;
	const unsigned enableOffset = 0x100 + (kVector / 32) * 4;
	const unsigned pendingOffset = 0x200 + (kVector / 32) * 4;
	const unsigned activeOffset = 0x300 + (kVector / 32) * 4;
	uint32 typer = distributor.Read(4);
	uint32 pidr2 = distributor.Read(0xffe8);
	uint32 group = distributor.Read(groupOffset);
	uint32 enabled = distributor.Read(enableOffset);
	uint32 pending = distributor.Read(pendingOffset);
	uint32 active = distributor.Read(activeOffset);
	dprintf("ROCK5_MBI_REGISTERS typer=%#" B_PRIx32 " pidr2=%#" B_PRIx32
		" group=%#" B_PRIx32 " enabled=%#" B_PRIx32 " pending=%#" B_PRIx32
		" active=%#" B_PRIx32 "\n", typer, pidr2, group, enabled, pending, active);
	if (!RegistersMatch(typer, pidr2, group, enabled, pending, active)
		|| (distributor.Read(0xd00 + (kVector / 32) * 4) & kMask) != 0)
		return B_NOT_SUPPORTED;
	Registers alias("ROCK5 MBI test alias", kAlias, B_PAGE_SIZE);
	if (alias.InitCheck() != B_OK)
		return alias.InitCheck();
	InterruptState state;
	state.sem = create_sem(0, "ROCK5 MBI probe");
	if (state.sem < B_OK)
		return state.sem;
	uint32 oldConfig = distributor.Read(kConfigOffset);
	// No global MSI interface is published. The GIC already reserves this SPI
	// as IRQ; no peripheral in the verified firmware profile uses it.
	distributor.Write(kConfigOffset, oldConfig | kEdgeMask);
	status_t status = B_ERROR;
	bool installed = false;
	if (distributor.Read(kConfigOffset) == (oldConfig | kEdgeMask)) {
		status = install_io_interrupt_handler(kVector, HandleInterrupt, &state, 0);
		installed = status == B_OK;
	}
	if (installed) {
		status = acquire_sem_etc(state.sem, 1, B_RELATIVE_TIMEOUT, 20000);
		if (status == B_TIMED_OUT && atomic_get(&state.count) == 0) {
			status = B_OK;
			dprintf("ROCK5_MBI_QUIET_PASS vector=%u\n", kVector);
		} else
			status = B_ERROR;
		for (unsigned i = 0; status == B_OK && i < 8; i++) {
			const char* frame = i < 4 ? "distributor" : "alias";
			dprintf("ROCK5_MBI_SEND frame=%s vector=%u sequence=%u\n", frame, kVector, i + 1);
			(i < 4 ? distributor : alias).Write(0x40, kVector);
			status = acquire_sem_etc(state.sem, 1, B_RELATIVE_TIMEOUT, 500000);
			if (status == B_OK && (atomic_get(&state.count) != int32(i + 1)
				|| atomic_get(&state.cpu) != 0))
				status = B_ERROR;
			if (status == B_OK)
				dprintf("ROCK5_MBI_RECEIVED frame=%s count=%" B_PRId32 " cpu=%" B_PRId32 "\n",
					frame, atomic_get(&state.count), atomic_get(&state.cpu));
		}
		if (status == B_OK) {
			status_t quiet = acquire_sem_etc(state.sem, 1, B_RELATIVE_TIMEOUT, 20000);
			if (quiet != B_TIMED_OUT || atomic_get(&state.count) != 8)
				status = B_ERROR;
		}
		// Removal synchronizes with the handler through the vector lock and
		// disables delivery before the stack context and semaphore disappear.
		if (remove_io_interrupt_handler(kVector, HandleInterrupt, &state) != B_OK)
			panic("ROCK5 MBI test could not remove its interrupt handler");
	}
	bool disabled = distributor.WaitClear(0, 1u << 31)
		&& (distributor.Read(enableOffset) & kMask) == 0;
	bool inactive = distributor.WaitClear(activeOffset, kMask);
	// Clear only our pending bit, then restore only its trigger field. No
	// distributor-wide enable, affinity, priority or other interrupt changes.
	distributor.Write(0x280 + (kVector / 32) * 4, kMask);
	uint32 currentConfig = distributor.Read(kConfigOffset);
	distributor.Write(kConfigOffset, (currentConfig & ~kEdgeMask) | (oldConfig & kEdgeMask));
	bool restored = distributor.Read(kConfigOffset) == oldConfig
		&& distributor.WaitClear(pendingOffset, kMask);
	delete_sem(state.sem);
	dprintf("ROCK5_MBI_CLEANUP disabled=%d inactive=%d restored=%d count=%" B_PRId32
		" status=%" B_PRId32 "\n", disabled, inactive, restored, atomic_get(&state.count), status);
	if (!disabled || !inactive || !restored)
		status = B_ERROR;
	if (status == B_OK) {
		cookie->length = snprintf(cookie->report, sizeof(cookie->report),
			"ROCK5_MBI_PASS vector=%u distributor=4 alias=4 count=8 cpu=0 cleanup=pass\n", kVector);
		dprintf("%s", cookie->report);
	}
	return status;
}

static status_t
ProbeOpen(const char*, uint32, void** cookie)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	void* settings = load_driver_settings("rock5_mbi_probe");
	const char* profile = get_driver_parameter(settings, "firmware_profile", nullptr, nullptr);
	bool allowed = FirmwareMatches(gFDT, profile) && !msi_supported();
	unload_driver_settings(settings);
	if (!allowed) {
		dprintf("ROCK5_MBI_REJECTED_BEFORE_MMIO\n");
		return B_NOT_SUPPORTED;
	}
	if (atomic_test_and_set(&sOpen, 1, 0) != 0)
		return B_BUSY;
	ProbeCookie* result = new(std::nothrow) ProbeCookie;
	status_t status = result != nullptr ? RunProbe(result) : B_NO_MEMORY;
	if (status != B_OK) {
		delete result;
		atomic_set(&sOpen, 0);
		dprintf("ROCK5_MBI_FAILED status=%" B_PRId32 "\n", status);
		return status;
	}
	*cookie = result;
	return B_OK;
}

static status_t ProbeClose(void*) { return B_OK; }
static status_t ProbeControl(void*, uint32, void*, size_t) { return B_NOT_SUPPORTED; }
static status_t ProbeWrite(void*, off_t, const void*, size_t* size)
{
	*size = 0;
	return B_NOT_ALLOWED;
}
static status_t ProbeFree(void* cookie)
{
	delete (ProbeCookie*)cookie;
	atomic_set(&sOpen, 0);
	return B_OK;
}
static status_t ProbeRead(void* cookie, off_t position, void* buffer, size_t* size)
{
	ProbeCookie* result = (ProbeCookie*)cookie;
	if (position < 0)
		return B_BAD_VALUE;
	size_t remaining = uint64(position) < result->length ? result->length - position : 0;
	if (*size > remaining)
		*size = remaining;
	return *size == 0 ? B_OK : user_memcpy(buffer, result->report + position, *size);
}

status_t init_hardware() { return B_OK; }
status_t init_driver() { return B_OK; }
void uninit_driver() {}
const char** publish_devices()
{
	static const char* names[] = {kDeviceName, nullptr};
	return names;
}
device_hooks* find_device(const char* name)
{
	static device_hooks hooks = {ProbeOpen, ProbeClose, ProbeFree, ProbeControl, ProbeRead, ProbeWrite};
	return strcmp(name, kDeviceName) == 0 ? &hooks : nullptr;
}
