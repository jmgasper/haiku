/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Host fixture for the RK3588 display driver. The production device-tree
// traversal, ioctls, read-only observation, EDID transfer and scanout swap run
// against a modeled device manager and guarded register pages; nothing here
// touches hardware.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

using int32 = int32_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using status_t = int32_t;
static const status_t B_OK = 0, B_BAD_VALUE = -1, B_BAD_ADDRESS = -2,
	B_DEV_INVALID_IOCTL = -3, B_NO_MEMORY = -4, B_NOT_SUPPORTED = -5,
	B_NOT_ALLOWED = -6, B_ENTRY_NOT_FOUND = -8, B_ERROR = -9, B_BUSY = -10;
using area_id = int32_t;
using team_id = int32_t;
using addr_t = uintptr_t;
static const unsigned B_PAGE_SIZE = 4096, B_ANY_KERNEL_ADDRESS = 4,
	B_UNCACHED_MEMORY = 1u << 28, B_KERNEL_READ_AREA = 1u << 4,
	B_KERNEL_WRITE_AREA = 1u << 5;

#include "DisplayEdid.h"
#include "DisplayScanout.h"

using namespace RK3588Display;

static std::map<int, std::pair<void*, size_t> > sAreas;
static unsigned sMapAttempts, sFailMap;
static std::vector<uint64> sMappedBases;
static int64_t sTime;
static unsigned sLockDepth;
static uint32 sRepairStatus = (1u << 16) | (1u << 18);
static uint32 sGate52, sGate61;
static bool sAllowEdid;
static uint32 sHotPlug = (1u << 24) | (1u << 27);

// VOP2 model: firmware-like words for the registers the scanout swap reads,
// pseudo-random elsewhere. A writable mapping logs every changed word in
// order; only the located window's address and the commit word may change.
static std::map<unsigned, uint32> sVopOverrides;
static uint32* sVopModel;
static std::vector<uint32> sVopShadow;
static std::vector<std::pair<unsigned, uint32> > sVopWrites;
static bool sAllowScanout, sStickyAddress, sCommitNeverCompletes;
// Shadowed window registers: a written address waits in the shadow set until
// the port's frame start, modeled as a countdown of barrier/poll steps after
// the commit word; reads keep returning the active value until then.
static uint32 sVopPendingAddress;
static bool sVopAddressPending;
static int sVopCommitCountdown = -1;
static const unsigned kModelWindow = 2;
static const unsigned kModelAddressOffset = 0x1800 + kModelWindow * 0x200 + 0x14;
static const uint32 kModelFirmwareAddress = 0xed280000;
static const uint64 kModelPatternPhysical = 0x40100000;

struct mutex {};
#define MUTEX_INITIALIZER(name) {}
class MutexLocker {
public:
	explicit MutexLocker(mutex&) { assert(sLockDepth++ == 0); }
	~MutexLocker() { assert(--sLockDepth == 0); }
};

static int64_t system_time() { return ++sTime; }
static void memory_read_barrier() {}
static void kernel_dprintf(const char*, ...) {}
#define dprintf kernel_dprintf
#define B_PRIu32 "u"
#define B_PRIx32 "x"
#define B_PRIx64 "llx"

// I2C master model for the HDMI TX1 window: reacts synchronously to writes
// (the production write helper issues a barrier after each store) and to polls.
static uint32* sHdmiModel;
static uint8_t sEdid[512];
static int sNackAt = -1;
static bool sUnresponsive;
static unsigned sServed, sResets, sModelSpins;
static bool sServing;

static uint32 ModelRegister(uint64 base, unsigned offset);

static void
VopModelStep()
{
	if (sVopModel == NULL)
		return;
	for (unsigned i = 0; i < sVopShadow.size(); i++) {
		if (sVopModel[i] == sVopShadow[i])
			continue;
		unsigned offset = i * 4;
		uint32 value = sVopModel[i];
		sVopWrites.push_back(std::make_pair(offset, value));
		if (offset == 0x000) {
			// Commit: the port bit stays visible until its next frame start.
			assert((value & 0x8000) != 0 && (value >> 16) == (value & 0xf));
			sVopModel[i] = 0x8000 | (value & 0xf);
			sVopCommitCountdown = sCommitNeverCompletes ? -1 : 3;
		} else if (offset == kModelAddressOffset) {
			sVopPendingAddress = value;
			sVopAddressPending = true;
			sVopModel[i] = sVopShadow[i]; // the active address stays readable
		} else
			assert(!"unexpected VOP2 register write");
		sVopShadow[i] = sVopModel[i];
	}
	if (sVopCommitCountdown > 0 && --sVopCommitCountdown == 0) {
		// Frame start: the shadow set becomes active and the port bit clears.
		sVopModel[0] = ModelRegister(0xfdd90000, 0);
		sVopShadow[0] = sVopModel[0];
		if (sVopAddressPending && !sStickyAddress) {
			sVopOverrides[kModelAddressOffset] = sVopPendingAddress;
			sVopModel[kModelAddressOffset / 4] = sVopPendingAddress;
			sVopShadow[kModelAddressOffset / 4] = sVopPendingAddress;
		}
		sVopAddressPending = false;
	}
}


static void
ModelStep()
{
	VopModelStep();
	if (sHdmiModel == NULL)
		return;
	uint32* regs = sHdmiModel;
	if (regs[0x3028 / 4] != 0) {
		regs[0x3020 / 4] &= ~regs[0x3028 / 4];
		regs[0x3028 / 4] = 0;
	}
	if ((regs[0xec / 4] & 1) != 0) {
		regs[0xec / 4] = 0;
		regs[0xf4 / 4] &= ~0x1eu;
		sServing = false;
		sResets++;
	}
	uint32 control = regs[0xf4 / 4];
	if ((control & 0x1e) == 0) {
		sServing = false;
		return;
	}
	if (sServing || sUnresponsive)
		return;
	sServing = true;
	assert((control & 0x1e) == 0x04 || (control & 0x1e) == 0x10);
	assert(((control >> 5) & 0x7f) == 0x50);
	unsigned address = (control >> 12) & 0xff;
	unsigned segment = 0;
	if ((control & 0x10) != 0) {
		assert((regs[0xf8 / 4] & 0x7f) == 0x30);
		segment = (regs[0xf8 / 4] >> 7) & 0x7f;
	}
	assert((regs[0x3024 / 4] & 0x5) == 0x5); // done/error status unmasked during transfers
	if (sNackAt >= 0 && (int)sServed == sNackAt) {
		regs[0x3020 / 4] |= 0x4;
	} else {
		regs[0x10c / 4] = 0xa5000000u | sEdid[segment * 256 + address];
		regs[0x3020 / 4] |= 0x1;
	}
	sServed++;
}

static void memory_write_barrier() { ModelStep(); }
static void spin(unsigned micros) { assert(micros == 20); sModelSpins++; ModelStep(); }


static uint32
ModelRegister(uint64 base, unsigned offset)
{
	if (base == 0xfd8d8000 && offset == 0x290)
		return sRepairStatus;
	if (base == 0xfd7c0000 && offset == 0x8d0)
		return sGate52;
	if (base == 0xfd7c0000 && offset == 0x8f4)
		return sGate61;
	if (base == 0xfd58c000 && offset == 0x384)
		return sHotPlug;
	if (base == 0xfdd90000) {
		auto found = sVopOverrides.find(offset);
		if (found != sVopOverrides.end())
			return found->second;
	}
	return (uint32)(base >> 4) ^ (offset * 0x01010101u);
}


static int
map_physical_memory(const char*, uint64 base, size_t bytes, uint32 spec,
	uint32 protection, void** address)
{
	assert(sLockDepth == 1);
	static const uint64 kControl[] = {0xfd8d8000, 0xfd7c0000, 0xfd58c000,
		0xfd5a4000, 0xfd5a8000, 0xfd5e4000};
	bool control = false;
	for (uint64 candidate : kControl)
		control |= candidate == base;
	bool writable = false;
	if (control)
		assert(bytes == B_PAGE_SIZE);
	else if (base == 0xfdd90000) {
		assert(bytes == kVopMapSize && (sRepairStatus & (1u << 16)) != 0 && (sGate52 & 0x300) == 0);
		// Only the opt-in scanout swap or restore maps VOP2 writable; queries do not.
		writable = (protection & B_KERNEL_WRITE_AREA) != 0;
		assert(!writable || sAllowScanout);
	} else if (base == 0xfdea0000) {
		assert((sRepairStatus & (1u << 18)) != 0 && (sGate61 & 4) == 0);
		// Only the opt-in EDID path maps HDMI TX1 writable, after its own gating.
		writable = (protection & B_KERNEL_WRITE_AREA) != 0;
		assert(bytes == (writable ? kHdmiEdidMapSize : kHdmiMapSize));
		assert(!writable || sAllowEdid);
	} else
		assert(false);
	assert(spec == (B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY));
	// Never a writable mapping of any control block.
	assert(protection == (B_KERNEL_READ_AREA | (writable ? B_KERNEL_WRITE_AREA : 0)));
	sMappedBases.push_back(base);
	if (++sMapAttempts == sFailMap)
		return B_NO_MEMORY;
	void* allocation = mmap(NULL, bytes + 2 * B_PAGE_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(allocation != MAP_FAILED);
	uint32* registers = (uint32*)((char*)allocation + B_PAGE_SIZE);
	assert(mprotect(registers, bytes, PROT_READ | PROT_WRITE) == 0);
	for (unsigned offset = 0; offset < bytes; offset += 4)
		registers[offset / 4] = ModelRegister(base, offset);
	if (writable && base == 0xfdd90000) {
		assert(sVopModel == NULL);
		sVopModel = registers;
		sVopShadow.assign(registers, registers + bytes / 4);
		// The driver always writes the address before committing, so a
		// commit without an observed write applies the active value again.
		sVopAddressPending = false;
		sVopCommitCountdown = -1;
	} else if (writable) {
		registers[0xf4 / 4] = 0x00000a00; // idle master, slave 0x50 set by firmware
		registers[0x3020 / 4] = 0;
		registers[0x3024 / 4] = 0;
		registers[0x3028 / 4] = 0;
		registers[0xec / 4] = 0;
		sHdmiModel = registers;
		sServing = false;
	} else {
		// Any production write faults immediately, as would either guard page.
		assert(mprotect(registers, bytes, PROT_READ) == 0);
	}
	int area = 17 + sMapAttempts;
	sAreas[area] = {allocation, bytes + 2 * B_PAGE_SIZE};
	*address = registers;
	return area;
}


class AreaDeleter {
public:
	explicit AreaDeleter(int area = -1) : fArea(area) {}
	~AreaDeleter() { SetTo(-1); }
	void SetTo(int area)
	{
		if (fArea >= 0) {
			assert(sAreas.count(fArea) == 1);
			if (sHdmiModel != NULL && (char*)sHdmiModel == (char*)sAreas.at(fArea).first + B_PAGE_SIZE)
				sHdmiModel = NULL;
			if (sVopModel != NULL && (char*)sVopModel == (char*)sAreas.at(fArea).first + B_PAGE_SIZE) {
				VopModelStep();
				sVopModel = NULL;
			}
			assert(munmap(sAreas.at(fArea).first, sAreas.at(fArea).second) == 0);
			sAreas.erase(fArea);
		}
		fArea = area;
	}
	int Get() const { return fArea; }
private:
	int fArea;
};


struct module_info { const char* name; };
struct driver_module_info { module_info info; };
struct device_node;
struct fdt_device { device_node* node; };
struct fdt_bus {};
struct fdt_device_module_info {
	driver_module_info info;
	device_node* (*get_bus)(fdt_device*);
	const char* (*get_name)(fdt_device*);
	const void* (*get_prop)(fdt_device*, const char*, int*);
	bool (*get_reg)(fdt_device*, uint32, uint64*, uint64*);
	bool (*get_interrupt)(fdt_device*, uint32, device_node**, uint64*);
};
struct fdt_bus_module_info {
	driver_module_info info;
	device_node* (*node_by_phandle)(fdt_bus*, int);
};
static const int B_STRING_TYPE = 1;
struct device_attr {
	const char* name;
	int type;
	union { const char* string; } value;
};
struct device_manager_info {
	status_t (*get_driver)(device_node*, driver_module_info**, void**);
	device_node* (*get_parent_node)(device_node*);
	void (*put_node)(device_node*);
	status_t (*find_child_node)(device_node*, const device_attr*, device_node**);
};
struct device_node {
	std::string name;
	device_node* parent = NULL;
	std::map<std::string, std::vector<uint8_t> > properties;
	fdt_device device{this};
	uint64 base = 0, size = 0, base1 = 0, size1 = 0;
	std::vector<uint64> irqs;
	device_node* irqController = NULL;
	bool wrongModule = false;
	int held = 0;
};

static device_node sBusNode, sRoot, sVop, sPorts, sPort1, sEndpoint8, sHdmi,
	sHdmiPorts, sHdmiPort0, sHdmiEndpoint, sPhy0, sPhy1, sHdptxGrf, sSysGrf,
	sVopGrf, sVo1Grf, sPmu, sPower, sClock, sGic, sOther;
static fdt_bus sBus;
static std::map<int, device_node*> sPhandles;
static std::vector<device_node*> sAllNodes = {&sBusNode, &sRoot, &sVop, &sPorts,
	&sPort1, &sEndpoint8, &sHdmi, &sHdmiPorts, &sHdmiPort0, &sHdmiEndpoint, &sPhy0,
	&sPhy1, &sHdptxGrf, &sSysGrf, &sVopGrf, &sVo1Grf, &sPmu, &sPower, &sClock, &sGic,
	&sOther};


static const void*
GetProperty(fdt_device* dev, const char* property, int* length)
{
	auto it = dev->node->properties.find(property);
	if (it == dev->node->properties.end()) {
		if (length != NULL) *length = -1;
		return NULL;
	}
	if (length != NULL) *length = it->second.size();
	return it->second.empty() ? (const void*)"" : it->second.data();
}


static bool
GetReg(fdt_device* dev, uint32 index, uint64* base, uint64* size)
{
	assert(index <= 1);
	*base = index == 0 ? dev->node->base : dev->node->base1;
	*size = index == 0 ? dev->node->size : dev->node->size1;
	return *size != 0;
}


static bool
GetInterrupt(fdt_device* dev, uint32 index, device_node** node, uint64* irq)
{
	assert(dev->node == &sVop || dev->node == &sHdmi);
	assert(index < dev->node->irqs.size());
	*node = dev->node->irqController;
	*irq = dev->node->irqs[index];
	return true;
}


static fdt_device_module_info sFdt = {
	{{"bus_managers/fdt/driver_v1"}},
	[](fdt_device*) { return &sBusNode; },
	[](fdt_device* dev) { return dev->node->name.c_str(); },
	GetProperty, GetReg, GetInterrupt
};
static fdt_bus_module_info sFdtBus = {
	{{"bus_managers/fdt/root/driver_v1"}},
	[](fdt_bus*, int phandle) -> device_node* {
		auto it = sPhandles.find(phandle);
		return it == sPhandles.end() ? NULL : it->second;
	}
};
static driver_module_info sWrongModule = {{"unrelated/driver_v1"}};

static device_manager_info sManager = {
	[](device_node* node, driver_module_info** module, void** cookie) -> status_t {
		if (node->wrongModule) {
			*module = &sWrongModule;
			*cookie = NULL;
		} else if (node == &sBusNode) {
			*module = &sFdtBus.info;
			*cookie = &sBus;
		} else {
			*module = &sFdt.info;
			*cookie = &node->device;
		}
		return B_OK;
	},
	[](device_node* node) -> device_node* {
		if (node->parent != NULL) node->parent->held++;
		return node->parent;
	},
	[](device_node* node) { assert(node->held > 0); node->held--; },
	[](device_node* parent, const device_attr* attributes, device_node** output) -> status_t {
		assert(*output == NULL && strcmp(attributes[0].name, "fdt/name") == 0);
		assert(attributes[0].type == B_STRING_TYPE && attributes[1].name == NULL);
		if (parent == NULL)
			return B_ENTRY_NOT_FOUND;
		// The real lookup searches all descendants; the caller must verify
		// the immediate parent of the returned node.
		for (device_node* node : sAllNodes) {
			if (node->name != attributes[0].value.string) continue;
			for (device_node* ancestor = node->parent; ancestor != NULL; ancestor = ancestor->parent) {
				if (ancestor != parent) continue;
				node->held++; *output = node; return B_OK;
			}
		}
		return B_ENTRY_NOT_FOUND;
	}
};


static status_t
user_memcpy(void* output, const void* input, size_t bytes)
{
	if (output == NULL) return B_BAD_ADDRESS;
	memcpy(output, input, bytes);
	return B_OK;
}


// Pattern buffer services: contiguous allocation below 4 GiB, its physical
// address, the cache eviction/retyping step and the framebuffer boot item.
struct virtual_address_restrictions { void* address; uint32 address_specification; size_t alignment; };
struct physical_address_restrictions { uint64 low_address, high_address, alignment, boundary; };
struct physical_entry { uint64 address; uint64 size; };
static const uint32 B_CONTIGUOUS = 3;
static const team_id B_SYSTEM_TEAM = 1;
static void* sPatternAllocation;
static int sPatternArea = -1;
static unsigned sPatternAllocations, sFailPattern, sNoncacheableCalls;
static bool sPatternHighPhysical;

static area_id
create_area_etc(team_id team, const char* name, size_t size, uint32 lock, uint32 protection,
	uint32 flags, size_t guardSize, const virtual_address_restrictions* virtualRestrictions,
	const physical_address_restrictions* physicalRestrictions, void** address)
{
	assert(sLockDepth == 1 && team == B_SYSTEM_TEAM && strcmp(name, "RK3588 display pattern") == 0);
	assert(size == kPatternBytes && lock == B_CONTIGUOUS && flags == 0 && guardSize == 0);
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA));
	assert(virtualRestrictions->address == NULL && virtualRestrictions->address_specification == 0);
	assert(physicalRestrictions->low_address == 0 && physicalRestrictions->high_address == 0x100000000ull);
	assert(physicalRestrictions->alignment == B_PAGE_SIZE && physicalRestrictions->boundary == 0);
	assert(sPatternArea < 0);
	if (++sPatternAllocations == sFailPattern)
		return B_NO_MEMORY;
	sPatternAllocation = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(sPatternAllocation != MAP_FAILED);
	sPatternArea = 900 + (int)sPatternAllocations;
	*address = sPatternAllocation;
	return sPatternArea;
}


static status_t
get_memory_map(const void* address, size_t bytes, physical_entry* table, int32 count)
{
	assert(address == sPatternAllocation && bytes == kPatternBytes && count == 1);
	table->address = sPatternHighPhysical ? 0xffc00000ull : kModelPatternPhysical;
	table->size = bytes;
	return B_OK;
}


static status_t
delete_area(area_id area)
{
	assert(area == sPatternArea && sPatternAllocation != NULL);
	assert(munmap(sPatternAllocation, kPatternBytes) == 0);
	sPatternAllocation = NULL;
	sPatternArea = -1;
	return B_OK;
}


static status_t
MakePatternNoncacheable(area_id area, void* address, size_t bytes)
{
	assert(area == sPatternArea && address == sPatternAllocation && bytes == kPatternBytes);
	// The whole pattern is filled through the cached alias before retyping.
	const uint32* pixels = (const uint32*)address;
	assert(pixels[0] == kPatternBorderColor && pixels[1079 * 1920 + 1919] == kPatternBorderColor);
	assert(pixels[540 * 1920 + 31] == kPatternBorderColor && pixels[31 * 1920 + 960] == kPatternBorderColor);
	assert(pixels[540 * 1920 + 32] == 0xffffffff && pixels[540 * 1920 + 1887] == 0xff000000);
	for (unsigned bar = 0; bar < kPatternBars; bar++) {
		for (unsigned y = 32; y < 1048; y += 127)
			assert(pixels[y * 1920 + 32 + bar * 232 + 116] == kPatternColors[bar]);
	}
	sNoncacheableCalls++;
	return B_OK;
}


#define FRAME_BUFFER_BOOT_INFO "frame_buffer/v1"
struct frame_buffer_boot_info {
	area_id area;
	addr_t physical_frame_buffer;
	addr_t frame_buffer;
	int32 width, height, depth, bytes_per_row;
	uint8_t vesa_capabilities;
};
static frame_buffer_boot_info sBootInfo;
static bool sBootInfoPresent;

static void*
get_boot_item(const char* name, size_t* size)
{
	assert(strcmp(name, FRAME_BUFFER_BOOT_INFO) == 0 && size == NULL);
	return sBootInfoPresent ? &sBootInfo : NULL;
}


#include "driver.inc"


static void
Cells(device_node& node, const char* property, std::initializer_list<uint32> cells)
{
	auto& bytes = node.properties[property];
	bytes.clear();
	for (uint32 value : cells) {
		for (int shift = 24; shift >= 0; shift -= 8)
			bytes.push_back(value >> shift);
	}
}


static void
Strings(device_node& node, const char* property, std::initializer_list<const char*> strings)
{
	auto& bytes = node.properties[property];
	bytes.clear();
	for (const char* value : strings)
		bytes.insert(bytes.end(), value, value + strlen(value) + 1);
}


static void
Prepare()
{
	for (device_node* node : sAllNodes) {
		assert(node->held == 0);
		node->properties.clear();
		node->wrongModule = false;
		node->parent = &sRoot;
		node->base1 = node->size1 = 0;
		node->irqs.clear();
		node->irqController = &sGic;
	}
	sRoot.parent = &sBusNode;
	sBusNode.parent = NULL;
	sRoot.name = "";
	sVop.name = "vop@fdd90000";
	sPorts.name = "ports"; sPorts.parent = &sVop;
	sPort1.name = "port@1"; sPort1.parent = &sPorts;
	sEndpoint8.name = "endpoint@8"; sEndpoint8.parent = &sPort1;
	sHdmi.name = "hdmi@fdea0000";
	sHdmiPorts.name = "ports"; sHdmiPorts.parent = &sHdmi;
	sHdmiPort0.name = "port@0"; sHdmiPort0.parent = &sHdmiPorts;
	sHdmiEndpoint.name = "endpoint"; sHdmiEndpoint.parent = &sHdmiPort0;
	sPhy0.name = "phy@fed60000";
	sPhy1.name = "phy@fed70000";
	sHdptxGrf.name = "syscon@fd5e4000";
	sSysGrf.name = "syscon@fd58c000";
	sVopGrf.name = "syscon@fd5a4000";
	sVo1Grf.name = "syscon@fd5a8000";
	sPmu.name = "power-management@fd8d8000";
	sPower.name = "power-controller"; sPower.parent = &sPmu;
	sClock.name = "clock-controller@fd7c0000";
	sGic.name = "interrupt-controller@fe600000";
	sOther.name = "other";
	sPhandles = {{0x21, &sClock}, {0x22, &sPower}, {0x6b, &sPhy0}, {0x6c, &sPhy1},
		{0x6e, &sSysGrf}, {0x6f, &sVopGrf}, {0x70, &sVo1Grf}, {0x71, &sPmu},
		{0x72, &sHdmiEndpoint}, {0x117, &sEndpoint8}, {0x127, &sHdptxGrf}};
	sVop.base = 0xfdd90000; sVop.size = 0x4200; sVop.base1 = 0xfdd95000; sVop.size1 = 0x1000;
	sVop.irqs = {188};
	sHdmi.base = 0xfdea0000; sHdmi.size = 0x20000;
	sHdmi.irqs = {205, 206, 207, 208, 393};
	sPhy0.base = 0xfed60000; sPhy0.size = 0x2000;
	sPhy1.base = 0xfed70000; sPhy1.size = 0x2000;
	sHdptxGrf.base = 0xfd5e4000; sHdptxGrf.size = 0x100;
	sSysGrf.base = 0xfd58c000; sSysGrf.size = 0x1000;
	sVopGrf.base = 0xfd5a4000; sVopGrf.size = 0x2000;
	sVo1Grf.base = 0xfd5a8000; sVo1Grf.size = 0x4000;
	sPmu.base = 0xfd8d8000; sPmu.size = 0x400;
	sClock.base = 0xfd7c0000; sClock.size = 0x5c000;
	sGic.base = 0xfe600000; sGic.size = 0x10000;
	Strings(sRoot, "compatible", {"radxa,rock-5-itx", "rockchip,rk3588"});
	Strings(sVop, "compatible", {"rockchip,rk3588-vop"});
	Strings(sVop, "status", {"okay"});
	Strings(sVop, "reg-names", {"vop", "gamma-lut"});
	Strings(sVop, "clock-names", {"aclk", "hclk", "dclk_vp0", "dclk_vp1", "dclk_vp2",
		"dclk_vp3", "pclk_vop", "pll_hdmiphy0", "pll_hdmiphy1"});
	Cells(sVop, "clocks", {0x21, 0x25d, 0x21, 0x25c, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
		0x21, 0x264, 0x21, 0x25b, 0x6b, 0x6c});
	Cells(sVop, "interrupts", {0, 156, 4, 0});
	Cells(sVop, "power-domains", {0x22, 24});
	Cells(sVop, "rockchip,grf", {0x6e});
	Cells(sVop, "rockchip,vop-grf", {0x6f});
	Cells(sVop, "rockchip,vo1-grf", {0x70});
	Cells(sVop, "rockchip,pmu", {0x71});
	Cells(sPort1, "reg", {1});
	Cells(sEndpoint8, "reg", {8});
	Cells(sEndpoint8, "remote-endpoint", {0x72});
	Cells(sHdmiPort0, "reg", {0});
	Cells(sHdmiEndpoint, "remote-endpoint", {0x117});
	Strings(sHdmi, "compatible", {"rockchip,rk3588-dw-hdmi-qp"});
	Strings(sHdmi, "status", {"okay"});
	Strings(sHdmi, "clock-names", {"pclk", "earc", "ref", "aud", "hdp", "hclk_vo1"});
	Cells(sHdmi, "clocks", {0x21, 0x213, 0x21, 0x214, 0x21, 0x215, 0x21, 0x239, 0x21, 0x253,
		0x21, 0x2cd});
	Strings(sHdmi, "interrupt-names", {"avp", "cec", "earc", "main", "hpd"});
	Cells(sHdmi, "interrupts", {0, 173, 4, 0, 0, 174, 4, 0, 0, 175, 4, 0, 0, 176, 4, 0,
		0, 361, 4, 0});
	Strings(sHdmi, "reset-names", {"ref", "hdp"});
	Cells(sHdmi, "resets", {0x21, 0x1d0, 0x21, 0x231});
	Cells(sHdmi, "power-domains", {0x22, 26});
	Cells(sHdmi, "rockchip,grf", {0x6e});
	Cells(sHdmi, "rockchip,vo-grf", {0x70});
	Cells(sHdmi, "phys", {0x6c});
	Strings(sPhy0, "compatible", {"rockchip,rk3588-hdptx-phy"});
	Strings(sPhy1, "compatible", {"rockchip,rk3588-hdptx-phy"});
	Strings(sPhy1, "status", {"okay"});
	Strings(sPhy1, "clock-names", {"ref", "apb"});
	Cells(sPhy1, "rockchip,grf", {0x127});
	Strings(sHdptxGrf, "compatible", {"rockchip,rk3588-hdptxphy-grf", "syscon"});
	Strings(sSysGrf, "compatible", {"rockchip,rk3588-sys-grf", "syscon"});
	Strings(sVopGrf, "compatible", {"rockchip,rk3588-vop-grf", "syscon"});
	Strings(sVo1Grf, "compatible", {"rockchip,rk3588-vo1-grf", "syscon"});
	Strings(sPmu, "compatible", {"rockchip,rk3588-pmu", "syscon", "simple-mfd"});
	Strings(sPower, "compatible", {"rockchip,rk3588-power-controller"});
	Cells(sPower, "#power-domain-cells", {1});
	Strings(sClock, "compatible", {"rockchip,rk3588-cru"});
	Cells(sClock, "#clock-cells", {1});
	Strings(sGic, "compatible", {"arm,gic-v3"});
	Cells(sGic, "#interrupt-cells", {4});
	sRepairStatus = (1u << 16) | (1u << 18);
	sGate52 = 0;
	sGate61 = 0;
	sMapAttempts = 0;
	sFailMap = 0;
	sMappedBases.clear();
	sAllowEdid = false;
	sHotPlug = (1u << 24) | (1u << 27);
	sNackAt = -1;
	sUnresponsive = false;
	sServed = sResets = sModelSpins = 0;
	sHdmiModel = NULL;
	sVopModel = NULL;
	sVopShadow.clear();
	sVopWrites.clear();
	sAllowScanout = false;
	sStickyAddress = false;
	sCommitNeverCompletes = false;
	sVopAddressPending = false;
	sVopCommitCountdown = -1;
	sVopOverrides.clear();
	// Firmware state from the qualified +263 observation: HDMI1 fed by video
	// port 2, ports 0/1/3 in standby, ESMART2 region 0 alone scanning the
	// 1920x1080 XRGB8888 framebuffer at 0xed280000.
	sVopOverrides[0x000] = 0x8000; // GLB_CFG_DONE_EN stays set, port bits clear
	sVopOverrides[0x028] = 0x00080020;
	sVopOverrides[0xc00] = 0x8000000f;
	sVopOverrides[0xd00] = 0x8000000f;
	sVopOverrides[0xe00] = 0x0000000f;
	sVopOverrides[0xf00] = 0x8000000f;
	for (unsigned window = 0; window < kVopEsmartCount; window++) {
		for (unsigned offset : kVopEsmartOffsets)
			sVopOverrides[kVopEsmartBase + window * kVopEsmartStride + offset] = 0;
	}
	sVopOverrides[0x1c00] = 4;
	sVopOverrides[0x1c10] = 1;
	sVopOverrides[kModelAddressOffset] = kModelFirmwareAddress;
	sVopOverrides[0x1c1c] = 1920;
	sVopOverrides[0x1c20] = 0x0437077f;
	sVopOverrides[0x1c24] = 0x0437077f;
	sVopOverrides[0x1c28] = 0;
	sBootInfoPresent = true;
	sBootInfo = frame_buffer_boot_info{17, kModelFirmwareAddress, 0, 1920, 1080, 32, 7680, 0};
	sScanoutSwapped = false;
	sFirmwareAddress = 0;
	ReleasePattern();
	sPatternAllocations = sFailPattern = sNoncacheableCalls = 0;
	sPatternHighPhysical = false;
	for (unsigned i = 0; i < 512; i++)
		sEdid[i] = (uint8_t)(i * 7 + 3);
	static const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	memcpy(sEdid, header, 8);
	sEdid[126] = 3;
	for (unsigned block = 0; block < 4; block++) {
		unsigned sum = 0;
		for (unsigned i = 0; i < 127; i++)
			sum += sEdid[block * 128 + i];
		sEdid[block * 128 + 127] = (uint8_t)(0x100 - (sum & 0xff));
	}
}


static void
CheckSnapshotValues(const DisplaySnapshot& snapshot, bool vop, bool hdmi)
{
	assert(snapshot.version == kSnapshotVersion);
	assert((snapshot.flags & kSnapshotReadOnly) != 0);
	assert(((snapshot.flags & kSnapshotVopRead) != 0) == vop);
	assert(((snapshot.flags & kSnapshotVopSkipped) != 0) == !vop);
	assert(((snapshot.flags & kSnapshotHdmiRead) != 0) == hdmi);
	assert(((snapshot.flags & kSnapshotHdmiSkipped) != 0) == !hdmi);
	assert(snapshot.finishedMicros > snapshot.startedMicros);
	for (unsigned i = 0; i < kPmuCount; i++)
		assert(snapshot.pmu[i] == ModelRegister(0xfd8d8000, kPmuOffsets[i]));
	for (unsigned i = 0; i < kClockSelectCount; i++)
		assert(snapshot.clockSelect[i] == ModelRegister(0xfd7c0000, kClockSelectOffsets[i]));
	for (unsigned i = 0; i < kClockGateCount; i++)
		assert(snapshot.clockGate[i] == ModelRegister(0xfd7c0000, kClockGateOffsets[i]));
	for (unsigned i = 0; i < kSysGrfCount; i++)
		assert(snapshot.sysGrf[i] == ModelRegister(0xfd58c000, kSysGrfOffsets[i]));
	assert(snapshot.vopGrf == ModelRegister(0xfd5a4000, kVopGrfOffset));
	for (unsigned i = 0; i < kVo1GrfCount; i++)
		assert(snapshot.vo1Grf[i] == ModelRegister(0xfd5a8000, kVo1GrfOffsets[i]));
	for (unsigned i = 0; i < kHdptxGrfCount; i++)
		assert(snapshot.hdptxGrf[i] == ModelRegister(0xfd5e4000, kHdptxGrfOffsets[i]));
	for (unsigned i = 0; i < kVopSystemCount; i++)
		assert(snapshot.vopSystem[i] == (vop ? ModelRegister(0xfdd90000, kVopSystemOffsets[i]) : 0));
	for (unsigned i = 0; i < kVopOverlayCount; i++)
		assert(snapshot.vopOverlay[i] == (vop ? ModelRegister(0xfdd90000, kVopOverlayOffsets[i]) : 0));
	for (unsigned port = 0; port < kVopPortCount; port++) {
		for (unsigned i = 0; i < kVopPortRegisterCount; i++) {
			assert(snapshot.vopPort[port][i] == (vop ? ModelRegister(0xfdd90000,
				kVopPortBase + port * kVopPortStride + kVopPortOffsets[i]) : 0));
		}
	}
	for (unsigned window = 0; window < kVopClusterCount; window++) {
		for (unsigned i = 0; i < kVopClusterRegisterCount; i++) {
			assert(snapshot.vopCluster[window][i] == (vop ? ModelRegister(0xfdd90000,
				kVopClusterBase + window * kVopClusterStride + kVopClusterOffsets[i]) : 0));
		}
	}
	for (unsigned window = 0; window < kVopEsmartCount; window++) {
		for (unsigned i = 0; i < kVopEsmartRegisterCount; i++) {
			assert(snapshot.vopEsmart[window][i] == (vop ? ModelRegister(0xfdd90000,
				kVopEsmartBase + window * kVopEsmartStride + kVopEsmartOffsets[i]) : 0));
		}
	}
	for (unsigned i = 0; i < kHdmiCount; i++)
		assert(snapshot.hdmi[i] == (hdmi ? ModelRegister(0xfdea0000, kHdmiOffsets[i]) : 0));
}


int
main()
{
	static_assert(sizeof(ResourceInfo) == 304, "Diagnostic ABI layout changed");
	static_assert(sizeof(EdidRequest) == 192, "EDID ABI layout changed");
	static_assert(sizeof(DisplaySnapshot) == 784, "Snapshot ABI layout changed");
	static_assert(sizeof(ScanoutRequest) == 88, "Scanout ABI layout changed");
	static_assert(kPatternBytes == 1920 * 1080 * 4, "Pattern is the firmware framebuffer size");
	for (unsigned offset : kVopSystemOffsets) assert(offset + 4 <= kVopMapSize);
	for (unsigned offset : kVopOverlayOffsets) assert(offset + 4 <= kVopMapSize);
	assert(kVopPortBase + 3 * kVopPortStride + 0x54 + 4 <= kVopMapSize);
	assert(kVopClusterBase + 3 * kVopClusterStride + 0x100 + 4 <= kVopMapSize);
	assert(kVopEsmartBase + 3 * kVopEsmartStride + 0x28 + 4 <= kVopMapSize);
	for (unsigned offset : kHdmiOffsets) assert(offset + 4 <= kHdmiMapSize);
	{
		// Registers Linux 6.18 dw-hdmi-qp.c or EDK2 DwHdmiQpLib.c read or read-modify-write.
		static const unsigned kReadableByReference[] = {0x044, 0x0f4, 0x0f8, 0x10c, 0x820, 0x8e0,
			0x968, 0xa9c, 0xaa8, 0x3020, 0x3024};
		for (unsigned offset : kHdmiOffsets) {
			bool listed = false;
			for (unsigned known : kReadableByReference) listed |= known == offset;
			assert(listed);
			assert(offset != 0x0ec); // write-only I2CM_CONTROL0 aborts on read (+259 panic)
		}
	}
	for (unsigned offset : kPmuOffsets) assert(offset + 4 <= 0x400);
	for (unsigned offset : kHdptxGrfOffsets) assert(offset + 4 <= 0x100);
	assert(sysconf(_SC_PAGESIZE) == B_PAGE_SIZE);
	sDeviceManager = &sManager;
	Prepare();
	ResourceInfo good = {};
	assert(ReadResources(&sVop, good) && ResourcesMatch(good));
	assert(good.hdmiPhyPhandle == 0x6c && good.vopInterrupt == 188 && good.hdmiInterrupts[4] == 393);
	assert(good.pmuBase == 0xfd8d8000 && good.clockBase == 0xfd7c0000 && good.hdptxGrfBase == 0xfd5e4000);
	Prepare();
	// Phandles are references, not fixed numerical board identifiers.
	sPhandles.erase(0x21); sPhandles[909] = &sClock;
	sPhandles.erase(0x6c); sPhandles[910] = &sPhy1;
	sPhandles.erase(0x72); sPhandles[911] = &sHdmiEndpoint;
	Cells(sVop, "clocks", {909, 0x25d, 909, 0x25c, 909, 0x261, 909, 0x262, 909, 0x263,
		909, 0x264, 909, 0x25b, 0x6b, 910});
	Cells(sHdmi, "clocks", {909, 0x213, 909, 0x214, 909, 0x215, 909, 0x239, 909, 0x253,
		909, 0x2cd});
	Cells(sHdmi, "resets", {909, 0x1d0, 909, 0x231});
	Cells(sHdmi, "phys", {910});
	Cells(sEndpoint8, "remote-endpoint", {911});
	ResourceInfo moved = {};
	assert(ReadResources(&sVop, moved));
	good.hdmiPhyPhandle = 910;
	assert(memcmp(&moved, &good, sizeof(good)) == 0);
	good.hdmiPhyPhandle = 0x6c;

	std::vector<std::function<void()> > faults = {
		[] { sRoot.properties["compatible"].pop_back(); },
		[] { Strings(sRoot, "compatible", {"radxa,rock-5b", "rockchip,rk3588"}); },
		[] { Strings(sVop, "status", {"disabled"}); },
		[] { Strings(sVop, "compatible", {"rockchip,rk3568-vop"}); },
		[] { sVop.wrongModule = true; },
		[] { sBusNode.wrongModule = true; },
		[] { sVop.size = 0x4000; },
		[] { sVop.base += 0x1000; },
		[] { sVop.size1 = 0; },
		[] { Strings(sVop, "reg-names", {"vop"}); },
		[] { Strings(sVop, "clock-names", {"aclk", "hclk", "dclk_vp0", "dclk_vp1", "dclk_vp2",
			"dclk_vp3", "pclk_vop", "pll_hdmiphy1", "pll_hdmiphy0"}); },
		[] { sVop.properties["clocks"].pop_back(); },
		[] { Cells(sVop, "clocks", {0x21, 0x25d, 0x22, 0x25c, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
			0x21, 0x264, 0x21, 0x25b, 0x6b, 0x6c}); },
		[] { Cells(sVop, "clocks", {0x21, 0x25d, 0x21, 0x25c, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
			0x21, 0x264, 0x21, 0x25b, 0x6c, 0x6c}); },
		[] { Cells(sVop, "clocks", {0x21, 0x25d, 0x21, 0x25c, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
			0x21, 0x264, 0x21, 0x25b, 0x6b, 0x6e}); },
		[] { Cells(sVop, "clocks", {0x21, 0x25c, 0x21, 0x25d, 0x21, 0x261, 0x21, 0x262, 0x21, 0x263,
			0x21, 0x264, 0x21, 0x25b, 0x6b, 0x6c}); },
		[] { Cells(sVop, "interrupts", {0, 157, 4, 0}); },
		[] { sVop.irqs = {189}; },
		[] { sVop.irqController = &sClock; },
		[] { Cells(sVop, "interrupts-extended", {1}); },
		[] { Cells(sVop, "power-domains", {0x22, 25}); },
		[] { Cells(sPower, "#power-domain-cells", {2}); },
		[] { sPower.parent = &sClock; },
		[] { sPmu.wrongModule = true; },
		[] { sPmu.size = 0x100; },
		[] { Cells(sVop, "rockchip,pmu", {0x22}); },
		[] { Cells(sVop, "rockchip,grf", {0x6f}); },
		[] { Strings(sSysGrf, "compatible", {"rockchip,rk3588-sys-grf"}); },
		[] { Strings(sVopGrf, "compatible", {"rockchip,rk3588-vo0-grf", "syscon"}); },
		[] { sVo1Grf.base += 0x1000; },
		[] { Strings(sVo1Grf, "status", {"disabled"}); },
		[] { sClock.wrongModule = true; },
		[] { Cells(sClock, "#clock-cells", {2}); },
		[] { sClock.size = 0x1000; },
		[] { sPorts.name = "port"; },
		[] { sPort1.name = "port@2"; },
		[] { Cells(sPort1, "reg", {2}); },
		[] { sEndpoint8.name = "endpoint@9"; },
		[] { Cells(sEndpoint8, "reg", {9}); },
		[] { sEndpoint8.properties.erase("remote-endpoint"); },
		[] { sPhandles.erase(0x72); },
		[] { Cells(sEndpoint8, "remote-endpoint", {0x6c}); },
		[] { sHdmiPort0.name = "port@1"; Cells(sHdmiPort0, "reg", {1}); },
		[] { sHdmiEndpoint.parent = &sHdmiPorts; },
		[] { Strings(sHdmi, "status", {"disabled"}); },
		[] { Strings(sHdmi, "compatible", {"rockchip,rk3588-dw-hdmi"}); },
		[] { sHdmi.base = 0xfde80000; },
		[] { sHdmi.size = 0x10000; },
		[] { Strings(sHdmi, "clock-names", {"pclk", "earc", "ref", "aud", "hdp"}); },
		[] { sHdmi.properties["clocks"].pop_back(); },
		[] { Cells(sHdmi, "clocks", {0x22, 0x213, 0x21, 0x214, 0x21, 0x215, 0x21, 0x239, 0x21, 0x253,
			0x21, 0x2cd}); },
		[] { Cells(sHdmi, "clocks", {0x21, 0x210, 0x21, 0x211, 0x21, 0x212, 0x21, 0x234, 0x21, 0x252,
			0x21, 0x2cd}); },
		[] { Strings(sHdmi, "interrupt-names", {"avp", "cec", "earc", "hpd", "main"}); },
		[] { sHdmi.irqs = {205, 206, 207, 208, 394}; },
		[] { sHdmi.irqController = &sClock; },
		[] { sHdmi.properties["interrupts"][7] = 1; },
		[] { Cells(sHdmi, "interrupts", {0, 169, 4, 0, 0, 170, 4, 0, 0, 171, 4, 0, 0, 172, 4, 0,
			0, 360, 4, 0}); },
		[] { Strings(sHdmi, "reset-names", {"hdp", "ref"}); },
		[] { Cells(sHdmi, "resets", {0x22, 0x1d0, 0x21, 0x231}); },
		[] { Cells(sHdmi, "power-domains", {0x23, 26}); },
		[] { Cells(sHdmi, "power-domains", {0x22, 25}); },
		[] { Cells(sHdmi, "rockchip,grf", {0x6f}); },
		[] { Cells(sHdmi, "rockchip,vo-grf", {0x6e}); },
		[] { Cells(sHdmi, "phys", {0x6b}); },
		[] { sHdmi.properties.erase("phys"); },
		[] { Strings(sPhy1, "status", {"disabled"}); },
		[] { Strings(sPhy1, "compatible", {"rockchip,rk3588-usbdp-phy"}); },
		[] { sPhy1.base = 0xfed60000; },
		[] { Strings(sPhy1, "clock-names", {"apb", "ref"}); },
		[] { sPhy1.properties.erase("rockchip,grf"); },
		[] { Cells(sPhy1, "rockchip,grf", {0x6e}); },
		[] { sHdptxGrf.size = 0x1000; },
		[] { Strings(sPhy0, "compatible", {"rockchip,rk3588-usbdp-phy"}); },
		[] { Cells(sGic, "#interrupt-cells", {3}); },
		[] { Strings(sGic, "compatible", {"arm,gic-v2"}); },
		[] { sGic.size = 0x20000; },
	};
	for (auto& fault : faults) {
		Prepare(); fault();
		ResourceInfo invalid;
		memset(&invalid, 0xa5, sizeof(invalid));
		assert(!ReadResources(&sVop, invalid));
		for (unsigned char byte : std::vector<unsigned char>((unsigned char*)&invalid,
				(unsigned char*)&invalid + sizeof(invalid))) assert(byte == 0xa5);
	}
	Prepare(); // Also asserts that node references were released on every failure.

	Controller controller{};
	controller.resources = good;
	ResourceInfo copy;
	assert(Control(&controller, kGetResources, &copy, sizeof(copy)) == B_OK);
	assert(memcmp(&copy, &good, sizeof(good)) == 0);
	assert(Control(&controller, kGetResources, NULL, sizeof(copy)) == B_BAD_ADDRESS);
	assert(Control(&controller, kGetResources, &copy, sizeof(copy) - 1) == B_BAD_VALUE);
	assert(Control(&controller, kGetResources, &copy, sizeof(copy) + 1) == B_BAD_VALUE);
	assert(Control(&controller, kGetResources + 127, &copy, sizeof(copy)) == B_DEV_INVALID_IOCTL);
	assert(sMapAttempts == 0);

	DisplaySnapshot snapshot;
	memset(&snapshot, 0xa5, sizeof(snapshot));
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot) - 1) == B_BAD_VALUE);
	assert(Control(&controller, kGetSnapshot, NULL, sizeof(snapshot)) == B_BAD_ADDRESS);
	assert(sMapAttempts == 0);
	// Both blocks powered and clocked: eight read-only mappings, all released.
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 8 && sAreas.empty());
	assert(sMappedBases.back() == 0xfdea0000 && sMappedBases[6] == 0xfdd90000);
	CheckSnapshotValues(snapshot, true, true);
	// VOP power domain off: VOP2 is never mapped; HDMI still observed.
	Prepare(); sRepairStatus = 1u << 18;
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 7 && sAreas.empty());
	CheckSnapshotValues(snapshot, false, true);
	// VOP bus clock gated: same skip.
	Prepare(); sGate52 = 1u << 8;
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 7 && sAreas.empty());
	CheckSnapshotValues(snapshot, false, true);
	// VO1 off or HDMI APB clock gated: HDMI TX1 is never mapped.
	Prepare(); sRepairStatus = 1u << 16;
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 7 && sAreas.empty());
	CheckSnapshotValues(snapshot, true, false);
	Prepare(); sGate61 = 1u << 2;
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 7 && sAreas.empty());
	CheckSnapshotValues(snapshot, true, false);
	Prepare(); sRepairStatus = 0;
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	assert(sMapAttempts == 6 && sAreas.empty());
	CheckSnapshotValues(snapshot, false, false);
	// Every mapping failure is reported and leaves nothing mapped.
	for (unsigned failing = 1; failing <= 8; failing++) {
		Prepare(); sFailMap = failing;
		memset(&snapshot, 0xa5, sizeof(snapshot));
		assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_NO_MEMORY);
		assert(sMapAttempts == failing && sAreas.empty());
		for (unsigned char byte : std::vector<unsigned char>((unsigned char*)&snapshot,
				(unsigned char*)&snapshot + sizeof(snapshot))) assert(byte == 0xa5);
	}
	// An altered description is refused before any mapping.
	Prepare();
	controller.resources.vopBase += 0x1000;
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_NOT_SUPPORTED);
	assert(sMapAttempts == 0);
	controller.resources = good;
	controller.resources.boardCompatible[16] = 'x';
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_NOT_SUPPORTED);
	assert(sMapAttempts == 0);
	assert(sLockDepth == 0);

	// EDID: request validation, opt-in gating, power/hot-plug gating, data and cleanup.
	Prepare();
	controller.resources = good;
	controller.edidEnabled = false;
	EdidRequest edid = {};
	edid.version = kEdidVersion;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid) - 1) == B_BAD_VALUE);
	assert(Control(&controller, kReadEdid, NULL, sizeof(edid)) == B_BAD_ADDRESS);
	edid.version = 2;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_BAD_VALUE);
	edid.version = kEdidVersion;
	edid.block = kEdidMaxBlocks;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_BAD_VALUE);
	edid.block = 0;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_NOT_ALLOWED);
	assert(sMapAttempts == 0);
	controller.edidEnabled = true;
	sAllowEdid = true;
	sRepairStatus = 1u << 16;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidNotReady && sMapAttempts == 3 && sAreas.empty() && edid.bytesRead == 0);
	Prepare(); sAllowEdid = true; sGate61 = 1u << 2;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidNotReady && sMapAttempts == 3 && sAreas.empty());
	Prepare(); sAllowEdid = true; sHotPlug = 1u << 27;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidNoHotPlug && sMapAttempts == 3 && sAreas.empty() && edid.hotPlug == (1u << 27));
	for (unsigned block = 0; block < 4; block++) {
		Prepare(); sAllowEdid = true;
		memset(&edid, 0xa5, sizeof(edid));
		edid.version = kEdidVersion;
		edid.block = block;
		assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_OK);
		assert(edid.result == kEdidOK && edid.bytesRead == 128 && edid.block == block);
		assert(memcmp(edid.data, sEdid + block * 128, 128) == 0);
		assert(sMapAttempts == 4 && sAreas.empty() && sServed == 128 && sResets == 0);
		assert(edid.flags == (block >= 2 ? kEdidSegmentUsed : 0));
		assert((edid.controlAfter & kI2cmWriteMask) == 0 && (edid.statusAfter & 0x5) == 0);
		assert(edid.finishedMicros > edid.startedMicros && edid.polls == 0);
		assert(edid.hotPlug == ((1u << 24) | (1u << 27)));
	}
	// A NACK part-way through aborts the transfer, resets the master and clears requests.
	Prepare(); sAllowEdid = true; sNackAt = 17;
	memset(&edid, 0, sizeof(edid));
	edid.version = kEdidVersion;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidNack && edid.bytesRead == 17 && sResets == 1 && sAreas.empty());
	assert((edid.flags & kEdidMasterReset) != 0 && (edid.controlAfter & kI2cmWriteMask) == 0);
	assert((edid.statusAfter & 0x5) == 0 && memcmp(edid.data, sEdid, 17) == 0 && edid.data[17] == 0);
	// A silent bus times out after the bounded poll count and leaves the master idle.
	Prepare(); sAllowEdid = true; sUnresponsive = true;
	memset(&edid, 0, sizeof(edid));
	edid.version = kEdidVersion;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_OK);
	assert(edid.result == kEdidTimeout && edid.bytesRead == 0 && edid.polls == kEdidPollLimit);
	assert(sResets == 1 && sAreas.empty() && (edid.controlAfter & kI2cmWriteMask) == 0);
	// A failed HDMI mapping is reported and nothing stays mapped.
	Prepare(); sAllowEdid = true; sFailMap = 4;
	assert(Control(&controller, kReadEdid, &edid, sizeof(edid)) == B_NO_MEMORY);
	assert(sMapAttempts == 4 && sAreas.empty());
	assert(sLockDepth == 0);

	// Scanout swap: request validation, opt-in gating, power gating, window
	// location, the two permitted writes, restore, restore-on-close and failures.
	Prepare();
	controller.resources = good;
	controller.scanoutEnabled = false;
	ScanoutRequest scan = {};
	scan.version = kScanoutVersion;
	assert(Control(&controller, kSwapScanout, &scan, sizeof(scan) - 1) == B_BAD_VALUE);
	assert(Control(&controller, kSwapScanout, NULL, sizeof(scan)) == B_BAD_ADDRESS);
	scan.version = 2;
	assert(Control(&controller, kSwapScanout, &scan, sizeof(scan)) == B_BAD_VALUE);
	scan.version = kScanoutVersion;
	scan.action = kScanoutRestore + 1;
	assert(Control(&controller, kSwapScanout, &scan, sizeof(scan)) == B_BAD_VALUE);
	scan.action = kScanoutQuery;
	assert(Control(&controller, kSwapScanout, &scan, sizeof(scan)) == B_NOT_ALLOWED);
	assert(sMapAttempts == 0);
	controller.scanoutEnabled = true;
	auto scanout = [&](uint32 action, uint32 expected, bool writable) {
		sAllowScanout = writable;
		memset(&scan, 0xa5, sizeof(scan));
		scan.version = kScanoutVersion;
		scan.action = action;
		assert(Control(&controller, kSwapScanout, &scan, sizeof(scan)) == B_OK);
		assert(scan.result == expected && scan.action == action && scan.version == kScanoutVersion);
		assert(scan.finishedMicros > scan.startedMicros && sAreas.empty());
		sAllowScanout = false;
	};
	// A query never maps VOP2 writable and needs the VOP domain and bus clocks.
	sRepairStatus = 1u << 18;
	scanout(kScanoutQuery, kScanoutNotReady, false);
	assert(sMapAttempts == 2 && scan.port == 0 && scan.addressBefore == 0);
	Prepare(); sGate52 = 1u << 9;
	scanout(kScanoutQuery, kScanoutNotReady, false);
	assert(sMapAttempts == 2);
	Prepare();
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.port == 2 && scan.window == 2 && scan.flags == 0 && sMapAttempts == 3);
	assert(scan.addressBefore == kModelFirmwareAddress && scan.addressAfter == 0);
	assert(scan.firmwareAddress == 0 && scan.patternAddress == 0 && scan.configDone == 0);
	assert(scan.regionControl == 1 && scan.virtualWidth == 1920 && scan.activeInfo == 0x0437077f);
	assert(scan.displayInfo == 0x0437077f && scan.displayStart == 0 && scan.interfaceEnable == 0x00080020);
	assert(sVopWrites.empty());
	// Window location rejects every deviation from the qualified firmware state.
	Prepare(); sVopOverrides[0x028] = 0x00080000; scanout(kScanoutQuery, kScanoutNoWindow, false); // HDMI1 off
	Prepare(); sVopOverrides[0x028] = 0x00040020; scanout(kScanoutQuery, kScanoutNoWindow, false); // port 1 (standby)
	Prepare(); sVopOverrides[0xe00] = 0x8000000f; scanout(kScanoutQuery, kScanoutNoWindow, false); // port 2 standby
	Prepare(); sVopOverrides[0x1810] = 1; scanout(kScanoutQuery, kScanoutNoWindow, false); // two windows
	Prepare(); sVopOverrides[0x1c10] = 0; scanout(kScanoutQuery, kScanoutNoWindow, false); // no window
	Prepare(); sVopOverrides[0x1c10] = 1 | (2 << 1); scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	Prepare(); sVopOverrides[0x1c1c] = 1921; scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	Prepare(); sVopOverrides[0x1c20] = 0x0437077e; scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	Prepare(); sVopOverrides[0x1c24] = 0x0433077f; scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	Prepare(); sVopOverrides[0x1c28] = 0x00010000; scanout(kScanoutQuery, kScanoutUnexpectedState, false);
	assert(sVopWrites.empty() && sPatternArea < 0);
	// Showing the pattern requires the firmware framebuffer to match the boot item.
	Prepare(); sBootInfoPresent = false; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	assert(sVopWrites.empty() && sPatternArea < 0 && scan.flags == 0 && sMapAttempts == 3);
	Prepare(); sBootInfo.physical_frame_buffer = 0xed281000; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	Prepare(); sBootInfo.width = 1280; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	Prepare(); sBootInfo.height = 1024; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	Prepare(); sBootInfo.bytes_per_row = 7684; scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	assert(sVopWrites.empty() && sPatternArea < 0 && sNoncacheableCalls == 0);
	Prepare(); sFailPattern = 1; scanout(kScanoutShowPattern, kScanoutNoBuffer, true);
	assert(sVopWrites.empty() && sPatternArea < 0 && sNoncacheableCalls == 0 && scan.flags == 0);
	Prepare(); sPatternHighPhysical = true; scanout(kScanoutShowPattern, kScanoutNoBuffer, true);
	assert(sVopWrites.empty() && sPatternArea < 0 && sNoncacheableCalls == 0 && sPatternAllocations == 1);
	Prepare(); sFailMap = 3; sAllowScanout = true;
	memset(&scan, 0, sizeof(scan));
	scan.version = kScanoutVersion;
	scan.action = kScanoutShowPattern;
	assert(Control(&controller, kSwapScanout, &scan, sizeof(scan)) == B_NO_MEMORY);
	assert(sMapAttempts == 3 && sAreas.empty() && sVopWrites.empty() && sPatternArea < 0);
	// The swap itself: exactly two writes, in order, with a verified read-back.
	Prepare();
	scanout(kScanoutShowPattern, kScanoutOK, true);
	assert(scan.flags == kScanoutSwapped && scan.port == 2 && scan.window == 2 && sMapAttempts == 3);
	assert(scan.addressBefore == kModelFirmwareAddress && scan.addressAfter == kModelPatternPhysical);
	assert(scan.firmwareAddress == kModelFirmwareAddress && scan.patternAddress == kModelPatternPhysical);
	assert(scan.configDone == (0x8000u | (1u << 2) | (1u << 18)) && scan.polls == 2);
	assert(sVopWrites.size() == 2);
	assert(sVopWrites[0] == std::make_pair(kModelAddressOffset, (uint32)kModelPatternPhysical));
	assert(sVopWrites[1] == std::make_pair(0x000u, 0x00048004u));
	assert(sNoncacheableCalls == 1 && sPatternArea >= 0 && sPatternAllocations == 1);
	assert(sModelSpins == 2); // one pause per poll that saw the port bit set
	// Observation reports the pattern address through its own read-only mapping.
	sVopWrites.clear();
	assert(Control(&controller, kGetSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
	CheckSnapshotValues(snapshot, true, true);
	assert(snapshot.vopEsmart[2][2] == kModelPatternPhysical && sVopWrites.empty());
	// Showing again is idempotent; a foreign window address is refused.
	scanout(kScanoutShowPattern, kScanoutOK, true);
	assert(scan.flags == kScanoutSwapped && scan.addressBefore == kModelPatternPhysical && sVopWrites.empty());
	sVopOverrides[kModelAddressOffset] = 0x11111000;
	scanout(kScanoutShowPattern, kScanoutUnexpectedState, true);
	assert(scan.flags == kScanoutSwapped && sVopWrites.empty());
	sVopOverrides[kModelAddressOffset] = kModelPatternPhysical;
	// Restore writes the firmware address back and keeps the buffer for reuse.
	scanout(kScanoutRestore, kScanoutOK, true);
	assert(scan.flags == 0 && scan.addressBefore == kModelPatternPhysical && scan.addressAfter == kModelFirmwareAddress);
	assert(scan.firmwareAddress == kModelFirmwareAddress && scan.patternAddress == kModelPatternPhysical);
	assert(scan.polls == 2 && sModelSpins == 4);
	assert(sVopWrites.size() == 2 && sVopWrites[0] == std::make_pair(kModelAddressOffset, kModelFirmwareAddress));
	assert(sVopWrites[1] == std::make_pair(0x000u, 0x00048004u) && sPatternArea >= 0 && sNoncacheableCalls == 1);
	sVopWrites.clear();
	scanout(kScanoutRestore, kScanoutNotSwapped, true);
	assert(sVopWrites.empty() && scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress && scan.polls == 0);
	// Close restores a pending swap and otherwise touches nothing.
	unsigned attempts = sMapAttempts;
	assert(Close(&controller) == B_OK && sMapAttempts == attempts && sVopWrites.empty());
	scanout(kScanoutShowPattern, kScanoutOK, true);
	assert(sNoncacheableCalls == 1 && sPatternAllocations == 1); // buffer reused, not refilled
	sVopWrites.clear();
	sAllowScanout = true;
	assert(Close(&controller) == B_OK);
	sAllowScanout = false;
	assert(sVopWrites.size() == 2 && sVopWrites[0] == std::make_pair(kModelAddressOffset, kModelFirmwareAddress));
	assert(sAreas.empty());
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress);
	// A swap that cannot be read back stays pending and is retried on close.
	sVopWrites.clear();
	sStickyAddress = true;
	scanout(kScanoutShowPattern, kScanoutVerifyFailed, true);
	assert(scan.flags == kScanoutSwapped && scan.addressAfter == kModelFirmwareAddress && sVopWrites.size() == 2);
	assert(scan.polls == 2);
	sStickyAddress = false;
	sVopWrites.clear();
	sAllowScanout = true;
	assert(Close(&controller) == B_OK);
	sAllowScanout = false;
	assert(!sVopWrites.empty() && sVopWrites.back() == std::make_pair(0x000u, 0x00048004u));
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress);
	// A port that never takes the commit times out after the bounded polls;
	// the swap stays pending and restore succeeds once the port runs again.
	sVopWrites.clear();
	sCommitNeverCompletes = true;
	unsigned spins = sModelSpins;
	scanout(kScanoutShowPattern, kScanoutTimeout, true);
	assert(scan.flags == kScanoutSwapped && scan.polls == kScanoutPollLimit && sModelSpins == spins + kScanoutPollLimit);
	assert(scan.addressAfter == kModelFirmwareAddress && sVopWrites.size() == 2);
	sVopWrites.clear();
	sAllowScanout = true;
	assert(Close(&controller) == B_OK);
	sAllowScanout = false;
	assert(sVopWrites.size() == 1 && sVopWrites[0] == std::make_pair(0x000u, 0x00048004u));
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == kScanoutSwapped);
	sCommitNeverCompletes = false;
	sVopWrites.clear();
	sAllowScanout = true;
	assert(Close(&controller) == B_OK);
	sAllowScanout = false;
	assert(sVopWrites.size() == 1 && sVopWrites[0] == std::make_pair(0x000u, 0x00048004u));
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress);
	// With the VOP domain off a pending swap waits for power to return.
	scanout(kScanoutShowPattern, kScanoutOK, true);
	sVopWrites.clear();
	sRepairStatus = 1u << 18;
	sAllowScanout = true;
	assert(Close(&controller) == B_OK);
	assert(sVopWrites.empty() && sAreas.empty());
	sRepairStatus = (1u << 16) | (1u << 18);
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == kScanoutSwapped && scan.addressBefore == kModelPatternPhysical);
	sAllowScanout = true;
	assert(Close(&controller) == B_OK);
	sAllowScanout = false;
	assert(sVopWrites.size() == 2 && sVopWrites[0] == std::make_pair(kModelAddressOffset, kModelFirmwareAddress));
	scanout(kScanoutQuery, kScanoutOK, false);
	assert(scan.flags == 0 && scan.addressBefore == kModelFirmwareAddress);
	ReleasePattern();
	assert(sPatternArea < 0 && sPatternAllocation == NULL);
	assert(sLockDepth == 0);
	printf("RK3588_DISPLAY_RESOURCES_TEST_PASS faults=%zu\n", faults.size());
	return 0;
}
