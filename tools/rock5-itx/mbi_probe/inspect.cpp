/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include <Drivers.h>
#include <KernelExport.h>
#include <driver_settings.h>
#include <arch/arm64/rk3588_mbi.h>
#include <arch/generic/msi.h>
#include <smp.h>
#include <util/kernel_cpp.h>
#include <stdio.h>
#include <string.h>

#include "../pci_config_probe_checks.h"

extern void* gFDT;
int32 api_version = B_CUR_DRIVER_API_VERSION;
static const char kDeviceName[] = "misc/rock5_msi_inspect";
static int32 sOpen = 0;

struct ProbeCookie {
	char report[4096];
	size_t length = 0;
	void Append(const char* name, uint32 value)
	{
		int count = snprintf(report + length, sizeof(report) - length,
			"ROCK5_MSI_INSPECT %s=%#" B_PRIx32 "\n", name, value);
		if (count < 0 || size_t(count) >= sizeof(report) - length)
			panic("ROCK5 MSI inspection report overflow");
		length += count;
		dprintf("ROCK5_MSI_INSPECT %s=%#" B_PRIx32 "\n", name, value);
	}
};

class ReadFrame {
public:
	ReadFrame(const char* name, phys_addr_t address, size_t size)
	{
		fArea = map_physical_memory(name, address, size,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA, (void**)&fBase);
	}
	~ReadFrame() { if (fArea >= B_OK) delete_area(fArea); }
	status_t InitCheck() const { return fArea < B_OK ? fArea : B_OK; }
	uint32 Read(unsigned offset) const
	{
		uint32 value = *(volatile const uint32*)(fBase + offset);
		memory_full_barrier();
		return value;
	}
private:
	area_id fArea;
	volatile const uint8* fBase = nullptr;
};

static status_t
Inspect(ProbeCookie* result)
{
	// These configuration addresses and root checks match the separately
	// qualified EDK2 v1.1 Samsung profile. Never access a disconnected endpoint.
	ReadFrame root("ROCK5 MSI root inspection", UINT64_C(0xa40000000), B_PAGE_SIZE);
	if (root.InitCheck() != B_OK)
		return root.InitCheck();
	uint32 words[64];
	for (unsigned i = 0; i < 64; i++)
		words[i] = root.Read(i * 4);
	if (!Rock5RootLinkReady(words))
		return B_NOT_SUPPORTED;
	ReadFrame config("ROCK5 MSI endpoint inspection", UINT64_C(0x900100000), B_PAGE_SIZE);
	if (config.InitCheck() != B_OK)
		return config.InitCheck();
	// Require the exact BAR and MSI-X layout captured on this Samsung 950 Pro.
	for (unsigned i = 0; i < 64; i++)
		words[i] = config.Read(i * 4);
	if (!Rock5SamsungMsixLayoutMatches(words))
		return B_NOT_SUPPORTED;
	result->Append("pci_command_status", config.Read(4));
	result->Append("pci_msix_capability", config.Read(0xb0));

	ReadFrame nvme("ROCK5 MSI table inspection", 0xf0000000, 0x4000);
	if (nvme.InitCheck() != B_OK)
		return nvme.InitCheck();
	const struct { unsigned offset; const char* name; } nvmeFields[] = {
		{0x14, "nvme_cc"}, {0x1c, "nvme_csts"},
		{0x2000, "nvme_pba_lo"}, {0x2004, "nvme_pba_hi"},
		{0x3000, "nvme_table0_address_lo"}, {0x3004, "nvme_table0_address_hi"},
		{0x3008, "nvme_table0_data"}, {0x300c, "nvme_table0_control"}
	};
	for (const auto& field : nvmeFields)
		result->Append(field.name, nvme.Read(field.offset));

	ReadFrame gic("ROCK5 MSI GIC inspection", Gicv3Mbi::kDistributor, 0x10000);
	if (gic.InitCheck() != B_OK)
		return gic.InitCheck();
	const struct { unsigned offset; const char* name; } gicFields[] = {
		{0, "gic_control"}, {4, "gic_typer"}, {0xffe8, "gic_pidr2"},
		{0xb8, "gic_group_448_479"}, {0x138, "gic_enabled_448_479"},
		{0x238, "gic_pending_448_479"}, {0x338, "gic_active_448_479"},
		{0x5d0, "gic_priority_464_467"}, {0xc74, "gic_config_464_479"},
		{0x6e80, "gic_route_464_lo"}, {0x6e84, "gic_route_464_hi"}
	};
	for (const auto& field : gicFields)
		result->Append(field.name, gic.Read(field.offset));

	// RK3588 TRM Part 1, PHP_GRF register descriptions, pages 829..833.
	// Read the ITS bypass-match addresses and PCIe TBU controls without writes.
	ReadFrame php("ROCK5 MSI PHP inspection", 0xfd5b0000, B_PAGE_SIZE);
	if (php.InitCheck() != B_OK)
		return php.InitCheck();
	result->Append("php_its_taddr0", php.Read(0x28));
	result->Append("php_its_taddr1", php.Read(0x2c));
	result->Append("php_pcie_mmu_mode", php.Read(0x30));
	result->Append("php_pcie_mmu_control0", php.Read(0x34));
	result->Append("complete", 1);
	return B_OK;
}

static status_t
ProbeOpen(const char*, uint32, void** cookie)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	void* settings = load_driver_settings("rock5_msi_inspect");
	const char* profile = get_driver_parameter(settings, "firmware_profile", "", "");
	bool allowed = strcmp(profile, "rock5-itx-edk2-v1.1-dt-msi-inspect") == 0
		&& Gicv3Mbi::FirmwareMatches(gFDT) && msi_supported();
	unload_driver_settings(settings);
	if (!allowed) {
		dprintf("ROCK5_MSI_INSPECT_REJECTED_BEFORE_MMIO\n");
		return B_NOT_SUPPORTED;
	}
	if (atomic_test_and_set(&sOpen, 1, 0) != 0)
		return B_BUSY;
	ProbeCookie* result = new(std::nothrow) ProbeCookie;
	status_t status = result != nullptr ? Inspect(result) : B_NO_MEMORY;
	if (status != B_OK) {
		delete result;
		atomic_set(&sOpen, 0);
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
