/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Host fixture for the RK3588 display observation driver. The production
// device-tree traversal, ioctl and read-only observation run against a modeled
// device manager and guarded register pages; nothing here touches hardware.

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
	B_NOT_ALLOWED = -6, B_ENTRY_NOT_FOUND = -8;
static const unsigned B_PAGE_SIZE = 4096, B_ANY_KERNEL_ADDRESS = 4,
	B_UNCACHED_MEMORY = 1u << 28, B_KERNEL_READ_AREA = 1u << 4,
	B_KERNEL_WRITE_AREA = 1u << 5;

#include "DisplayObservation.h"

using namespace RK3588Display;

static std::map<int, std::pair<void*, size_t> > sAreas;
static unsigned sMapAttempts, sFailMap;
static std::vector<uint64> sMappedBases;
static int64_t sTime;
static unsigned sLockDepth;
static uint32 sRepairStatus = (1u << 16) | (1u << 18);
static uint32 sGate52, sGate61;

struct mutex {};
#define MUTEX_INITIALIZER(name) {}
class MutexLocker {
public:
	explicit MutexLocker(mutex&) { assert(sLockDepth++ == 0); }
	~MutexLocker() { assert(--sLockDepth == 0); }
};

static int64_t system_time() { return ++sTime; }
static void memory_read_barrier() {}


static uint32
ModelRegister(uint64 base, unsigned offset)
{
	if (base == 0xfd8d8000 && offset == 0x290)
		return sRepairStatus;
	if (base == 0xfd7c0000 && offset == 0x8d0)
		return sGate52;
	if (base == 0xfd7c0000 && offset == 0x8f4)
		return sGate61;
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
	if (control)
		assert(bytes == B_PAGE_SIZE);
	else if (base == 0xfdd90000)
		assert(bytes == kVopMapSize && (sRepairStatus & (1u << 16)) != 0 && (sGate52 & 0x300) == 0);
	else if (base == 0xfdea0000)
		assert(bytes == kHdmiMapSize && (sRepairStatus & (1u << 18)) != 0 && (sGate61 & 4) == 0);
	else
		assert(false);
	assert(spec == (B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY));
	// Never a writable mapping of any display or control block.
	assert(protection == B_KERNEL_READ_AREA);
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
	// Any production write faults immediately, as would either guard page.
	assert(mprotect(registers, bytes, PROT_READ) == 0);
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
	static_assert(sizeof(DisplaySnapshot) == 800, "Snapshot ABI layout changed");
	for (unsigned offset : kVopSystemOffsets) assert(offset + 4 <= kVopMapSize);
	for (unsigned offset : kVopOverlayOffsets) assert(offset + 4 <= kVopMapSize);
	assert(kVopPortBase + 3 * kVopPortStride + 0x54 + 4 <= kVopMapSize);
	assert(kVopClusterBase + 3 * kVopClusterStride + 0x100 + 4 <= kVopMapSize);
	assert(kVopEsmartBase + 3 * kVopEsmartStride + 0x28 + 4 <= kVopMapSize);
	for (unsigned offset : kHdmiOffsets) assert(offset + 4 <= kHdmiMapSize);
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
	printf("RK3588_DISPLAY_RESOURCES_TEST_PASS faults=%zu\n", faults.size());
	return 0;
}
