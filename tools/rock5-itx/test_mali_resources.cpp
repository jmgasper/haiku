#include "CsfReset.h"
#include "CsfRun.h"
#include "CsfCommands.h"
#include "CsfQueue.h"

#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>
#include <string>
#include <vector>

using namespace MaliCSF;
using int32 = int32_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using status_t = int32_t;
static const status_t B_OK = 0, B_BAD_VALUE = -1, B_BAD_ADDRESS = -2,
	B_DEV_INVALID_IOCTL = -3, B_NO_MEMORY = -4, B_NOT_SUPPORTED = -5,
	B_NOT_ALLOWED = -6, B_BUSY = -7, B_ENTRY_NOT_FOUND = -8;
static const unsigned B_PAGE_SIZE = 4096, B_ANY_KERNEL_ADDRESS = 4,
	B_UNCACHED_MEMORY = 1u << 28, B_KERNEL_READ_AREA = 1u << 4,
	B_KERNEL_WRITE_AREA = 1u << 5;

static std::map<int, std::pair<void*, size_t>> sAreas;
static unsigned sMapAttempts, sFailMap;
static int64_t sTime;
static bool sAllowWritable, sAllowGpu, sAllowResetGpu;
static unsigned sLockDepth;

struct mutex {};
#define MUTEX_INITIALIZER(name) {}
class MutexLocker {
public:
	explicit MutexLocker(mutex&) { assert(sLockDepth++ == 0); }
	~MutexLocker() { assert(--sLockDepth == 0); }
};

static int64_t system_time() { return ++sTime; }
static void memory_read_barrier() {}
static void memory_write_barrier() { assert(sAllowResetGpu); }
static void spin(int) { assert(false); }
static void kernel_dprintf(const char*, ...) {}
#define dprintf kernel_dprintf

static int
map_physical_memory(const char*, uint64 base, size_t bytes, uint32 spec,
	uint32 protection, void** address)
{
	assert(sLockDepth == 1);
	assert(base == 0xfd7c0000 || base == 0xfd8d8000 || (sAllowGpu && base == 0xfb000000));
	assert(bytes == (sAllowResetGpu && base == 0xfb000000 ? 3 : 1) * B_PAGE_SIZE
		&& spec == (B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY));
	assert(protection == (B_KERNEL_READ_AREA
		| (sAllowWritable && (base != 0xfb000000 || sAllowResetGpu) ? B_KERNEL_WRITE_AREA : 0)));
	if (++sMapAttempts == sFailMap)
		return B_NO_MEMORY;
	void* allocation = mmap(NULL, bytes + 2 * B_PAGE_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(allocation != MAP_FAILED);
	uint32* registers = (uint32*)((char*)allocation + B_PAGE_SIZE);
	assert(mprotect(registers, bytes, PROT_READ | PROT_WRITE) == 0);
	for (unsigned offset = 0; offset < bytes; offset += 4)
		registers[offset / 4] = (base == 0xfd7c0000 ? 0x12340000 : 0xabcd0000) | offset;
	if (base == 0xfb000000) registers[0] = 0xa8670005;
	// A production register write fails immediately, as would either guard page.
	assert(mprotect(registers, bytes, sAllowResetGpu && base == 0xfb000000
		? PROT_READ | PROT_WRITE : PROT_READ) == 0);
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

static const int B_UNHANDLED_INTERRUPT = 0, B_HANDLED_INTERRUPT = 1;
using interrupt_handler = int32 (*)(void*);
static std::mutex sVectorLock;
static interrupt_handler sHandler;
static void* sHandlerCookie;
static bool sFailInstall;
static unsigned sInstalled, sRemoved;
static std::atomic<bool> sPauseInCpu(false), sInsideCpu(false), sReleaseCpu(false);
static std::atomic<unsigned> sSpinAttempts(0);
static thread_local unsigned sSpinDepth;
struct spinlock { std::mutex lock; };
#define B_SPINLOCK_INITIALIZER {}
class InterruptsSpinLocker {
public:
	explicit InterruptsSpinLocker(spinlock& lock) : fLock(lock)
	{
		sSpinAttempts++;
		fLock.lock.lock();
		assert(sSpinDepth++ == 0);
	}
	~InterruptsSpinLocker() { assert(--sSpinDepth == 0); fLock.lock.unlock(); }
private:
	spinlock& fLock;
};
static int32 smp_get_current_cpu()
{
	assert(sSpinDepth == 1);
	if (sPauseInCpu.load()) {
		sInsideCpu = true;
		while (!sReleaseCpu.load()) std::this_thread::yield();
	}
	return 0;
}
static void snooze(int) { assert(false); }
static status_t install_io_interrupt_handler(int32 irq, interrupt_handler handler, void* cookie, uint32 flags)
{
	assert(irq == 126 && flags == 0 && sAreas.size() == 3 && sSpinDepth == 0 && !sHandler);
	if (sFailInstall) return B_NO_MEMORY;
	std::lock_guard<std::mutex> vector(sVectorLock);
	sHandler = handler;
	sHandlerCookie = cookie;
	sInstalled++;
	return B_OK;
}
static status_t remove_io_interrupt_handler(int32 irq, interrupt_handler handler, void* cookie)
{
	assert(irq == 126 && sAreas.size() == 3 && sSpinDepth == 0);
	std::lock_guard<std::mutex> vector(sVectorLock);
	assert(sHandler == handler && sHandlerCookie == cookie);
	sHandler = NULL;
	sHandlerCookie = NULL;
	sRemoved++;
	return B_OK;
}
static int32 InvokeInterrupt()
{
	std::lock_guard<std::mutex> vector(sVectorLock);
	assert(sHandler);
	return sHandler(sHandlerCookie);
}

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
	uint64 base = 0, size = 0;
	bool wrongModule = false;
	int held = 0;
};

static device_node sBusNode, sRoot, sGpu, sClock, sPower, sPmu, sRegulator, sGic;
static device_node sFixed, sDomain;
static fdt_bus sBus;
static std::map<int, device_node*> sPhandles;
static uint64 sDecodedIrqs[3] = {124, 125, 126};
static bool sWrongIrqController;

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
	assert(index == 0);
	*base = dev->node->base;
	*size = dev->node->size;
	return *size != 0;
}

static bool
GetInterrupt(fdt_device* dev, uint32 index, device_node** node, uint64* irq)
{
	assert(dev->node == &sGpu && index < 3);
	*node = sWrongIrqController && index == 2 ? &sClock : &sGic;
	*irq = sDecodedIrqs[index];
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
		for (device_node* node : {&sFixed, &sDomain}) {
			if (node->name != attributes[0].value.string) continue;
			// The real lookup searches descendants. Profile admission must also
			// verify the immediate parent of the returned node.
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

static bool sFirmwareRetained;
static bool FirmwareMemoryRetained() { return sFirmwareRetained; }
static unsigned sFirmwareRequests;
static status_t RunFirmwareRequest(const ResourceInfo&, void*, size_t, bool& recovery)
{
	assert(sLockDepth == 1);
	sFirmwareRequests++;
	recovery = true;
	return B_OK;
}
static unsigned sCommandRequests;
static unsigned sShaderRequests;
static status_t RunCommandRequest(const ResourceInfo&, void*, size_t, bool& recovery, bool shader)
{
	assert(sLockDepth == 1);
	sCommandRequests++;
	sShaderRequests += shader;
	recovery = true;
	return B_OK;
}

static status_t AccessClient(void* cookie) { assert(cookie == &sFirmwareRequests); return B_OK; }
static status_t ControlClient(void* cookie, uint32 op, void*, size_t)
{
    assert(cookie == &sFirmwareRequests
        && ((op >= kGetClientInfo && op <= kGetBufferInfo) || (op >= kCreateVm && op <= kBindVm)
            || (op >= kCreateHeap && op <= kGetHeapInfo)));
    return B_NOT_SUPPORTED;
}

static unsigned sQueueRequests;
static status_t ControlQueues(const ResourceInfo&, void* cookie, uint32 op, void*, size_t, bool&, void* syncClient = NULL)
{
	assert(syncClient == NULL || syncClient == &sFirmwareRequests);
	assert(cookie == &sFirmwareRequests && op >= kCreateQueue && op <= kSubmitQueueSync);
	assert(sLockDepth == unsigned(op == kCreateQueue || op == kDestroyQueue));
	sQueueRequests++;
	return B_OK;
}

static status_t ControlSync(void* cookie, uint32 op, void*, size_t)
{
	assert(cookie == &sFirmwareRequests && op >= kCreateSync && op <= kTransferSync);
	assert(sLockDepth == 0); return B_OK;
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
	for (device_node* node : {&sBusNode, &sRoot, &sGpu, &sClock, &sPower,
			&sPmu, &sRegulator, &sGic, &sFixed, &sDomain}) {
		assert(node->held == 0);
		node->properties.clear();
		node->wrongModule = false;
		node->parent = &sRoot;
	}
	sRoot.parent = &sBusNode;
	sBusNode.parent = NULL;
	sRoot.name = "";
	sGpu.name = "gpu@fb000000";
	sPmu.name = "power-management@fd8d8000";
	sPower.parent = &sPmu;
	sFixed.name = "clock-0";
	sDomain.name = "power-domain@12";
	sDomain.parent = &sPower;
	sPhandles = {{33, &sClock}, {34, &sPower}, {77, &sRegulator}};
	sDecodedIrqs[0] = 124; sDecodedIrqs[1] = 125; sDecodedIrqs[2] = 126;
	sWrongIrqController = false;
	sGpu.base = 0xfb000000; sGpu.size = 0x200000;
	sClock.base = 0xfd7c0000; sClock.size = 0x5c000;
	sPmu.base = 0xfd8d8000; sPmu.size = 0x400;
	sGic.base = 0xfe600000; sGic.size = 0x10000;
	Strings(sRoot, "compatible", {"radxa,rock-5-itx", "rockchip,rk3588"});
	Strings(sGpu, "compatible", {"rockchip,rk3588-mali", "arm,mali-valhall-csf"});
	Strings(sGpu, "status", {"okay"});
	Strings(sGpu, "interrupt-names", {"job", "mmu", "gpu"});
	Strings(sGpu, "clock-names", {"core", "coregroup", "stacks"});
	Cells(sGpu, "interrupts", {0, 92, 4, 0, 0, 93, 4, 0, 0, 94, 4, 0});
	Cells(sGpu, "clocks", {33, 262, 33, 263, 33, 264});
	Cells(sGpu, "power-domains", {34, 12});
	Cells(sGpu, "mali-supply", {77});
	Strings(sGic, "compatible", {"arm,gic-v3"});
	Cells(sGic, "#interrupt-cells", {4});
	Strings(sClock, "compatible", {"rockchip,rk3588-cru"});
	Cells(sClock, "#clock-cells", {1});
	Strings(sPower, "compatible", {"rockchip,rk3588-power-controller"});
	Cells(sPower, "#power-domain-cells", {1});
	Strings(sPmu, "compatible", {"rockchip,rk3588-pmu", "syscon", "simple-mfd"});
	Strings(sRegulator, "regulator-name", {"vdd_gpu_s0"});
	Cells(sRegulator, "regulator-min-microvolt", {550000});
	Cells(sRegulator, "regulator-max-microvolt", {950000});
	sRegulator.properties["regulator-boot-on"] = {};
	Strings(sFixed, "compatible", {"fixed-clock"});
	Strings(sFixed, "clock-output-names", {"spll"});
	Cells(sFixed, "#clock-cells", {0});
	Cells(sFixed, "clock-frequency", {702000000});
	Cells(sDomain, "reg", {12});
	Cells(sDomain, "clocks", {33, 262, 33, 263, 33, 264});
	Cells(sDomain, "#power-domain-cells", {0});
	Cells(sDomain, "domain-supply", {77});
}

int
main()
{
	static_assert(sizeof(ResourceInfo) == 168, "Diagnostic ABI layout changed");
	static_assert(sizeof(PlatformSnapshot) == 64, "Platform ABI layout changed");
	assert(sysconf(_SC_PAGESIZE) == B_PAGE_SIZE);
	sDeviceManager = &sManager;
	Prepare();
	ResourceInfo good = {};
	assert(ReadResources(&sGpu, good) && ResourcesMatch(good));
	assert(good.supplyPhandle == 77 && good.interrupts[2] == 126);
	assert(good.clockBase == 0xfd7c0000 && good.powerBase == 0xfd8d8000);
	assert(ReadIdentityProfile(&sGpu, good));
	// Phandles are references, not fixed numerical board identifiers.
	sPhandles.erase(33); sPhandles[909] = &sClock;
	Cells(sGpu, "clocks", {909, 262, 909, 263, 909, 264});
	Cells(sDomain, "clocks", {909, 262, 909, 263, 909, 264});
	ResourceInfo moved = {};
	assert(ReadResources(&sGpu, moved) && memcmp(&moved, &good, sizeof(good)) == 0);
	assert(ReadIdentityProfile(&sGpu, moved));

	std::vector<std::function<void()> > faults = {
		[] { sRoot.properties["compatible"].pop_back(); },
		[] { Strings(sRoot, "compatible", {"radxa,rock-5b", "rockchip,rk3588"}); },
		[] { Strings(sGpu, "status", {"disabled"}); },
		[] { sGpu.wrongModule = true; },
		[] { sBusNode.wrongModule = true; },
		[] { Strings(sGpu, "status", {"okay", "disabled"}); },
		[] { sClock.wrongModule = true; },
		[] { sPmu.wrongModule = true; },
		[] { sGpu.size = 0x1000; },
		[] { sGpu.base += 4096; },
		[] { sClock.base += 4096; },
		[] { sPmu.size = 0x100; },
		[] { sGic.base += 4096; },
		[] { sGpu.properties.erase("mali-supply"); },
		[] { sPhandles.erase(77); },
		[] { Cells(sGpu, "mali-supply", {0}); },
		[] { Strings(sRegulator, "regulator-name", {"vdd_cpu_lit_s0"}); },
		[] { Cells(sRegulator, "regulator-max-microvolt", {1050000}); },
		[] { sPower.parent = &sClock; },
		[] { Cells(sGpu, "power-domains", {34, 13}); },
		[] { Cells(sPower, "#power-domain-cells", {2}); },
		[] { Cells(sClock, "#clock-cells", {2}); },
		[] { Cells(sGpu, "clocks", {33, 262, 34, 263, 33, 264}); },
		[] { Cells(sGpu, "clocks", {33, 262, 33, 264, 33, 263}); },
		[] { sGpu.properties["clocks"].pop_back(); },
		[] { Strings(sGpu, "clock-names", {"core", "coregroup", "stacks", "extra"}); },
		[] { Strings(sGpu, "clock-names", {"core", "coregroup", "core"}); },
		[] { sDecodedIrqs[1] = 93; },
		[] { sWrongIrqController = true; },
		[] { Cells(sGic, "#interrupt-cells", {3}); },
		[] { sGpu.properties["interrupts"][11] = 1; },
		[] { Cells(sGpu, "interrupts-extended", {1}); },
		[] { sGpu.properties["interrupts"].resize(12); },
		[] { Strings(sGpu, "interrupt-names", {"gpu", "mmu", "job"}); },
	};
	for (auto& fault : faults) {
		Prepare(); fault();
		ResourceInfo invalid;
		memset(&invalid, 0xa5, sizeof(invalid));
		assert(!ReadResources(&sGpu, invalid));
		for (unsigned char byte : std::vector<unsigned char>((unsigned char*)&invalid,
				(unsigned char*)&invalid + sizeof(invalid))) assert(byte == 0xa5);
	}
	std::vector<std::function<void()> > identityFaults = {
		[] { sFixed.name = "clock-unknown"; },
		[] { sFixed.parent = &sClock; },
		[] { Strings(sFixed, "status", {"disabled"}); },
		[] { Strings(sFixed, "compatible", {"unrelated-clock"}); },
		[] { Strings(sFixed, "clock-output-names", {"spll", "extra"}); },
		[] { Cells(sFixed, "clock-frequency", {1200000000}); },
		[] { Cells(sFixed, "#clock-cells", {1}); },
		[] { sDomain.name = "power-domain@13"; },
		[] { sDomain.parent = &sRegulator; sRegulator.parent = &sPower; },
		[] { Cells(sDomain, "reg", {13}); },
		[] { Cells(sDomain, "#power-domain-cells", {1}); },
		[] { Cells(sDomain, "clocks", {33, 262, 33, 264, 33, 263}); },
		[] { Cells(sDomain, "domain-supply", {78}); },
		[] { sRegulator.properties.erase("regulator-boot-on"); },
	};
	for (auto& fault : identityFaults) {
		Prepare(); fault();
		assert(!ReadIdentityProfile(&sGpu, good));
	}
	Prepare(); // Also asserts that parent-node references were released on failure.
	Controller controller{};
	controller.resources = good;
	OpenHandle opened{&controller, &sFirmwareRequests, &sFirmwareRequests};
	ResourceInfo copy;
	assert(Control(&opened, kGetResources, &copy, sizeof(copy)) == B_OK);
	assert(memcmp(&copy, &good, sizeof(good)) == 0);
	assert(Control(&opened, kGetResources, NULL, sizeof(copy)) == B_BAD_ADDRESS);
	assert(Control(&opened, kGetResources, &copy, sizeof(copy) - 1) == B_BAD_VALUE);
	assert(Control(&opened, kGetResources, &copy, sizeof(copy) + 1) == B_BAD_VALUE);
	assert(Control(&opened, kGetResources + 127, &copy, sizeof(copy)) == B_DEV_INVALID_IOCTL);
	assert(Control(&opened, kGetClientInfo, NULL, 0) == B_NOT_SUPPORTED);
	assert(Control(&opened, kBindVm, NULL, 0) == B_NOT_SUPPORTED);
	assert(Control(&opened, kCreateHeap, NULL, 0) == B_NOT_SUPPORTED);
	assert(Control(&opened, kDestroyHeap, NULL, 0) == B_NOT_SUPPORTED);
	assert(Control(&opened, kGetHeapInfo, NULL, 0) == B_NOT_SUPPORTED);
	assert(sMapAttempts == 0);
	copy.boardCompatible[16] = 'x';
	assert(!ResourcesMatch(copy));

	PlatformSnapshot snapshot;
	assert(Control(&opened, kGetPlatformSnapshot, &snapshot, sizeof(snapshot) - 1) == B_BAD_VALUE);
	assert(Control(&opened, kGetPlatformSnapshot, &snapshot, sizeof(snapshot) + 1) == B_BAD_VALUE);
	assert(Control(&opened, kGetPlatformSnapshot, NULL, sizeof(snapshot)) == B_BAD_ADDRESS);
	controller.resources.clockBase += B_PAGE_SIZE;
	assert(Control(&opened, kGetPlatformSnapshot, &snapshot, sizeof(snapshot)) == B_NOT_SUPPORTED);
	assert(sMapAttempts == 0);
	controller.resources = good;
	for (unsigned failure : {1u, 2u}) {
		sMapAttempts = 0; sFailMap = failure;
		memset(&snapshot, 0xa5, sizeof(snapshot));
		assert(Control(&opened, kGetPlatformSnapshot, &snapshot, sizeof(snapshot)) == B_NO_MEMORY);
		assert(sAreas.empty() && sMapAttempts == failure);
		for (unsigned char byte : std::vector<unsigned char>((unsigned char*)&snapshot,
				(unsigned char*)&snapshot + sizeof(snapshot))) assert(byte == 0xa5);
	}
	sFailMap = 0;
	for (int sample = 0; sample < 3; sample++) {
		sMapAttempts = 0;
		assert(Control(&opened, kGetPlatformSnapshot, &snapshot, sizeof(snapshot)) == B_OK);
		assert(sAreas.empty() && sMapAttempts == 2);
		assert(snapshot.version == 1 && snapshot.flags == 1);
		assert(snapshot.startedMicros == 1 + 2 * sample && snapshot.finishedMicros == 2 + 2 * sample);
		const uint32 selectors[] = {0x12340578, 0x1234057c, 0x12340580};
		const uint32 gates[] = {0x12340908, 0x1234090c};
		assert(memcmp(snapshot.clockSelect, selectors, sizeof(selectors)) == 0);
		assert(memcmp(snapshot.clockGate, gates, sizeof(gates)) == 0);
		assert(snapshot.idleRequest == 0xabcd010c && snapshot.idleAck == 0xabcd0118);
		assert(snapshot.idleStatus == 0xabcd0120 && snapshot.powerRequest == 0xabcd014c);
		assert(snapshot.powerRepair == 0xabcd0290);
	}
	IdentityInfo identity;
	sMapAttempts = 0;
	assert(Control(&opened, kCycleIdentity, &identity, sizeof(identity)) == B_NOT_ALLOWED);
	controller.identityEnabled = true;
	controller.identityNeedsRecovery = true;
	assert(Control(&opened, kCycleIdentity, &identity, sizeof(identity)) == B_BUSY);
	assert(Control(&opened, kCycleIdentity, &identity, sizeof(identity) - 1) == B_BAD_VALUE);
	assert(Control(&opened, kCycleIdentity, NULL, sizeof(identity)) == B_BAD_ADDRESS);
	assert(sMapAttempts == 0 && sLockDepth == 0);
	controller.identityNeedsRecovery = false;
	sAllowWritable = true;
	for (unsigned failure : {1u, 2u}) {
		sMapAttempts = 0; sFailMap = failure;
		assert(Control(&opened, kCycleIdentity, &identity, sizeof(identity)) == B_NO_MEMORY);
		assert(sMapAttempts == failure && sAreas.empty() && sLockDepth == 0);
	}
	sMapAttempts = sFailMap = 0;
	// The protected MMIO fixture has an unexpected initial state. No production
	// write may occur, even though the profile enabled the diagnostic ioctl.
	assert(Control(&opened, kCycleIdentity, &identity, sizeof(identity)) == B_OK);
	assert(identity.result == kInitialStateMismatch && identity.flags == 0);
	assert(!controller.identityNeedsRecovery && sAreas.empty() && sLockDepth == 0);
	for (unsigned failure : {0u, 3u}) {
		sMapAttempts = 0; sFailMap = failure; sAllowGpu = true;
		{
			MutexLocker locker(sHardwareLock);
			IdentityHardware hardware;
			assert(hardware.Init(good) == B_OK && sAreas.size() == 2);
			assert(hardware.MapGpu() == (failure == 0));
			if (failure == 0) {
				assert(sAreas.size() == 3 && hardware.ReadGpu(0) == 0xa8670005);
				hardware.UnmapGpu();
				assert(sAreas.size() == 2);
			}
		}
		assert(sAreas.empty() && sLockDepth == 0);
	}

	ResetInfo reset;
	sMapAttempts = 0;
	assert(Control(&opened, kCycleReset, &reset, sizeof(reset)) == B_NOT_ALLOWED);
	controller.resetEnabled = true;
	controller.identityNeedsRecovery = true;
	assert(Control(&opened, kCycleReset, &reset, sizeof(reset)) == B_BUSY);
	assert(Control(&opened, kCycleReset, &reset, sizeof(reset) - 1) == B_BAD_VALUE);
	assert(Control(&opened, kCycleReset, &reset, sizeof(reset) + 1) == B_BAD_VALUE);
	assert(Control(&opened, kCycleReset, NULL, sizeof(reset)) == B_BAD_ADDRESS);
	assert(sMapAttempts == 0);
	controller.identityNeedsRecovery = false;
	for (unsigned failure : {1u, 2u}) {
		sMapAttempts = 0; sFailMap = failure;
		assert(Control(&opened, kCycleReset, &reset, sizeof(reset)) == B_NO_MEMORY);
		assert(sMapAttempts == failure && sAreas.empty());
	}
	sMapAttempts = sFailMap = 0;
	assert(Control(&opened, kCycleReset, &reset, sizeof(reset)) == B_OK);
	assert(reset.result == kResetPowerCycleFailed && controller.identityNeedsRecovery);
	assert(sAreas.empty() && !sInstalled && !sRemoved);
	for (unsigned failure : {0u, 3u}) {
		sMapAttempts = 0; sFailMap = failure; sAllowResetGpu = true;
		MutexLocker locker(sHardwareLock);
		ResetHardware hardware;
		assert(hardware.Init(good) == B_OK && sAreas.size() == 2);
		assert(hardware.MapGpu() == (failure == 0));
		if (failure != 0) continue;
		auto allocation = sAreas.rbegin()->second.first;
		uint32* regs = (uint32*)((char*)allocation + B_PAGE_SIZE);
		regs[kGpuRaw / 4] = 0;
		regs[kGpuInterruptStatus / 4] = 0;
		regs[kGpuMask / 4] = 0;
		sFailInstall = true;
		assert(!hardware.InstallResetHandler() && !sHandler);
		sFailInstall = false;
		assert(hardware.InstallResetHandler());
		assert(InvokeInterrupt() == B_UNHANDLED_INTERRUPT);
		assert(hardware.BeginReset() == kResetOK);
		assert(regs[kGpuCommand / 4] == 0x101 && regs[kGpuMask / 4] == 0x100);
		regs[kGpuRaw / 4] = 0x100;
		regs[kGpuInterruptStatus / 4] = 0x100;
		regs[kGpuStatus / 4] = 0;
		sPauseInCpu = true;
		std::thread interrupt([] { assert(InvokeInterrupt() == B_HANDLED_INTERRUPT); });
		while (!sInsideCpu.load()) std::this_thread::yield();
		unsigned attempts = sSpinAttempts.load();
		std::atomic<bool> stopped(false);
		std::thread removal([&] { hardware.StopResetHandler(); stopped = true; });
		while (sSpinAttempts.load() == attempts) std::this_thread::yield();
		assert(!stopped.load() && sAreas.size() == 3);
		sReleaseCpu = true;
		interrupt.join(); removal.join();
		assert(stopped && !sHandler && sInstalled == 1 && sRemoved == 1);
		assert(hardware.ReadResetCapture().count == 1);
		assert(regs[kGpuMask / 4] == 0 && regs[kGpuClear / 4] == 0x100);
		hardware.UnmapGpu();
		assert(sAreas.size() == 2);
	}
	assert(sAreas.empty() && sLockDepth == 0);

	controller.identityNeedsRecovery = false;
	assert(Control(&opened, kCycleFirmware, NULL, 0) == B_NOT_ALLOWED);
	controller.firmwareEnabled = true;
	sFirmwareRetained = true;
	assert(Control(&opened, kCycleFirmware, NULL, 0) == B_BUSY);
	sFirmwareRetained = false;
	assert(Control(&opened, kCycleFirmware, NULL, 0) == B_OK);
	assert(sFirmwareRequests == 1 && controller.identityNeedsRecovery);
	assert(Control(&opened, kCycleFirmware, NULL, 0) == B_BUSY);
	assert(sFirmwareRequests == 1);
	controller.identityNeedsRecovery = false;
	assert(Control(&opened, kCycleCommands, NULL, 0) == B_NOT_ALLOWED);
	controller.commandsEnabled = true;
	sFirmwareRetained = true;
	assert(Control(&opened, kCycleCommands, NULL, 0) == B_BUSY && sCommandRequests == 0);
	sFirmwareRetained = false;
	assert(Control(&opened, kCycleCommands, NULL, 0) == B_OK);
	assert(sCommandRequests == 1 && controller.identityNeedsRecovery);
	assert(Control(&opened, kCycleCommands, NULL, 0) == B_BUSY);
	assert(Control(&opened, kCycleFirmware, NULL, 0) == B_BUSY);
	assert(sCommandRequests == 1 && sFirmwareRequests == 1);
	controller.identityNeedsRecovery = false;
	assert(Control(&opened, kCycleShader, NULL, 0) == B_NOT_ALLOWED);
	assert(sCommandRequests == 1 && sShaderRequests == 0);
	controller.shaderEnabled = true;
	sFirmwareRetained = true;
	assert(Control(&opened, kCycleShader, NULL, 0) == B_BUSY);
	assert(sCommandRequests == 1 && sShaderRequests == 0);
	sFirmwareRetained = false;
	assert(Control(&opened, kCycleShader, NULL, 0) == B_OK);
	assert(sCommandRequests == 2 && sShaderRequests == 1 && controller.identityNeedsRecovery);
	assert(Control(&opened, kCycleShader, NULL, 0) == B_BUSY);
	assert(Control(&opened, kCycleCommands, NULL, 0) == B_BUSY);
	assert(Control(&opened, kCycleFirmware, NULL, 0) == B_BUSY);
	assert(sCommandRequests == 2 && sShaderRequests == 1 && sFirmwareRequests == 1);

	size_t page = sysconf(_SC_PAGESIZE);
	uint8_t* memory = (uint8_t*)mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(memory != MAP_FAILED && mprotect(memory + page, page, PROT_NONE) == 0);
	uint32 value = 0;
	memcpy(memory + page - 5, "\x12\x34\x56\x78", 4);
	assert(ReadCells(memory + page - 5, 4, &value, 1) && value == 0x12345678);
	assert(!ReadCells(memory + page - 3, 3, &value, 1));
	memcpy(memory + page - 3, "abc", 3);
	assert(StringIndex(memory + page - 3, 3, "abc") == -1);
	assert(StringIndex("core\0bad", 8, "core") == -1);
	assert(StringIndex("core\0core", 10, "core") == -1);
	assert(munmap(memory, 2 * page) == 0);
	controller.identityNeedsRecovery = false;
	controller.shaderEnabled = false;
	assert(Control(&opened, kCreateQueue, NULL, 0) == B_NOT_ALLOWED);
	controller.shaderEnabled = true;
	for (uint32 op = kCreateQueue; op <= kSubmitQueueSync; op++)
		assert(Control(&opened, op, NULL, 0) == B_OK);
	assert(sQueueRequests == 6 && sLockDepth == 0);
	controller.identityNeedsRecovery = true;
	assert(Control(&opened, kCreateQueue, NULL, 0) == B_BUSY && sQueueRequests == 6);
	assert(Control(&opened, kGetQueueInfo, NULL, 0) == B_OK);
	assert(Control(&opened, kDestroyQueue, NULL, 0) == B_OK);
	controller.identityNeedsRecovery = false;
	sFirmwareRetained = true;
	for (uint32 op = kCreateSync; op <= kTransferSync; op++)
		assert(Control(&opened, op, NULL, 0) == B_OK);
	IdentityInfo runtimeIdentity{}; ResetInfo runtimeReset{};
	assert(Control(&opened, kCycleIdentity, &runtimeIdentity, sizeof(runtimeIdentity)) == B_BUSY);
	assert(Control(&opened, kCycleReset, &runtimeReset, sizeof(runtimeReset)) == B_BUSY);
	sFirmwareRetained = false;
	puts("MALI_CSF_RESOURCES_TEST_PASS");
}
