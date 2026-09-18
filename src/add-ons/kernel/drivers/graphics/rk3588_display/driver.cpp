/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <bus/FDT.h>
#include <KernelExport.h>
#include <AutoDeleterOS.h>
#include <driver_settings.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "DisplayEdid.h"


using namespace RK3588Display;

#define DRIVER_NAME "drivers/graphics/rk3588_display/driver_v1"
#define DEVICE_NAME "drivers/graphics/rk3588_display/device_v1"

static device_manager_info* sDeviceManager;
static mutex sHardwareLock = MUTEX_INITIALIZER("RK3588 display platform");

struct Controller {
	device_node* node;
	ResourceInfo resources;
	bool edidEnabled;
};


#include "DisplayHardware.h"


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
		unload_driver_settings(settings);
	}
	dprintf("rk3588_display: validated VOP2 %#" B_PRIx64 " and HDMI TX1 %#" B_PRIx64
		" resources; observation only; EDID %s\n", controller->resources.vopBase,
		controller->resources.hdmiBase, controller->edidEnabled ? "enabled" : "disabled");
	*cookie = controller;
	return B_OK;
}


static void UninitDriver(void* cookie) { free(cookie); }
static status_t InitDevice(void* driver, void** device) { *device = driver; return B_OK; }
static void UninitDevice(void*) {}
static status_t Close(void*) { return B_OK; }
static status_t Free(void*) { return B_OK; }


static status_t
PublishDevices(void* cookie)
{
	return sDeviceManager->publish_device(((Controller*)cookie)->node,
		"graphics/rk3588_display/0", DEVICE_NAME);
}


static status_t
Open(void* cookie, const char*, int mode, void** handle)
{
	if ((mode & O_ACCMODE) != O_RDONLY)
		return B_NOT_ALLOWED;
	*handle = cookie;
	return B_OK;
}


static status_t Read(void*, off_t, void*, size_t* length) { *length = 0; return B_NOT_ALLOWED; }
static status_t Write(void*, off_t, const void*, size_t* length) { *length = 0; return B_NOT_ALLOWED; }


static status_t
Control(void* cookie, uint32 op, void* buffer, size_t length)
{
	Controller* controller = (Controller*)cookie;
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
	if (op != kGetResources)
		return B_DEV_INVALID_IOCTL;
	if (length != sizeof(ResourceInfo))
		return B_BAD_VALUE;
	if (buffer == NULL)
		return B_BAD_ADDRESS;
	return user_memcpy(buffer, &controller->resources, sizeof(ResourceInfo));
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
