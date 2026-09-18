/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <bus/FDT.h>
#include <KernelExport.h>
#include <AutoDeleterOS.h>
#include <boot_item.h>
#include <driver_settings.h>
#include <frame_buffer_console.h>
#include <graphic_driver.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#if defined(__aarch64__)
#include <arch/arm64/cache_line_size.h>
#endif
#include <fcntl.h>
#include <new>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#include "DisplayScanout.h"
#include "DisplayAccelerant.h"
#include "DisplayModeSet.h"
#include "DisplayCursor.h"


using namespace RK3588Display;

#define DRIVER_NAME "drivers/graphics/rk3588_display/driver_v1"
#define DEVICE_NAME "drivers/graphics/rk3588_display/device_v1"

// Kernel-only: evict the cached fill of the pattern buffer, then retype the
// area write-combining so later CPU stores reach RAM before the VOP2 reads
// it (the Mali client buffer pattern). The host fixture models this call.
static status_t
MakeBufferNoncacheable(area_id area, void* address, size_t bytes)
{
#if defined(__aarch64__)
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)address; p < (addr_t)address + bytes; p += line)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	status_t status = vm_set_area_memory_type(area, 0, B_WRITE_COMBINING_MEMORY);
	memory_full_barrier();
	return status;
#else
	(void)area; (void)address; (void)bytes;
	return B_NOT_SUPPORTED;
#endif
}


static device_manager_info* sDeviceManager;
static mutex sHardwareLock = MUTEX_INITIALIZER("RK3588 display platform");

struct Controller {
	device_node* node;
	ResourceInfo resources;
	bool edidEnabled;
	bool scanoutEnabled;
	bool accelerantEnabled;
	bool modeSetEnabled;
	bool cursorEnabled;
};

// One open file handle. Only writable handles (the accelerant profile) may
// acquire the frame buffer; the acquiring handle releases it when closed.
struct Handle {
	Controller* controller;
	bool writable;
};

// Driver-owned scanout buffers (the swap test pattern and the accelerant
// frame buffer). Physically contiguous below 4 GiB (VOP2 window addresses
// are 32-bit), filled through the cached alias, evicted and then mapped
// write-combining like the Mali client buffers.
struct ContiguousBuffer {
	area_id area;
	void* address;
	uint32_t physical;
};
static ContiguousBuffer sPattern = {-1, NULL, 0};
static ContiguousBuffer sFrame = {-1, NULL, 0};
static bool sScanoutSwapped = false;
static uint32_t sFirmwareAddress = 0;
static area_id sSharedArea = -1;
static SharedInfo* sShared = NULL;
static Handle* sOwner = NULL;
static AccelerantInfo sAccelerant = {};
// While the frame buffer is acquired VOP2 stays mapped for the frame-start
// interrupt handler; the semaphore is released only towards waiting threads.
static area_id sVopArea = -1;
static volatile uint32* sVopRegisters = NULL;
static sem_id sRetraceSemaphore = -1;
static int32 sRetraces = 0;
static int32 sInterruptCalls = 0;
static int32 sInterruptSpurious = 0;
static int64 sFirstRetraceMicros = 0;
static int64 sLastRetraceMicros = 0;
static bool sInterruptInstalled = false;
static int32 sHoldValid = 0; // the port reported standby (DSP_HOLD_VALID)
static ModeRequest sCurrentMode = {}; // the mode the driver set, if any
static ModeRequest sFirmwareMode = {}; // the firmware's mode as a request
static uint32 sPowerMode = kPowerOn;
static ContiguousBuffer sCursor = {-1, NULL, 0};
static CursorState sCursorState = {};
static bool sCursorProgrammed = false; // the cursor window was written since acquisition
static status_t RestoreScanout(Controller* controller);
static status_t AcquireFrameBuffer(Handle* handle);
static status_t ChangeDisplayMode(Handle* handle, ModeRequest& request);
static status_t ChangePowerMode(Handle* handle, PowerRequest& request);
static status_t CursorControl(Handle* handle, uint32 op, void* buffer, size_t length);
static uint32_t ProgramCursor(uint32_t& polls);
static int32 RetraceInterrupt(void* data);
static void ReleaseFrameBuffer(Controller* controller);


#include "DisplayHardware.h"


static void
ReleaseContiguous(ContiguousBuffer& buffer)
{
	if (buffer.area >= B_OK)
		delete_area(buffer.area);
	buffer.area = -1;
	buffer.address = NULL;
	buffer.physical = 0;
}


static status_t
AllocateContiguous(ContiguousBuffer& buffer, const char* name, size_t bytes, bool pattern)
{
	if (buffer.area >= B_OK)
		return B_OK;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 0x100000000ull;
	physicalRestrictions.alignment = B_PAGE_SIZE;
	buffer.area = create_area_etc(B_SYSTEM_TEAM, name, bytes, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, &buffer.address);
	if (buffer.area < B_OK)
		return buffer.area;
	physical_entry entry;
	status_t status = get_memory_map(buffer.address, bytes, &entry, 1);
	if (status == B_OK && (entry.size < bytes || entry.address + bytes > 0x100000000ull))
		status = B_BAD_VALUE;
	if (status == B_OK) {
		buffer.physical = (uint32_t)entry.address;
		uint32_t* pixels = (uint32_t*)buffer.address;
		if (pattern) {
			for (uint32_t y = 0; y < kPatternHeight; y++) {
				for (uint32_t x = 0; x < kPatternWidth; x++)
					pixels[y * kPatternWidth + x] = PatternPixel(x, y);
			}
		} else
			memset(pixels, 0, bytes);
		status = MakeBufferNoncacheable(buffer.area, buffer.address, bytes);
	}
	if (status != B_OK)
		ReleaseContiguous(buffer);
	return status;
}


class FdtNode {
public:
	bool SetTo(device_node* node)
	{
		driver_module_info* module;
		void* cookie;
		if (node == NULL
			|| sDeviceManager->get_driver(node, &module, &cookie) != B_OK
			|| strcmp(module->info.name, "bus_managers/fdt/driver_v1") != 0) {
			return false;
		}
		fModule = (fdt_device_module_info*)module;
		fDevice = (fdt_device*)cookie;
		return true;
	}

	bool HasString(const char* property, const char* value) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		return StringIndex(data, length, value) >= 0;
	}

	bool Cells(const char* property, uint32_t* cells, size_t count) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		return ReadCells(data, length, cells, count);
	}

	bool Names(const char* property, const char* const* names, size_t count) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		size_t expected = 0;
		for (size_t i = 0; i < count; i++) {
			expected += strlen(names[i]) + 1;
			if (StringIndex(data, length, names[i]) != (int)i)
				return false;
		}
		return length >= 0 && (size_t)length == expected;
	}

	bool Reg(uint32_t index, uint64_t& base, uint64_t& size) const
	{
		uint64 address, length;
		if (!fModule->get_reg(fDevice, index, &address, &length))
			return false;
		base = address;
		size = length;
		return true;
	}

	bool Enabled() const
	{
		int length = 0;
		const void* status = fModule->get_prop(fDevice, "status", &length);
		return status == NULL || (length == 5 && memcmp(status, "okay", 5) == 0)
			|| (length == 3 && memcmp(status, "ok", 3) == 0);
	}

	fdt_device_module_info* fModule = NULL;
	fdt_device* fDevice = NULL;
};


class NodeReference {
public:
	explicit NodeReference(device_node* node = NULL) : fNode(node) {}
	~NodeReference() { SetTo(NULL); }
	void SetTo(device_node* node)
	{
		if (fNode != NULL)
			sDeviceManager->put_node(fNode);
		fNode = node;
	}
	device_node* Get() const { return fNode; }
private:
	device_node* fNode;
};


static bool
BoardMatches(device_node* vop)
{
	device_node* node = sDeviceManager->get_parent_node(vop);
	while (node != NULL) {
		FdtNode fdt;
		if (!fdt.SetTo(node)) {
			sDeviceManager->put_node(node);
			return false;
		}
		bool found = strcmp(fdt.fModule->get_name(fdt.fDevice), "") == 0;
		if (found) {
			bool matches = fdt.HasString("compatible", "radxa,rock-5-itx");
			sDeviceManager->put_node(node);
			return matches;
		}
		device_node* parent = sDeviceManager->get_parent_node(node);
		sDeviceManager->put_node(node);
		node = parent;
	}
	return false;
}


static device_node*
FindNamedChild(device_node* parent, const char* name)
{
	device_attr attributes[] = {
		{"fdt/name", B_STRING_TYPE, {.string = name}},
		{}
	};
	device_node* node = NULL;
	if (sDeviceManager->find_child_node(parent, attributes, &node) != B_OK)
		return NULL;
	device_node* actualParent = sDeviceManager->get_parent_node(node);
	bool directChild = actualParent == parent;
	if (actualParent != NULL)
		sDeviceManager->put_node(actualParent);
	if (!directChild) {
		sDeviceManager->put_node(node);
		return NULL;
	}
	return node;
}


static bool
ReadSyscon(fdt_bus_module_info* busModule, fdt_bus* bus, uint32_t phandle,
	const char* compatible, uint64_t& base, uint64_t& size)
{
	FdtNode node;
	return node.SetTo(busModule->node_by_phandle(bus, phandle)) && node.Enabled()
		&& node.HasString("compatible", compatible) && node.HasString("compatible", "syscon")
		&& node.Reg(0, base, size);
}


static bool
ReadInterrupts(const FdtNode& node, unsigned count, const uint32_t* expectedSpi,
	uint32_t* output, device_node*& gicNode)
{
	uint32_t specifiers[20];
	if (count > 5 || !node.Cells("interrupts", specifiers, count * 4)
		|| node.fModule->get_prop(node.fDevice, "interrupts-extended", NULL) != NULL) {
		return false;
	}
	for (unsigned i = 0; i < count; i++) {
		device_node* controller;
		uint64 irq;
		if (specifiers[i * 4] != 0 || specifiers[i * 4 + 1] != expectedSpi[i]
			|| specifiers[i * 4 + 2] != 4 || specifiers[i * 4 + 3] != 0
			|| !node.fModule->get_interrupt(node.fDevice, i, &controller, &irq)
			|| controller == NULL || irq != expectedSpi[i] + 32
			|| (gicNode != NULL && gicNode != controller)) {
			return false;
		}
		gicNode = controller;
		output[i] = irq;
	}
	return true;
}


// Admits exactly the ROCK 5 ITX display description: the VOP2 node, its video
// port 1 endpoint leading to the enabled DW HDMI QP TX1 controller, that
// controller's HDPTX PHY and GRF, and the shared control blocks. Phandles
// stay dynamic; register bases, interrupts and clock identities are compared
// against the recorded firmware description before anything is mapped.
static bool
ReadResources(device_node* parent, ResourceInfo& output)
{
	ResourceInfo info = {};
	FdtNode vop;
	if (!vop.SetTo(parent) || !vop.Enabled()
		|| !vop.HasString("compatible", "rockchip,rk3588-vop")
		|| !BoardMatches(parent)
		|| !vop.Reg(0, info.vopBase, info.vopSize)
		|| !vop.Reg(1, info.vopLutBase, info.vopLutSize)) {
		return false;
	}
	const char* const vopRegNames[] = {"vop", "gamma-lut"};
	const char* const vopClockNames[] = {"aclk", "hclk", "dclk_vp0", "dclk_vp1",
		"dclk_vp2", "dclk_vp3", "pclk_vop", "pll_hdmiphy0", "pll_hdmiphy1"};
	uint32_t vopClocks[16], power[2], sysGrf, vopGrf, vo1Grf, pmu;
	if (!vop.Names("reg-names", vopRegNames, 2)
		|| !vop.Names("clock-names", vopClockNames, 9)
		|| !vop.Cells("clocks", vopClocks, 16)
		|| !vop.Cells("power-domains", power, 2)
		|| !vop.Cells("rockchip,grf", &sysGrf, 1)
		|| !vop.Cells("rockchip,vop-grf", &vopGrf, 1)
		|| !vop.Cells("rockchip,vo1-grf", &vo1Grf, 1)
		|| !vop.Cells("rockchip,pmu", &pmu, 1)) {
		return false;
	}
	device_node* gicNode = NULL;
	const uint32_t vopSpi[] = {156};
	if (!ReadInterrupts(vop, 1, vopSpi, &info.vopInterrupt, gicNode))
		return false;
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(vop.fModule->get_bus(vop.fDevice),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK
		|| strcmp(busModule->info.info.name, "bus_managers/fdt/root/driver_v1") != 0) {
		return false;
	}
	for (unsigned i = 1; i < 7; i++) {
		if (vopClocks[i * 2] != vopClocks[0])
			return false;
	}
	for (unsigned i = 0; i < 7; i++)
		info.vopClockIds[i] = vopClocks[i * 2 + 1];
	FdtNode clock;
	uint32_t cells;
	if (vopClocks[0] == 0 || !clock.SetTo(busModule->node_by_phandle(bus, vopClocks[0]))
		|| !clock.Enabled() || !clock.HasString("compatible", "rockchip,rk3588-cru")
		|| !clock.Cells("#clock-cells", &cells, 1) || cells != 1
		|| !clock.Reg(0, info.clockBase, info.clockSize)) {
		return false;
	}
	// The two PHY pixel clocks are single-cell references to the HDPTX PHYs.
	FdtNode phy0, phy1;
	if (!phy0.SetTo(busModule->node_by_phandle(bus, vopClocks[14]))
		|| !phy0.HasString("compatible", "rockchip,rk3588-hdptx-phy")
		|| !phy1.SetTo(busModule->node_by_phandle(bus, vopClocks[15]))
		|| !phy1.HasString("compatible", "rockchip,rk3588-hdptx-phy")
		|| vopClocks[14] == vopClocks[15]) {
		return false;
	}
	device_node* powerNode = busModule->node_by_phandle(bus, power[0]);
	FdtNode powerController;
	if (!powerController.SetTo(powerNode) || !powerController.Enabled()
		|| !powerController.HasString("compatible", "rockchip,rk3588-power-controller")
		|| !powerController.Cells("#power-domain-cells", &cells, 1) || cells != 1) {
		return false;
	}
	device_node* pmuParent = sDeviceManager->get_parent_node(powerNode);
	FdtNode pmuNode;
	bool validPmu = pmuNode.SetTo(pmuParent) && pmuNode.Enabled()
		&& pmuNode.HasString("compatible", "rockchip,rk3588-pmu")
		&& pmuNode.Reg(0, info.pmuBase, info.pmuSize)
		&& busModule->node_by_phandle(bus, pmu) == pmuParent;
	if (pmuParent != NULL)
		sDeviceManager->put_node(pmuParent);
	if (!validPmu)
		return false;
	info.vopPowerDomain = power[1];
	if (!ReadSyscon(busModule, bus, sysGrf, "rockchip,rk3588-sys-grf",
			info.sysGrfBase, info.sysGrfSize)
		|| !ReadSyscon(busModule, bus, vopGrf, "rockchip,rk3588-vop-grf",
			info.vopGrfBase, info.vopGrfSize)
		|| !ReadSyscon(busModule, bus, vo1Grf, "rockchip,rk3588-vo1-grf",
			info.vo1GrfBase, info.vo1GrfSize)) {
		return false;
	}

	// Follow VOP video port 1 to the HDMI TX1 input endpoint.
	NodeReference ports(FindNamedChild(parent, "ports"));
	NodeReference port(FindNamedChild(ports.Get(), "port@1"));
	NodeReference endpoint(FindNamedChild(port.Get(), "endpoint@8"));
	FdtNode portNode, endpointNode;
	uint32_t portIndex, endpointIndex, remote;
	if (!portNode.SetTo(port.Get()) || !portNode.Cells("reg", &portIndex, 1)
		|| portIndex != 1 || !endpointNode.SetTo(endpoint.Get())
		|| !endpointNode.Cells("reg", &endpointIndex, 1) || endpointIndex != 8
		|| !endpointNode.Cells("remote-endpoint", &remote, 1)) {
		return false;
	}
	info.vopPortIndex = portIndex;
	// node_by_phandle returns a borrowed node; only get_parent_node results
	// carry references that must be released.
	device_node* remoteEndpoint = busModule->node_by_phandle(bus, remote);
	if (remoteEndpoint == NULL)
		return false;
	NodeReference hdmiPort0(sDeviceManager->get_parent_node(remoteEndpoint));
	NodeReference hdmiPortsNode(hdmiPort0.Get() == NULL ? NULL
		: sDeviceManager->get_parent_node(hdmiPort0.Get()));
	NodeReference hdmiNode(hdmiPortsNode.Get() == NULL ? NULL
		: sDeviceManager->get_parent_node(hdmiPortsNode.Get()));
	// The remote endpoint sits in hdmi/ports/port@0.
	FdtNode remotePortNode, remoteEndpointNode, portsNode;
	uint32_t remotePortIndex;
	if (!remoteEndpointNode.SetTo(remoteEndpoint)
		|| !remotePortNode.SetTo(hdmiPort0.Get())
		|| !remotePortNode.Cells("reg", &remotePortIndex, 1) || remotePortIndex != 0
		|| strcmp(remotePortNode.fModule->get_name(remotePortNode.fDevice), "port@0") != 0
		|| !portsNode.SetTo(hdmiPortsNode.Get())
		|| strcmp(portsNode.fModule->get_name(portsNode.fDevice), "ports") != 0) {
		return false;
	}
	FdtNode hdmi;
	if (!hdmi.SetTo(hdmiNode.Get()) || !hdmi.Enabled()
		|| !hdmi.HasString("compatible", "rockchip,rk3588-dw-hdmi-qp")
		|| !hdmi.Reg(0, info.hdmiBase, info.hdmiSize)) {
		return false;
	}
	const char* const hdmiClockNames[] = {"pclk", "earc", "ref", "aud", "hdp", "hclk_vo1"};
	const char* const hdmiInterruptNames[] = {"avp", "cec", "earc", "main", "hpd"};
	const char* const hdmiResetNames[] = {"ref", "hdp"};
	uint32_t hdmiClocks[12], hdmiPower[2], hdmiGrf, hdmiVoGrf, hdmiPhy, resets[4];
	if (!hdmi.Names("clock-names", hdmiClockNames, 6)
		|| !hdmi.Cells("clocks", hdmiClocks, 12)
		|| !hdmi.Names("interrupt-names", hdmiInterruptNames, 5)
		|| !hdmi.Names("reset-names", hdmiResetNames, 2)
		|| !hdmi.Cells("resets", resets, 4)
		|| !hdmi.Cells("power-domains", hdmiPower, 2) || hdmiPower[0] != power[0]
		|| !hdmi.Cells("rockchip,grf", &hdmiGrf, 1) || hdmiGrf != sysGrf
		|| !hdmi.Cells("rockchip,vo-grf", &hdmiVoGrf, 1) || hdmiVoGrf != vo1Grf
		|| !hdmi.Cells("phys", &hdmiPhy, 1) || hdmiPhy != vopClocks[15]) {
		return false;
	}
	for (unsigned i = 0; i < 6; i++) {
		if (hdmiClocks[i * 2] != vopClocks[0])
			return false;
		info.hdmiClockIds[i] = hdmiClocks[i * 2 + 1];
	}
	if (resets[0] != vopClocks[0] || resets[2] != vopClocks[0])
		return false;
	const uint32_t hdmiSpi[] = {173, 174, 175, 176, 361};
	if (!ReadInterrupts(hdmi, 5, hdmiSpi, info.hdmiInterrupts, gicNode))
		return false;
	info.hdmiPowerDomain = hdmiPower[1];
	info.hdmiPhyPhandle = hdmiPhy;
	uint32_t phyGrf;
	const char* const phyClockNames[] = {"ref", "apb"};
	if (!phy1.Enabled() || !phy1.Reg(0, info.hdptxBase, info.hdptxSize)
		|| !phy1.Names("clock-names", phyClockNames, 2)
		|| !phy1.Cells("rockchip,grf", &phyGrf, 1)
		|| !ReadSyscon(busModule, bus, phyGrf, "rockchip,rk3588-hdptxphy-grf",
			info.hdptxGrfBase, info.hdptxGrfSize)) {
		return false;
	}
	FdtNode gic;
	uint64_t gicSize;
	if (!gic.SetTo(gicNode) || !gic.HasString("compatible", "arm,gic-v3")
		|| !gic.Cells("#interrupt-cells", &cells, 1) || cells != 4
		|| !gic.Reg(0, info.interruptBase, gicSize) || gicSize != 0x10000) {
		return false;
	}
	strcpy(info.boardCompatible, "radxa,rock-5-itx");
	info.version = kResourceVersion;
	info.flags = kDescriptionValidated;
	if (!ResourcesMatch(info))
		return false;
	output = info;
	return true;
}


static float
SupportsDevice(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0;
	}
	ResourceInfo resources;
	return ReadResources(parent, resources) ? 0.8f : 0;
}


static status_t
RegisterDevice(device_node* parent)
{
	device_attr attributes[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "RK3588 display observation"}},
		{NULL}
	};
	return sDeviceManager->register_node(parent, DRIVER_NAME, attributes, NULL, NULL);
}


static status_t
InitDriver(device_node* node, void** cookie)
{
	Controller* controller = (Controller*)calloc(1, sizeof(Controller));
	if (controller == NULL)
		return B_NO_MEMORY;
	device_node* parent = sDeviceManager->get_parent_node(node);
	bool valid = ReadResources(parent, controller->resources);
	if (parent != NULL)
		sDeviceManager->put_node(parent);
	if (!valid) {
		free(controller);
		return B_NOT_SUPPORTED;
	}
	controller->node = node;
	void* settings = load_driver_settings("rk3588_display");
	if (settings != NULL) {
		const char* profile = get_driver_parameter(settings, "firmware_profile", "", "");
		controller->edidEnabled = strcmp(profile, "rock5-itx-edk2-v1.1-display-edid") == 0;
		controller->scanoutEnabled = strcmp(profile, "rock5-itx-edk2-v1.1-display-scanout") == 0;
		// The retrace profile names the stage that added the frame-start
		// interrupt; both run the accelerant with retrace.
		controller->cursorEnabled = strcmp(profile, "rock5-itx-edk2-v1.1-display-cursor") == 0;
		controller->modeSetEnabled = strcmp(profile, "rock5-itx-edk2-v1.1-display-modeset") == 0
			|| controller->cursorEnabled;
		controller->accelerantEnabled
			= strcmp(profile, "rock5-itx-edk2-v1.1-display-accelerant") == 0
			|| strcmp(profile, "rock5-itx-edk2-v1.1-display-retrace") == 0
			|| controller->modeSetEnabled;
		controller->scanoutEnabled = controller->scanoutEnabled || controller->accelerantEnabled;
		controller->edidEnabled = controller->edidEnabled || controller->scanoutEnabled;
	}
	if (settings != NULL)
		unload_driver_settings(settings);
	dprintf("rk3588_display: validated VOP2 %#" B_PRIx64 " and HDMI TX1 %#" B_PRIx64
		" resources; observation only; EDID %s; scanout %s; accelerant %s; modeset %s; cursor %s\n",
		controller->resources.vopBase, controller->resources.hdmiBase,
		controller->edidEnabled ? "enabled" : "disabled",
		controller->scanoutEnabled ? "enabled" : "disabled",
		controller->accelerantEnabled ? "enabled" : "disabled",
		controller->modeSetEnabled ? "enabled" : "disabled",
		controller->cursorEnabled ? "enabled" : "disabled");
	*cookie = controller;
	return B_OK;
}


static void
UninitDriver(void* cookie)
{
	MutexLocker locker(sHardwareLock);
	ReleaseFrameBuffer((Controller*)cookie);
	RestoreScanout((Controller*)cookie);
	ReleaseContiguous(sPattern);
	free(cookie);
}
static status_t InitDevice(void* driver, void** device) { *device = driver; return B_OK; }
static void UninitDevice(void*) {}
static status_t
PublishDevices(void* cookie)
{
	return sDeviceManager->publish_device(((Controller*)cookie)->node,
		"graphics/rk3588_display/0", DEVICE_NAME);
}


static status_t Read(void*, off_t, void*, size_t* length) { *length = 0; return B_NOT_ALLOWED; }
static status_t Write(void*, off_t, const void*, size_t* length) { *length = 0; return B_NOT_ALLOWED; }


static status_t
Control(void* cookie, uint32 op, void* buffer, size_t length)
{
	Handle* handle = (Handle*)cookie;
	Controller* controller = handle->controller;
	if (op == B_GET_ACCELERANT_SIGNATURE) {
		// Under the other profiles app_server must keep ignoring this device.
		if (!controller->accelerantEnabled)
			return B_DEV_INVALID_IOCTL;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		if (length < sizeof(kAccelerantSignature))
			return B_BAD_VALUE;
		return user_strlcpy((char*)buffer, kAccelerantSignature, length) < B_OK
			? B_BAD_ADDRESS : B_OK;
	}
	if (op == kGetDeviceName) {
		if (!controller->accelerantEnabled)
			return B_DEV_INVALID_IOCTL;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		if (length < sizeof(kDevicePath))
			return B_BAD_VALUE;
		return user_strlcpy((char*)buffer, kDevicePath, length) < B_OK ? B_BAD_ADDRESS : B_OK;
	}
	if (op == kAcquireFrameBuffer) {
		if (!controller->accelerantEnabled || !handle->writable)
			return B_NOT_ALLOWED;
		MutexLocker locker(sHardwareLock);
		if (sOwner != NULL)
			return B_BUSY;
		return AcquireFrameBuffer(handle);
	}
	if (op == kGetAccelerantInfo) {
		if (!controller->accelerantEnabled)
			return B_DEV_INVALID_IOCTL;
		if (length != sizeof(AccelerantInfo))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		AccelerantInfo info;
		if (user_memcpy(&info, buffer, sizeof(info)) != B_OK)
			return B_BAD_ADDRESS;
		if (info.version != kAccelerantVersion)
			return B_BAD_VALUE;
		MutexLocker locker(sHardwareLock);
		if (sOwner == NULL)
			return B_NO_INIT;
		sAccelerant.retraces = (uint32_t)atomic_get(&sRetraces);
		sAccelerant.interruptCalls = (uint32_t)atomic_get(&sInterruptCalls);
		sAccelerant.interruptSpurious = (uint32_t)atomic_get(&sInterruptSpurious);
		sAccelerant.firstRetraceMicros = sFirstRetraceMicros;
		sAccelerant.lastRetraceMicros = sLastRetraceMicros;
		return user_memcpy(buffer, &sAccelerant, sizeof(sAccelerant));
	}
	if (op == kSetDisplayMode) {
		if (!controller->modeSetEnabled)
			return B_DEV_INVALID_IOCTL;
		if (!handle->writable)
			return B_NOT_ALLOWED;
		if (length != sizeof(ModeRequest))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		ModeRequest request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != kModeVersion)
			return B_BAD_VALUE;
		MutexLocker locker(sHardwareLock);
		status_t status = ChangeDisplayMode(handle, request);
		if (status != B_OK)
			return status;
		return user_memcpy(buffer, &request, sizeof(request));
	}
	if (op == kSetPowerMode) {
		if (!controller->modeSetEnabled)
			return B_DEV_INVALID_IOCTL;
		if (!handle->writable)
			return B_NOT_ALLOWED;
		if (length != sizeof(PowerRequest))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		PowerRequest request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != kPowerVersion)
			return B_BAD_VALUE;
		MutexLocker locker(sHardwareLock);
		status_t status = ChangePowerMode(handle, request);
		if (status != B_OK)
			return status;
		return user_memcpy(buffer, &request, sizeof(request));
	}
	if (op == kRearmRetrace) {
		if (!controller->accelerantEnabled)
			return B_DEV_INVALID_IOCTL;
		if (!handle->writable)
			return B_NOT_ALLOWED;
		if (length != sizeof(RetraceRearm))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		RetraceRearm rearm;
		if (user_memcpy(&rearm, buffer, sizeof(rearm)) != B_OK)
			return B_BAD_ADDRESS;
		if (rearm.version != kAccelerantVersion)
			return B_BAD_VALUE;
		MutexLocker locker(sHardwareLock);
		if (sOwner == NULL || sVopRegisters == NULL || !sInterruptInstalled)
			return B_NO_INIT;
		memset(&rearm, 0, sizeof(rearm));
		rearm.version = kAccelerantVersion;
		uint32 base = kVopPortInterruptBase + sAccelerant.port * kVopPortInterruptStride;
		rearm.enableBefore = ReadDisplayRegister(sVopRegisters, base + kVopPortInterruptEnable);
		rearm.statusBefore = ReadDisplayRegister(sVopRegisters, base + kVopPortInterruptStatus);
		WriteDisplayRegister(sVopRegisters, base + kVopPortInterruptEnable,
			kVopInterruptFrameStart << 16);
		remove_io_interrupt_handler(controller->resources.vopInterrupt, RetraceInterrupt, NULL);
		sInterruptInstalled = false;
		rearm.reinstall = install_io_interrupt_handler(controller->resources.vopInterrupt,
			RetraceInterrupt, NULL, 0);
		sInterruptInstalled = rearm.reinstall == B_OK;
		WriteDisplayRegister(sVopRegisters, base + kVopPortInterruptClear,
			kVopInterruptMask << 16 | kVopInterruptMask);
		if (sInterruptInstalled) {
			WriteDisplayRegister(sVopRegisters, base + kVopPortInterruptEnable,
				kVopInterruptFrameStart << 16 | kVopInterruptFrameStart);
		}
		rearm.enableAfter = ReadDisplayRegister(sVopRegisters, base + kVopPortInterruptEnable);
		rearm.statusAfter = ReadDisplayRegister(sVopRegisters, base + kVopPortInterruptStatus);
		rearm.retraces = (uint32_t)atomic_get(&sRetraces);
		dprintf("rk3588_display: retrace re-armed enable=%#" B_PRIx32 "/%#" B_PRIx32 " status=%#"
			B_PRIx32 "/%#" B_PRIx32 " reinstall=%" B_PRId32 " retraces=%" B_PRIu32 " calls=%"
			B_PRId32 " spurious=%" B_PRId32 "\n", rearm.enableBefore, rearm.enableAfter,
			rearm.statusBefore, rearm.statusAfter, rearm.reinstall, rearm.retraces,
			atomic_get(&sInterruptCalls), atomic_get(&sInterruptSpurious));
		return user_memcpy(buffer, &rearm, sizeof(rearm));
	}
	if (op == kCloneFrameBuffer) {
		if (!controller->accelerantEnabled)
			return B_DEV_INVALID_IOCTL;
		if (length != sizeof(area_info))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		MutexLocker locker(sHardwareLock);
		if (sOwner == NULL)
			return B_NO_INIT;
		void* address = NULL;
		area_id area = vm_clone_area(B_CURRENT_TEAM, "RK3588 display frame buffer clone",
			&address, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0, sFrame.area, true);
		if (area < B_OK)
			return area;
		return _user_get_area_info(area, (area_info*)buffer);
	}
	if (op == kSetCursorBitmap || op == kMoveCursor || op == kShowCursor || op == kGetCursor)
		return CursorControl(handle, op, buffer, length);
	if (op == kGetSnapshot) {
		if (length != sizeof(DisplaySnapshot))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		MutexLocker locker(sHardwareLock);
		ObservationHardware hardware;
		DisplaySnapshot snapshot;
		status_t status = ObserveDisplay(hardware, controller->resources, snapshot);
		if (status != B_OK)
			return status;
		return user_memcpy(buffer, &snapshot, sizeof(snapshot));
	}
	if (op == kReadEdid) {
		if (length != sizeof(EdidRequest))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		EdidRequest request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != kEdidVersion || request.block >= kEdidMaxBlocks)
			return B_BAD_VALUE;
		if (!controller->edidEnabled)
			return B_NOT_ALLOWED;
		uint32_t block = request.block;
		memset(&request, 0, sizeof(request));
		request.version = kEdidVersion;
		request.block = block;
		MutexLocker locker(sHardwareLock);
		if (sPowerMode == kPowerOff) {
			// The HDMI TX I2C master is unreachable while the PHY is powered down.
			request.result = kEdidPoweredOff;
			return user_memcpy(buffer, &request, sizeof(request));
		}
		EdidHardware hardware;
		uint32_t result = kEdidNotReady;
		status_t status = hardware.Prepare(controller->resources, request.hotPlug, result);
		if (status != B_OK)
			return status;
		if (hardware.Ready())
			ReadEdidBlock(hardware, block, request);
		else
			request.result = result;
		dprintf("rk3588_display: EDID block %" B_PRIu32 " result=%" B_PRIu32 " bytes=%" B_PRIu32
			" polls=%" B_PRIu32 " flags=%#" B_PRIx32 "\n", block, request.result,
			request.bytesRead, request.polls, request.flags);
		return user_memcpy(buffer, &request, sizeof(request));
	}
	if (op == kSwapScanout) {
		if (length != sizeof(ScanoutRequest))
			return B_BAD_VALUE;
		if (buffer == NULL)
			return B_BAD_ADDRESS;
		ScanoutRequest request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != kScanoutVersion || request.action > kScanoutRestore)
			return B_BAD_VALUE;
		if (!controller->scanoutEnabled)
			return B_NOT_ALLOWED;
		uint32_t action = request.action;
		memset(&request, 0, sizeof(request));
		request.version = kScanoutVersion;
		request.action = action;
		MutexLocker locker(sHardwareLock);
		ScanoutHardware hardware;
		uint32_t result = kScanoutNotReady;
		// While the accelerant owns the window only queries are served.
		bool owned = sOwner != NULL;
		status_t status = hardware.Prepare(controller->resources,
			action != kScanoutQuery && !owned, result);
		if (status != B_OK)
			return status;
		request.startedMicros = hardware.Now();
		if (hardware.Ready())
			result = LocateScanoutWindow(hardware, request);
		if (result == kScanoutOK && owned && action != kScanoutQuery)
			result = kScanoutUnexpectedState;
		if (result == kScanoutOK && action == kScanoutShowPattern) {
			if (!sScanoutSwapped) {
				frame_buffer_boot_info* bootInfo
					= (frame_buffer_boot_info*)get_boot_item(FRAME_BUFFER_BOOT_INFO, NULL);
				if (bootInfo == NULL || bootInfo->physical_frame_buffer != request.addressBefore
					|| bootInfo->width != (int32)kPatternWidth
					|| bootInfo->height != (int32)kPatternHeight
					|| bootInfo->bytes_per_row != (int32)(kPatternWidth * 4)) {
					result = kScanoutUnexpectedState;
				} else if (AllocateContiguous(sPattern, "RK3588 display pattern", kPatternBytes,
						true) != B_OK) {
					result = kScanoutNoBuffer;
				} else {
					sFirmwareAddress = request.addressBefore;
					result = SwapScanoutAddress(hardware, request, sPattern.physical);
					sScanoutSwapped = true;
				}
			} else if (request.addressBefore != sPattern.physical) {
				result = kScanoutUnexpectedState;
			}
		} else if (result == kScanoutOK && action == kScanoutRestore) {
			if (!sScanoutSwapped)
				result = kScanoutNotSwapped;
			else {
				result = SwapScanoutAddress(hardware, request, sFirmwareAddress);
				// A failed read-back keeps the swap pending so Close retries.
				sScanoutSwapped = result != kScanoutOK;
			}
		}
		request.result = result;
		request.flags = sScanoutSwapped ? kScanoutSwapped : 0;
		request.firmwareAddress = sFirmwareAddress;
		request.patternAddress = sPattern.physical;
		request.finishedMicros = hardware.Now();
		dprintf("rk3588_display: scanout action=%" B_PRIu32 " result=%" B_PRIu32 " port=%" B_PRIu32
			" window=%" B_PRIu32 " before=%#" B_PRIx32 " after=%#" B_PRIx32 " polls=%" B_PRIu32
			" swapped=%u\n", action, result, request.port, request.window, request.addressBefore,
			request.addressAfter, request.polls, sScanoutSwapped ? 1 : 0);
		return user_memcpy(buffer, &request, sizeof(request));
	}
	if (op != kGetResources)
		return B_DEV_INVALID_IOCTL;
	if (length != sizeof(ResourceInfo))
		return B_BAD_VALUE;
	if (buffer == NULL)
		return B_BAD_ADDRESS;
	return user_memcpy(buffer, &controller->resources, sizeof(ResourceInfo));
}


static status_t
RestoreScanout(Controller* controller)
{
	if (!sScanoutSwapped)
		return B_OK;
	ScanoutHardware hardware;
	uint32_t result = kScanoutNotReady;
	status_t status = hardware.Prepare(controller->resources, true, result);
	if (status != B_OK)
		return status;
	if (!hardware.Ready())
		return B_BUSY;
	ScanoutRequest request = {};
	result = LocateScanoutWindow(hardware, request);
	if (result != kScanoutOK)
		return B_ERROR;
	result = SwapScanoutAddress(hardware, request, sFirmwareAddress);
	sScanoutSwapped = result != kScanoutOK;
	dprintf("rk3588_display: scanout restored to %#" B_PRIx32 " result=%" B_PRIu32 " polls=%"
		B_PRIu32 "\n", sFirmwareAddress, result, request.polls);
	return result == kScanoutOK ? B_OK : B_ERROR;
}


static int32
RetraceInterrupt(void* /*data*/)
{
	atomic_add(&sInterruptCalls, 1);
	uint32 base = kVopPortInterruptBase + sAccelerant.port * kVopPortInterruptStride;
	uint32 status = ReadDisplayRegister(sVopRegisters, base + kVopPortInterruptStatus)
		& kVopInterruptMask;
	if (status == 0) {
		atomic_add(&sInterruptSpurious, 1);
		return B_UNHANDLED_INTERRUPT;
	}
	WriteDisplayRegister(sVopRegisters, base + kVopPortInterruptClear, status << 16 | status);
	if ((status & kVopInterruptHoldValid) != 0)
		atomic_set(&sHoldValid, 1);
	if ((status & kVopInterruptFrameStart) == 0)
		return B_HANDLED_INTERRUPT;
	int32 count = atomic_add(&sRetraces, 1) + 1;
	sLastRetraceMicros = system_time();
	if (count == 1)
		sFirstRetraceMicros = sLastRetraceMicros;
	int32 waiting = 0;
	if (get_sem_count(sRetraceSemaphore, &waiting) == B_OK && waiting < 0) {
		release_sem_etc(sRetraceSemaphore, -waiting, B_DO_NOT_RESCHEDULE);
		return B_INVOKE_SCHEDULER;
	}
	return B_HANDLED_INTERRUPT;
}


// Everything the mode change touches: the persistent VOP2 mapping plus the
// PHY, HDMI TX, HDPTX GRF and CRU blocks mapped writable for its duration.
// The port's hold-valid report comes from the interrupt handler when it is
// installed and from the status word otherwise.
class ModeSetHardware {
public:
	status_t Prepare(const ResourceInfo& resources, uint32_t port)
	{
		fPort = port;
		void* address = NULL;
		fPhyArea.SetTo(map_physical_memory("RK3588 mode set PHY", resources.hdptxBase, kPhyMapSize,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			&address));
		if (fPhyArea.Get() < B_OK)
			return fPhyArea.Get();
		fPhy = (volatile uint32*)address;
		fHdmiArea.SetTo(map_physical_memory("RK3588 mode set HDMI TX", resources.hdmiBase,
			kHdmiTxMapSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address));
		if (fHdmiArea.Get() < B_OK)
			return fHdmiArea.Get();
		fHdmi = (volatile uint32*)address;
		fGrfArea.SetTo(map_physical_memory("RK3588 mode set HDPTX GRF", resources.hdptxGrfBase,
			B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address));
		if (fGrfArea.Get() < B_OK)
			return fGrfArea.Get();
		fGrf = (volatile uint32*)address;
		fCruArea.SetTo(map_physical_memory("RK3588 mode set CRU", resources.clockBase,
			resources.clockSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address));
		if (fCruArea.Get() < B_OK)
			return fCruArea.Get();
		fCru = (volatile uint32*)address;
		return B_OK;
	}
	uint32_t ReadVop(uint32_t offset) { return ReadDisplayRegister(sVopRegisters, offset); }
	void WriteVop(uint32_t offset, uint32_t value)
		{ WriteDisplayRegister(sVopRegisters, offset, value); }
	uint32_t ReadPhy(uint32_t offset) { return ReadDisplayRegister(fPhy, offset); }
	void WritePhy(uint32_t offset, uint32_t value) { WriteDisplayRegister(fPhy, offset, value); }
	uint32_t ReadHdmi(uint32_t offset) { return ReadDisplayRegister(fHdmi, offset); }
	void WriteHdmi(uint32_t offset, uint32_t value) { WriteDisplayRegister(fHdmi, offset, value); }
	void WriteHdptxGrf(uint32_t offset, uint32_t value) { WriteDisplayRegister(fGrf, offset, value); }
	uint32_t ReadHdptxGrfStatus() { return ReadDisplayRegister(fGrf, kPhyGrfStatus); }
	void WriteCru(uint32_t offset, uint32_t value) { WriteDisplayRegister(fCru, offset, value); }
	void Pause(unsigned micros) { spin(micros); }
	void ClearHoldValid() { atomic_set(&sHoldValid, 0); }
	bool HoldValid()
	{
		if (sInterruptInstalled)
			return atomic_get(&sHoldValid) != 0;
		uint32_t base = kVopPortInterruptBase + fPort * kVopPortInterruptStride;
		uint32_t status = ReadVop(base + kVopPortInterruptStatus) & kVopInterruptHoldValid;
		if (status == 0)
			return false;
		WriteVop(base + kVopPortInterruptClear, status << 16 | status);
		return true;
	}
private:
	AreaDeleter fPhyArea, fHdmiArea, fGrfArea, fCruArea;
	volatile uint32* fPhy = NULL;
	volatile uint32* fHdmi = NULL;
	volatile uint32* fGrf = NULL;
	volatile uint32* fCru = NULL;
	uint32_t fPort = 0;
};


// Adapter over the persistent mapping for the swap helpers.
struct MappedVop {
	uint32_t ReadVop(uint32_t offset) { return ReadDisplayRegister(sVopRegisters, offset); }
	void WriteVop(uint32_t offset, uint32_t value)
		{ WriteDisplayRegister(sVopRegisters, offset, value); }
	void Pause(unsigned micros) { spin(micros); }
};


static void
StopRetrace(uint32_t port, uint32_t interrupt)
{
	uint32 base = kVopPortInterruptBase + port * kVopPortInterruptStride;
	if (sInterruptInstalled) {
		// Only an enabled interrupt gets disabled: nothing else is written.
		WriteDisplayRegister(sVopRegisters, base + kVopPortInterruptEnable,
			kVopInterruptFrameStart << 16);
		remove_io_interrupt_handler(interrupt, RetraceInterrupt, NULL);
	}
	sInterruptInstalled = false;
	if (sRetraceSemaphore >= 0)
		delete_sem(sRetraceSemaphore);
	sRetraceSemaphore = -1;
}


// Frame-start interrupts on the live port drive the retrace semaphore. The
// three interrupt words of that port are the only registers touched.
static status_t
StartRetrace(uint32_t port, uint32_t interrupt)
{
	sRetraceSemaphore = create_sem(0, "RK3588 display retrace");
	if (sRetraceSemaphore < B_OK)
		return sRetraceSemaphore;
	// A kernel-owned semaphore cannot be acquired from userland (the kernel
	// logs "tried to acquire kernel semaphore"); the acquiring team, which is
	// app_server, owns it and it goes away with that team.
	status_t status = set_sem_owner(sRetraceSemaphore, team_get_current_team_id());
	if (status != B_OK) {
		delete_sem(sRetraceSemaphore);
		sRetraceSemaphore = -1;
		return status;
	}
	sRetraces = 0;
	sInterruptCalls = 0;
	sInterruptSpurious = 0;
	sFirstRetraceMicros = 0;
	sLastRetraceMicros = 0;
	status = install_io_interrupt_handler(interrupt, RetraceInterrupt, NULL, 0);
	if (status != B_OK) {
		StopRetrace(port, interrupt);
		return status;
	}
	sInterruptInstalled = true;
	uint32 base = kVopPortInterruptBase + port * kVopPortInterruptStride;
	WriteDisplayRegister(sVopRegisters, base + kVopPortInterruptClear,
		kVopInterruptMask << 16 | kVopInterruptMask);
	WriteDisplayRegister(sVopRegisters, base + kVopPortInterruptEnable,
		kVopInterruptFrameStart << 16 | kVopInterruptFrameStart);
	return B_OK;
}


// Acquires the frame buffer for the accelerant: the live window must still
// scan the firmware frame buffer, whose geometry the boot item describes.
// Only the two qualified VOP2 words are written for the swap; the port's
// interrupt words follow once the buffer is live.
static status_t
AcquireFrameBuffer(Handle* handle)
{
	Controller* controller = handle->controller;
	frame_buffer_boot_info* bootInfo
		= (frame_buffer_boot_info*)get_boot_item(FRAME_BUFFER_BOOT_INFO, NULL);
	if (bootInfo == NULL || bootInfo->width != (int32)kFrameWidth
		|| bootInfo->height != (int32)kFrameHeight
		|| bootInfo->bytes_per_row != (int32)kFrameBytesPerRow || bootInfo->depth != 32) {
		return B_NOT_SUPPORTED;
	}
	SharedInfo shared = {};
	shared.version = kAccelerantVersion;
	shared.modeListArea = -1;
	shared.width = kFrameWidth;
	shared.height = kFrameHeight;
	shared.bytesPerRow = kFrameBytesPerRow;
	strncpy(shared.name, "RK3588 VOP2 HDMI TX1", sizeof(shared.name) - 1);
	{
		// The sink's EDID base block, when the port is powered and connected.
		EdidHardware edid;
		EdidRequest request = {};
		request.version = kEdidVersion;
		uint32_t result = kEdidNotReady;
		status_t status = edid.Prepare(controller->resources, request.hotPlug, result);
		if (status != B_OK)
			return status;
		if (edid.Ready())
			ReadEdidBlock(edid, 0, request);
		else
			request.result = result;
		shared.edidResult = request.result;
		if (request.result == kEdidOK) {
			memcpy(shared.edid, request.data, sizeof(shared.edid));
			shared.flags |= kAccelerantEdid;
		}
	}
	ScanoutHardware hardware;
	uint32_t result = kScanoutNotReady;
	status_t status = hardware.Prepare(controller->resources, true, result);
	if (status != B_OK)
		return status;
	if (!hardware.Ready())
		return B_BUSY;
	ScanoutRequest request = {};
	result = LocateScanoutWindow(hardware, request);
	if (result != kScanoutOK || request.addressBefore != bootInfo->physical_frame_buffer)
		return B_NOT_SUPPORTED;
	for (unsigned i = 0; i < 4; i++) {
		shared.portTiming[i] = hardware.ReadVop(kVopPortBase + request.port * kVopPortStride
			+ kVopPortOffsets[kVopPortHTotal + i]);
	}
	if (!DecodePortTiming(shared.portTiming, kFrameWidth, kFrameHeight, shared))
		return B_NOT_SUPPORTED;
	// The firmware runs the port at 60 Hz; the pixel clock follows the totals.
	shared.pixelClockKHz = shared.hTotal * shared.vTotal * 60 / 1000;
	shared.powerMode = kPowerOn;
	shared.syncFlags = kModePositiveHSync | kModePositiveVSync; // CEA 1080p60
	memset(&sFirmwareMode, 0, sizeof(sFirmwareMode));
	sFirmwareMode.version = kModeVersion;
	sFirmwareMode.flags = kModePositiveHSync | kModePositiveVSync;
	sFirmwareMode.pixelClockKHz = shared.pixelClockKHz;
	sFirmwareMode.hDisplay = shared.width;
	sFirmwareMode.hSyncStart = shared.hSyncStart;
	sFirmwareMode.hSyncEnd = shared.hSyncEnd;
	sFirmwareMode.hTotal = shared.hTotal;
	sFirmwareMode.vDisplay = shared.height;
	sFirmwareMode.vSyncStart = shared.vSyncStart;
	sFirmwareMode.vSyncEnd = shared.vSyncEnd;
	sFirmwareMode.vTotal = shared.vTotal;
	sFirmwareMode.vic = CeaVideoCode(shared.width, shared.height, shared.pixelClockKHz);
	memset(&sCurrentMode, 0, sizeof(sCurrentMode));
	sPowerMode = kPowerOn;
	status = AllocateContiguous(sFrame, "RK3588 display frame buffer", kFrameBytes, false);
	if (status != B_OK)
		return status;
	sSharedArea = create_area("RK3588 display shared", (void**)&sShared, B_ANY_KERNEL_ADDRESS,
		B_PAGE_SIZE, B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (sSharedArea < B_OK) {
		ReleaseContiguous(sFrame);
		sShared = NULL;
		return sSharedArea;
	}
	*sShared = shared;
	result = SwapScanoutAddress(hardware, request, sFrame.physical);
	if (result != kScanoutOK) {
		ScanoutRequest restore = request;
		uint32_t restored = SwapScanoutAddress(hardware, restore, request.addressBefore);
		dprintf("rk3588_display: frame buffer swap result=%" B_PRIu32 " after=%#" B_PRIx32
			"; firmware restore result=%" B_PRIu32 "\n", result, request.addressAfter, restored);
		delete_area(sSharedArea);
		sSharedArea = -1;
		sShared = NULL;
		ReleaseContiguous(sFrame);
		return B_ERROR;
	}
	frame_buffer_update((addr_t)sFrame.address, kFrameWidth, kFrameHeight, 32, kFrameBytesPerRow);
	memset(&sAccelerant, 0, sizeof(sAccelerant));
	sAccelerant.version = kAccelerantVersion;
	sAccelerant.flags = kAccelerantAcquired | (shared.flags & kAccelerantEdid)
		| (controller->modeSetEnabled ? kAccelerantModeSet : 0);
	memset(&sCursorState, 0, sizeof(sCursorState));
	sCursorState.version = kCursorVersion;
	sCursorState.window = kVopCursorWindow;
	sCursorState.mixer = kVopCursorMixer;
	sCursorProgrammed = false;
	if (controller->cursorEnabled && AllocateContiguous(sCursor, "RK3588 display cursor",
			kCursorBufferBytes, false) == B_OK) {
		sAccelerant.flags |= kAccelerantCursor;
	}
	sAccelerant.retraceSemaphore = -1;
	sAccelerant.port = request.port;
	// Keep VOP2 mapped for the interrupt handler and the release path; the
	// swap's own mapping goes first so only one writable mapping exists.
	hardware.ReleaseVop();
	void* address = NULL;
	sVopArea = map_physical_memory("RK3588 display VOP2", controller->resources.vopBase,
		kVopMapSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (sVopArea >= B_OK) {
		sVopRegisters = (volatile uint32*)address;
		if (StartRetrace(request.port, controller->resources.vopInterrupt) == B_OK) {
			sAccelerant.flags |= kAccelerantRetrace;
			sAccelerant.retraceSemaphore = sRetraceSemaphore;
		}
	}
	sAccelerant.sharedArea = sSharedArea;
	sAccelerant.frameBufferPhysical = sFrame.physical;
	sAccelerant.firmwareAddress = request.addressBefore;
	sAccelerant.port = request.port;
	sAccelerant.window = request.window;
	sAccelerant.polls = request.polls;
	sAccelerant.width = kFrameWidth;
	sAccelerant.height = kFrameHeight;
	sAccelerant.bytesPerRow = kFrameBytesPerRow;
	sOwner = handle;
	dprintf("rk3588_display: frame buffer acquired at %#" B_PRIx32 " (firmware %#" B_PRIx32
		") port=%" B_PRIu32 " window=%" B_PRIu32 " polls=%" B_PRIu32 " edid=%" B_PRIu32
		" timing=%" B_PRIu32 "x%" B_PRIu32 " %" B_PRIu32 "/%" B_PRIu32 " %" B_PRIu32
		"/%" B_PRIu32 " %" B_PRIu32 " kHz retrace=%s irq=%" B_PRIu32 "\n", sFrame.physical,
		request.addressBefore, request.port, request.window, request.polls, shared.edidResult,
		shared.hTotal, shared.vTotal, shared.hSyncStart, shared.hSyncEnd, shared.vSyncStart,
		shared.vSyncEnd, shared.pixelClockKHz,
		(sAccelerant.flags & kAccelerantRetrace) != 0 ? "on" : "off",
		controller->resources.vopInterrupt);
	return B_OK;
}


// Returns the scanout and the kernel console to the firmware frame buffer
// and detaches every clone before the buffers go away.
static void
ReleaseFrameBuffer(Controller* controller)
{
	if (sOwner == NULL)
		return;
	uint32_t result = kScanoutNotReady;
	ScanoutRequest request = {};
	if (sVopRegisters != NULL && (sPowerMode == kPowerOff || (sCurrentMode.version != 0
			&& (sCurrentMode.hDisplay != sFirmwareMode.hDisplay
				|| sCurrentMode.vDisplay != sFirmwareMode.vDisplay
				|| sCurrentMode.pixelClockKHz != sFirmwareMode.pixelClockKHz)))) {
		// The firmware frame buffer wants the firmware's mode, powered on.
		ModeSetHardware hardware;
		if (hardware.Prepare(controller->resources, sAccelerant.port) == B_OK) {
			ModeRequest restore = sFirmwareMode;
			uint32_t restored = SetDisplayMode(hardware, sAccelerant.port, sAccelerant.window,
				restore);
			dprintf("rk3588_display: firmware mode restore result=%" B_PRIu32 " phase=%"
				B_PRIu32 " power=%" B_PRIu32 "\n", restored, restore.phase, sPowerMode);
		}
	}
	sPowerMode = kPowerOn;
	memset(&sCurrentMode, 0, sizeof(sCurrentMode));
	if (sCursorProgrammed && sVopRegisters != NULL) {
		// The firmware frame buffer gets no cursor window over it.
		sCursorState.visible = 0;
		uint32_t polls = 0;
		uint32_t hidden = ProgramCursor(polls);
		dprintf("rk3588_display: cursor window disabled result=%" B_PRIu32 " polls=%" B_PRIu32
			"\n", hidden, polls);
	}
	sCursorProgrammed = false;
	ReleaseContiguous(sCursor);
	StopRetrace(sAccelerant.port, controller->resources.vopInterrupt);
	if (sVopRegisters != NULL) {
		// The persistent mapping serves the restore; the domain stayed on.
		MappedVop hardware;
		result = LocateScanoutWindow(hardware, request);
		if (result == kScanoutOK)
			result = SwapScanoutAddress(hardware, request, sAccelerant.firmwareAddress);
		sVopRegisters = NULL;
		delete_area(sVopArea);
		sVopArea = -1;
	} else {
		ScanoutHardware hardware;
		if (hardware.Prepare(controller->resources, true, result) == B_OK && hardware.Ready()) {
			result = LocateScanoutWindow(hardware, request);
			if (result == kScanoutOK)
				result = SwapScanoutAddress(hardware, request, sAccelerant.firmwareAddress);
		}
	}
	frame_buffer_boot_info* bootInfo
		= (frame_buffer_boot_info*)get_boot_item(FRAME_BUFFER_BOOT_INFO, NULL);
	if (bootInfo != NULL) {
		frame_buffer_update(bootInfo->frame_buffer, bootInfo->width, bootInfo->height,
			bootInfo->depth, bootInfo->bytes_per_row);
	}
	vm_change_clones_to_null_areas(sFrame.area);
	ReleaseContiguous(sFrame);
	delete_area(sSharedArea);
	sSharedArea = -1;
	sShared = NULL;
	sOwner = NULL;
	dprintf("rk3588_display: frame buffer released; firmware %#" B_PRIx32 " restore result=%"
		B_PRIu32 " polls=%" B_PRIu32 " retraces=%" B_PRId32 " calls=%" B_PRId32 " spurious=%"
		B_PRId32 " first=%" B_PRId64 " last=%" B_PRId64 "\n", sAccelerant.firmwareAddress,
		result, request.polls, sRetraces, sInterruptCalls, sInterruptSpurious,
		sFirstRetraceMicros, sLastRetraceMicros);
	memset(&sAccelerant, 0, sizeof(sAccelerant));
}


// Changes the mode of the acquired frame buffer's port. The shared
// information follows so the accelerant reports the new mode.
static status_t
ChangeDisplayMode(Handle* handle, ModeRequest& request)
{
	uint32_t action[12];
	memcpy(action, &request, sizeof(action)); // version, flags, clock, timing, vic
	memset(&request, 0, sizeof(request));
	memcpy(&request, action, sizeof(action));
	if (sOwner == NULL || sVopRegisters == NULL || sShared == NULL) {
		request.result = kModeNotAcquired;
		return B_OK;
	}
	ModeSetHardware hardware;
	status_t status = hardware.Prepare(handle->controller->resources, sAccelerant.port);
	if (status != B_OK)
		return status;
	request.startedMicros = system_time();
	request.result = SetDisplayMode(hardware, sAccelerant.port, sAccelerant.window, request);
	request.finishedMicros = system_time();
	if (request.result == kModeOK) {
		sShared->width = request.hDisplay;
		sShared->height = request.vDisplay;
		sShared->pixelClockKHz = request.pixelClockKHz;
		sShared->hSyncStart = request.hSyncStart;
		sShared->hSyncEnd = request.hSyncEnd;
		sShared->hTotal = request.hTotal;
		sShared->vSyncStart = request.vSyncStart;
		sShared->vSyncEnd = request.vSyncEnd;
		sShared->vTotal = request.vTotal;
		memcpy(sShared->portTiming, request.timing, sizeof(sShared->portTiming));
		sShared->syncFlags = request.flags;
		sAccelerant.width = request.hDisplay;
		sAccelerant.height = request.vDisplay;
		sCurrentMode = request;
		sPowerMode = kPowerOn; // the mode set powers everything on
		sShared->powerMode = kPowerOn;
		if (sCursorProgrammed) {
			uint32_t polls = 0;
			ProgramCursor(polls); // clipped to the new frame
		}
		frame_buffer_update((addr_t)sFrame.address, request.hDisplay, request.vDisplay, 32,
			kFrameBytesPerRow);
	}
	dprintf("rk3588_display: mode %" B_PRIu32 "x%" B_PRIu32 " %" B_PRIu32 " kHz vic=%" B_PRIu32
		" result=%" B_PRIu32 " phase=%" B_PRIu32 " hold=%" B_PRIu32 " clock=%" B_PRIu32 " lock=%"
		B_PRIu32 " status=%#" B_PRIx32 " timing=%08" B_PRIx32 ",%08" B_PRIx32 ",%08" B_PRIx32
		",%08" B_PRIx32 " micros=%" B_PRId64 "\n", request.hDisplay, request.vDisplay,
		request.pixelClockKHz, request.vic, request.result, request.phase, request.holdPolls,
		request.clockPolls, request.lockPolls, request.phyStatus, request.timing[0],
		request.timing[1], request.timing[2], request.timing[3],
		request.finishedMicros - request.startedMicros);
	return B_OK;
}


// DPMS: off stops the port and powers the PHY down; on repeats the mode set
// of the current mode (the firmware's, if the driver never changed it).
static status_t
ChangePowerMode(Handle* handle, PowerRequest& request)
{
	uint32_t version = request.version, mode = request.mode;
	memset(&request, 0, sizeof(request));
	request.version = version;
	request.mode = mode;
	request.previous = sPowerMode;
	if (sOwner == NULL || sVopRegisters == NULL || sShared == NULL) {
		request.result = kModeNotAcquired;
		return B_OK;
	}
	if (mode != kPowerOn && mode != kPowerOff) {
		request.result = kModeUnsupported;
		return B_OK;
	}
	if (mode == sPowerMode) {
		// app_server asks for DPMS on at every start; nothing is touched.
		request.result = kModeOK;
		dprintf("rk3588_display: power %s result=0 phase=0 already\n",
			mode == kPowerOff ? "off" : "on");
		return B_OK;
	}
	ModeSetHardware hardware;
	status_t status = hardware.Prepare(handle->controller->resources, sAccelerant.port);
	if (status != B_OK)
		return status;
	request.startedMicros = system_time();
	if (mode == kPowerOff) {
		request.result = PowerOff(hardware, sAccelerant.port, request);
	} else {
		ModeRequest restore = sCurrentMode.version != 0 ? sCurrentMode : sFirmwareMode;
		uint32_t action[12];
		memcpy(action, &restore, sizeof(action)); // inputs only
		memset(&restore, 0, sizeof(restore));
		memcpy(&restore, action, sizeof(action));
		request.result = SetDisplayMode(hardware, sAccelerant.port, sAccelerant.window, restore);
		request.phase = restore.phase;
		request.holdPolls = restore.holdPolls;
		request.clockPolls = restore.clockPolls;
		request.lockPolls = restore.lockPolls;
	}
	request.finishedMicros = system_time();
	request.phyStatus = hardware.ReadHdptxGrfStatus();
	request.portControl = hardware.ReadVop(kVopPortBase + sAccelerant.port * kVopPortStride
		+ kVopPortControlWord);
	if (request.result == kModeOK) {
		sPowerMode = mode;
		sShared->powerMode = mode;
	}
	dprintf("rk3588_display: power %s result=%" B_PRIu32 " phase=%" B_PRIu32 " hold=%" B_PRIu32
		" clock=%" B_PRIu32 " lock=%" B_PRIu32 " status=%#" B_PRIx32 " control=%#" B_PRIx32
		" previous=%" B_PRIu32 " micros=%" B_PRId64 "\n", mode == kPowerOff ? "off" : "on",
		request.result, request.phase, request.holdPolls, request.clockPolls, request.lockPolls,
		request.phyStatus, request.portControl, request.previous,
		request.finishedMicros - request.startedMicros);
	return B_OK;
}


// Writes the cursor window for the current state over the persistent
// mapping; the acquiring team's frame size clips it.
static uint32_t
ProgramCursor(uint32_t& polls)
{
	polls = 0;
	if (sOwner == NULL || sVopRegisters == NULL || sCursor.area < 0)
		return kCursorNotAcquired;
	MappedVop hardware;
	uint32_t result = ApplyCursor(hardware, sCursorState, sCursor.physical, sAccelerant.port,
		sAccelerant.width, sAccelerant.height, polls);
	sCursorProgrammed = true;
	return result;
}


// Whether the window needs programming for a change of position or state:
// while the pointer shows, or while a failed hide left the window enabled.
static bool
CursorWindowLive()
{
	return sCursorState.visible != 0
		|| (sCursorState.regionControl & kVopEsmartRegionEnable) != 0;
}


static status_t
CursorControl(Handle* handle, uint32 op, void* buffer, size_t length)
{
	Controller* controller = handle->controller;
	if (!controller->cursorEnabled)
		return B_DEV_INVALID_IOCTL;
	if (buffer == NULL)
		return B_BAD_ADDRESS;
	if (op == kGetCursor) {
		// The state keeps a copy of the bitmap rows, so nothing of its 16 KB
		// ever sits on the 16 KB kernel stack (the +291 image panicked in
		// app_server's first ioctl with a copy here).
		if (length != sizeof(CursorState))
			return B_BAD_VALUE;
		MutexLocker locker(sHardwareLock);
		return user_memcpy(buffer, &sCursorState, sizeof(sCursorState));
	}
	if (!handle->writable)
		return B_NOT_ALLOWED;
	if (op == kSetCursorBitmap) {
		if (length != sizeof(CursorBitmap))
			return B_BAD_VALUE;
		CursorBitmap* bitmap = new(std::nothrow) CursorBitmap;
		if (bitmap == NULL)
			return B_NO_MEMORY;
		if (user_memcpy(bitmap, buffer, sizeof(*bitmap)) != B_OK) {
			delete bitmap;
			return B_BAD_ADDRESS;
		}
		if (bitmap->version != kCursorVersion) {
			delete bitmap;
			return B_BAD_VALUE;
		}
		MutexLocker locker(sHardwareLock);
		bitmap->polls = 0;
		if (sOwner == NULL || sCursor.address == NULL) {
			bitmap->result = kCursorNotAcquired;
		} else if (bitmap->width == 0 || bitmap->height == 0 || bitmap->width > kCursorMaxSize
			|| bitmap->height > kCursorMaxSize || bitmap->hotX >= bitmap->width
			|| bitmap->hotY >= bitmap->height || bitmap->bytesPerRow < bitmap->width * 4
			|| bitmap->bytesPerRow > kCursorBytesPerRow) {
			bitmap->result = kCursorUnsupported;
		} else {
			uint8_t* pixels = (uint8_t*)sCursor.address;
			memset(pixels, 0, kCursorBufferBytes);
			for (uint32 row = 0; row < bitmap->height; row++) {
				memcpy(pixels + row * kCursorBytesPerRow, bitmap->data + row * bitmap->bytesPerRow,
					bitmap->width * 4);
			}
			memcpy(sCursorState.data, pixels, kCursorBufferBytes);
			sCursorState.width = bitmap->width;
			sCursorState.height = bitmap->height;
			sCursorState.hotX = bitmap->hotX;
			sCursorState.hotY = bitmap->hotY;
			bitmap->result = CursorWindowLive() ? ProgramCursor(bitmap->polls) : kCursorOK;
			dprintf("rk3588_display: cursor bitmap %" B_PRIu32 "x%" B_PRIu32 " hot=%" B_PRIu32
				",%" B_PRIu32 " result=%" B_PRIu32 " polls=%" B_PRIu32 "\n", bitmap->width,
				bitmap->height, bitmap->hotX, bitmap->hotY, bitmap->result, bitmap->polls);
		}
		status_t status = user_memcpy(buffer, bitmap, offsetof(CursorBitmap, data));
		delete bitmap;
		return status;
	}
	if (op == kMoveCursor) {
		if (length != sizeof(CursorMove))
			return B_BAD_VALUE;
		CursorMove move;
		if (user_memcpy(&move, buffer, sizeof(move)) != B_OK)
			return B_BAD_ADDRESS;
		if (move.version != kCursorVersion)
			return B_BAD_VALUE;
		MutexLocker locker(sHardwareLock);
		move.polls = 0;
		if (sOwner == NULL || sCursor.address == NULL) {
			move.result = kCursorNotAcquired;
		} else if (move.x < -32768 || move.x > 32767 || move.y < -32768 || move.y > 32767) {
			move.result = kCursorUnsupported;
		} else {
			sCursorState.x = move.x;
			sCursorState.y = move.y;
			// A hidden cursor only remembers its position.
			move.result = CursorWindowLive() ? ProgramCursor(move.polls) : kCursorOK;
		}
		move.displayStart = sCursorState.displayStart;
		move.address = sCursorState.address;
		return user_memcpy(buffer, &move, sizeof(move));
	}
	if (op == kShowCursor) {
		if (length != sizeof(CursorShow))
			return B_BAD_VALUE;
		CursorShow show;
		if (user_memcpy(&show, buffer, sizeof(show)) != B_OK)
			return B_BAD_ADDRESS;
		if (show.version != kCursorVersion)
			return B_BAD_VALUE;
		MutexLocker locker(sHardwareLock);
		show.polls = 0;
		if (sOwner == NULL || sCursor.address == NULL) {
			show.result = kCursorNotAcquired;
		} else {
			bool wasVisible = sCursorState.visible != 0;
			sCursorState.visible = show.visible != 0 ? 1 : 0;
			show.result = wasVisible || CursorWindowLive() ? ProgramCursor(show.polls) : kCursorOK;
			dprintf("rk3588_display: cursor %s result=%" B_PRIu32 " polls=%" B_PRIu32
				" control=%#" B_PRIx32 " start=%#" B_PRIx32 "\n",
				show.visible != 0 ? "shown" : "hidden", show.result, show.polls,
				sCursorState.regionControl, sCursorState.displayStart);
		}
		show.regionControl = sCursorState.regionControl;
		return user_memcpy(buffer, &show, sizeof(show));
	}
	return B_DEV_INVALID_IOCTL;
}


static status_t
Open(void* cookie, const char*, int mode, void** _handle)
{
	Controller* controller = (Controller*)cookie;
	bool writable = (mode & O_ACCMODE) != O_RDONLY;
	if (writable && !controller->accelerantEnabled)
		return B_NOT_ALLOWED;
	Handle* handle = (Handle*)malloc(sizeof(Handle));
	if (handle == NULL)
		return B_NO_MEMORY;
	handle->controller = controller;
	handle->writable = writable;
	*_handle = handle;
	return B_OK;
}


static status_t
Close(void* cookie)
{
	// A client that swapped the scanout and went away must not leave the
	// desktop on the pattern buffer; the accelerant's handle gives the
	// firmware frame buffer back.
	Handle* handle = (Handle*)cookie;
	MutexLocker locker(sHardwareLock);
	if (sOwner == handle)
		ReleaseFrameBuffer(handle->controller);
	RestoreScanout(handle->controller);
	return B_OK;
}


static status_t
Free(void* cookie)
{
	free(cookie);
	return B_OK;
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};

static device_module_info sDevice = {
	{DEVICE_NAME, 0, NULL},
	InitDevice, UninitDevice, NULL,
	Open, Close, Free, Read, Write, NULL, Control, NULL, NULL
};

static driver_module_info sDriver = {
	{DRIVER_NAME, 0, NULL},
	SupportsDevice, RegisterDevice, InitDriver, UninitDriver, PublishDevices,
	NULL, NULL, NULL, NULL
};

module_info* modules[] = {
	(module_info*)&sDriver,
	(module_info*)&sDevice,
	NULL
};
