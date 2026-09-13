#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <stdexcept>

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using int32 = int32_t;
using status_t = int;
using sem_id = int;
using thread_id = int;
using driver_filter_t = int(void*);
using driver_intr_t = void(void*);
using interrupt_handler = int32(*)(void*);
#define B_PRIu32 PRIu32
#define B_PRId32 PRId32
enum { B_OK = 0, B_NO_MEMORY = -10, B_ERROR = -20, B_BAD_VALUE = -30,
	B_UNSUPPORTED = -40, B_IO_ERROR = -50, B_BUSY = -60, B_DO_NOT_RESCHEDULE = 1,
	B_REAL_TIME_DISPLAY_PRIORITY = 100, B_UNHANDLED_INTERRUPT = 0,
	B_HANDLED_INTERRUPT = 1, B_INVOKE_SCHEDULER = 2, INTR_MPSAFE = 1,
	SYS_RES_IRQ = 7, BUS_SPACE_TAG_IRQ = 3, BUS_SPACE_TAG_MSI = 4,
	PCI_interrupt_line = 0x3c, RGE_IMR = 0x38, RGE_ISR = 0x3c, RGE_FLAG_MSI = 1 };
struct pci_info { uint8 bus, device, function; };
struct root_device_softc { struct pci_info pci_info; bool is_msi, is_msix; };
struct device { device* root; device* parent; const char* device_name; void* softc; };
using device_t = device*;
struct resource { int r_type, r_bustag; uint64_t r_bushandle; };
struct rge_softc { uint32_t rge_flags; int32_t rge_haiku_intr_count; };
struct pci_module {
	status_t (*enable_msi)(uint8, uint8, uint8);
	status_t (*enable_msix)(uint8, uint8, uint8);
	status_t (*disable_msi)(uint8, uint8, uint8);
};
struct intx_module {
	status_t (*get_irq)(uint8, uint8, uint8, uint32*);
	status_t (*set_enabled)(uint8, uint8, uint8, bool);
};

static std::vector<std::string> sEvents;
static std::unordered_set<void*> sAllocations;
static bool sFailMalloc, sFailSem, sFailSpawn, sFailInstall, sFailEnable, sFailGet, sFailDisable;
static bool sSemAlive, sThreadAlive, sResumed, sInstalled;
static int sIRQ, sReleases;
static uint32 sRouteIRQ = 277, sISR, sIMR;
static int32 (*sThreadFunction)(void*);
static void* sThreadArg;
static interrupt_handler sInstalledHandler;
static void* sInstalledArg;
static root_device_softc sRootSoftc{{7, 0, 0}, false, false};
static rge_softc sRge{};
static device sRoot{nullptr, nullptr, "pci", &sRootSoftc};
static device sDevice{&sRoot, &sRoot, "rge", &sRge};
static int Giant;

static void* testMalloc(size_t size) {
	if (sFailMalloc) return nullptr;
	void* memory = std::malloc(size);
	if (memory != nullptr) sAllocations.insert(memory);
	return memory;
}
static void testFree(void* memory) {
	assert(sAllocations.erase(memory) == 1);
	std::free(memory);
}
static pci_info* get_device_pci_info(device_t dev) { return &sRootSoftc.pci_info; }
static uint32 pci_read_config(device_t, int, int) { return 37; }
static void device_printf(device_t, const char*, ...) {}
static void panic(const char*, ...) { throw std::runtime_error("unmasked route"); }
static rge_softc* device_get_softc(device_t dev) { return (rge_softc*)dev->softc; }
static status_t getIRQ(uint8 bus, uint8, uint8, uint32* irq) {
	assert(bus == 7);
	if (sFailGet) return B_ERROR;
	*irq = sRouteIRQ;
	return B_OK;
}
static status_t route(uint8 bus, uint8, uint8, bool enable) {
	assert(bus == 7);
	sEvents.push_back(enable ? "enable" : "disable");
	if (enable) assert(sInstalled);
	return (enable ? sFailEnable : sFailDisable) ? B_ERROR : B_OK;
}
static status_t msiEnable(uint8 bus, uint8 device, uint8 function) {
	return route(bus, device, function, true);
}
static status_t msiDisable(uint8 bus, uint8 device, uint8 function) {
	return route(bus, device, function, false);
}
static pci_module sPci{msiEnable, msiEnable, msiDisable};
static pci_module* gPci = &sPci;
static intx_module sIntx{getIRQ, route};
static intx_module* gPciIntx = &sIntx;
static sem_id create_sem(int count, const char*) {
	assert(count == 0 && !sSemAlive);
	if (sFailSem) return B_ERROR;
	sSemAlive = true;
	return 42;
}
static status_t delete_sem(sem_id id) {
	assert(id == 42 && sSemAlive);
	sSemAlive = false;
	sEvents.push_back("delete_sem");
	return B_OK;
}
static status_t acquire_sem(sem_id id) { assert(!sSemAlive); return B_ERROR; }
static status_t release_sem_etc(sem_id id, int count, int flags) {
	assert(id == 42 && sSemAlive && count == 1);
	sReleases++;
	return B_OK;
}
static thread_id spawn_kernel_thread(int32(*function)(void*), const char*, int, void* arg) {
	if (sFailSpawn) return B_ERROR;
	assert(!sThreadAlive);
	sThreadAlive = true;
	sThreadFunction = function;
	sThreadArg = arg;
	return 43;
}
static status_t resume_thread(thread_id id) {
	assert(id == 43 && sThreadAlive);
	sResumed = true;
	sEvents.push_back("resume");
	return B_OK;
}
static status_t wait_for_thread(thread_id id, status_t* result) {
	assert(id == 43 && sThreadAlive && sResumed && !sSemAlive);
	*result = sThreadFunction(sThreadArg);
	sThreadAlive = false;
	sEvents.push_back("join");
	return B_OK;
}
static status_t install_io_interrupt_handler(int irq, interrupt_handler handler, void* arg, int) {
	if (sFailInstall) return B_ERROR;
	assert(!sInstalled);
	sInstalled = true;
	sIRQ = irq;
	sInstalledHandler = handler;
	sInstalledArg = arg;
	sEvents.push_back("install");
	return B_OK;
}
static status_t remove_io_interrupt_handler(int irq, interrupt_handler handler, void* arg) {
	assert(sInstalled && sIRQ == irq && sInstalledHandler == handler && sInstalledArg == arg);
	sInstalled = false;
	sEvents.push_back("remove");
	return B_OK;
}
static int32 atomic_add(int32* value, int32 amount) {
	return __atomic_fetch_add(value, amount, __ATOMIC_RELAXED);
}
static void atomic_or(int32* value, int32 mask) { __atomic_fetch_or(value, mask, __ATOMIC_RELAXED); }
static void atomic_and(int32* value, int32 mask) { __atomic_fetch_and(value, mask, __ATOMIC_RELAXED); }
static void mtx_lock(int*) {}
static void mtx_unlock(int*) {}
static void HAIKU_REENABLE_INTERRUPTS(device_t) {}
int HAIKU_CHECK_DISABLE_INTERRUPTS(device_t);
int bus_teardown_intr(device_t, resource*, void*);
static uint32 readRegister(uint32 reg) {
	assert(reg == RGE_ISR || reg == RGE_IMR);
	sEvents.push_back(reg == RGE_IMR ? "read_mask" : "read_status");
	return reg == RGE_IMR ? sIMR : sISR;
}
static void writeRegister(uint32 reg, uint32 value) {
	assert(reg == RGE_IMR);
	sIMR = value;
	sEvents.push_back("write_mask");
}
#define RGE_READ_4(sc, reg) readRegister(reg)
#define RGE_WRITE_4(sc, reg, value) writeRegister(reg, value)
#define malloc testMalloc
#define free testFree
#include "interrupts_under_test.inc"
#undef malloc
#undef free

namespace CoreTest {
enum { PCI_interrupt_pin = 0x3d, PCI_command = 4, PCI_command_int_disable = 0x400,
	PCI_msi_control = 2, PCI_msix_control = 2, PCI_msi_control_enable = 1,
	PCI_msix_control_enable = 0x8000 };
struct PCIDev {
	uint8 domain = 0, bus = 1, device = 0, function = 0;
	bool intx_enabled = false;
	uint8 intx_pin = 0;
	struct { bool msi_capable = false; uint32 configured_count = 0; uint8 capability_offset = 0x50; } msi;
	struct { bool msix_capable = false; uint32 configured_count = 0; uint8 capability_offset = 0x70; } msix;
};
struct State {
	uint32 pin = 1, command = PCI_command_int_disable, msiControl = 0, msixControl = 0;
	bool enabled = false, badRead = false, badWrite = false, failEnable = false, failDisable = false;
	std::vector<std::string> events;
};
struct Provider {
	status_t (*get_irq)(void*, uint8, uint8, uint8, uint8, uint32*);
	status_t (*set_enabled)(void*, uint8, uint8, uint8, uint8, bool);
};
static status_t get(void*, uint8 bus, uint8 device, uint8 function, uint8 pin, uint32* irq) {
	assert(bus == 1 && device == 0 && function == 0);
	if (pin != 1) return B_UNSUPPORTED;
	*irq = 277;
	return B_OK;
}
static status_t set(void* cookie, uint8 bus, uint8 device, uint8 function, uint8 pin, bool enable) {
	assert(bus == 1 && device == 0 && function == 0 && pin == 1);
	State& state = *(State*)cookie;
	state.events.push_back(enable ? "route_on" : "route_off");
	if (enable ? state.failEnable : state.failDisable) return B_ERROR;
	state.enabled = enable;
	return B_OK;
}
static Provider provider{get, set};
struct domain_data {
	Provider* intx_controller;
	void* controller_cookie;
	status_t intx_status = B_OK;
};
class PCI {
public:
	State state;
	domain_data domain{&provider, &state, B_OK};
	domain_data* _GetDomainData(uint8) { return &domain; }
	uint32 ReadConfig(PCIDev*, uint16 offset, uint8 size) {
		if (state.badRead) return size == 1 ? 0xff : 0xffff;
		switch (offset) {
			case PCI_interrupt_pin: return state.pin;
			case PCI_interrupt_line: return 37;
			case PCI_command: return state.command;
			case 0x52: return state.msiControl;
			case 0x72: return state.msixControl;
		}
		assert(false); return 0;
	}
	status_t WriteConfig(PCIDev*, uint16 offset, uint8 size, uint32 value) {
		assert(offset == PCI_command && size == 2);
		state.events.push_back(value & PCI_command_int_disable ? "command_off" : "command_on");
		if (state.badWrite) return B_ERROR;
		state.command = value;
		return B_OK;
	}
	status_t GetIntxIRQ(PCIDev*, uint32*);
	status_t SetIntxEnabled(PCIDev*, bool);
};
#include "pci_intx_under_test.inc"

static void run() {
	PCI pci; PCIDev dev; uint32 irq = 0;
	assert(pci.GetIntxIRQ(&dev, &irq) == B_OK && irq == 277);
	assert(pci.GetIntxIRQ(&dev, nullptr) == B_BAD_VALUE);
	pci.domain.intx_status = B_ERROR;
	assert(pci.GetIntxIRQ(&dev, &irq) == B_ERROR && irq == 0);
	assert(pci.SetIntxEnabled(&dev, true) == B_ERROR && pci.state.events.empty());
	pci.domain.intx_status = B_OK; pci.domain.intx_controller = nullptr;
	assert(pci.GetIntxIRQ(&dev, &irq) == B_OK && irq == 37);
	assert(pci.SetIntxEnabled(&dev, true) == B_OK && pci.state.events.empty());
	pci.domain.intx_controller = &provider;
	pci.state.pin = 0;
	assert(pci.GetIntxIRQ(&dev, &irq) == B_UNSUPPORTED && irq == 0);
	pci.state.pin = 1;
	dev.msi.configured_count = 1;
	assert(pci.SetIntxEnabled(&dev, true) == B_BUSY && pci.state.events.empty());
	dev.msi.configured_count = 0; dev.msix.configured_count = 1;
	assert(pci.SetIntxEnabled(&dev, true) == B_BUSY && pci.state.events.empty());
	dev.msix.configured_count = 0; dev.msi.msi_capable = true; pci.state.msiControl = 1;
	assert(pci.SetIntxEnabled(&dev, true) == B_BUSY && pci.state.events.empty());
	pci.state.msiControl = 0; dev.msix.msix_capable = true; pci.state.msixControl = 0x8000;
	assert(pci.SetIntxEnabled(&dev, true) == B_BUSY && pci.state.events.empty());
	pci.state.msixControl = 0; pci.state.failEnable = true;
	assert(pci.SetIntxEnabled(&dev, true) == B_ERROR && !pci.state.enabled && !dev.intx_enabled);
	pci.state.failEnable = false; pci.state.events.clear();
	assert(pci.SetIntxEnabled(&dev, true) == B_OK && pci.state.enabled && dev.intx_enabled);
	assert((pci.state.events == std::vector<std::string>{"route_on", "command_on"}));
	pci.state.events.clear(); pci.state.failDisable = true;
	assert(pci.SetIntxEnabled(&dev, false) == B_ERROR && dev.intx_enabled && dev.intx_pin == 1);
	pci.state.failDisable = false; pci.state.events.clear();
	assert(pci.SetIntxEnabled(&dev, false) == B_OK && !pci.state.enabled && !dev.intx_enabled);
	assert((pci.state.events == std::vector<std::string>{"command_off", "route_off"}));
	pci.state.events.clear(); pci.state.badWrite = true;
	assert(pci.SetIntxEnabled(&dev, true) == B_ERROR && !pci.state.enabled && !dev.intx_enabled);
	assert(pci.state.events.back() == "route_off");
	pci.state.badWrite = false;
	assert(pci.SetIntxEnabled(&dev, true) == B_OK);
	pci.state.badRead = true; pci.state.events.clear();
	assert(pci.SetIntxEnabled(&dev, false) == B_OK && !pci.state.enabled && !dev.intx_enabled);
	assert((pci.state.events == std::vector<std::string>{"route_off"}));
}
} // namespace CoreTest

static void handler(void*) {}
static int filter(void*) { return B_HANDLED_INTERRUPT; }
static void reset() {
	assert(!sSemAlive && !sThreadAlive && !sInstalled && sAllocations.empty());
	sFailMalloc = sFailSem = sFailSpawn = sFailInstall = sFailEnable = sFailGet = sFailDisable = false;
	sResumed = false; sReleases = 0; sISR = 4; sIMR = 4;
	sRootSoftc.is_msi = sRootSoftc.is_msix = false;
	sEvents.clear(); sRge = {}; gPciIntx = &sIntx;
}
static size_t event(const char* name) {
	auto it = std::find(sEvents.begin(), sEvents.end(), name);
	assert(it != sEvents.end());
	return it - sEvents.begin();
}
int main() {
	CoreTest::run();
	reset();
	resource irq{SYS_RES_IRQ, 0, 0};
	for (uint32 value : {277u, 282u}) {
		sRouteIRQ = value;
		assert(bus_alloc_irq_resource(&sDevice, &irq) == 0 && irq.r_bushandle == value);
	}
	sFailGet = true;
	assert(bus_alloc_irq_resource(&sDevice, &irq) != 0); // No byte fallback.
	sFailGet = false; sRouteIRQ = UINT32_MAX;
	assert(bus_alloc_irq_resource(&sDevice, &irq) != 0);
	gPciIntx = nullptr;
	assert(bus_alloc_irq_resource(&sDevice, &irq) == 0 && irq.r_bushandle == 37);
	irq = {SYS_RES_IRQ, BUS_SPACE_TAG_IRQ, 277};
	for (bool* fault : {&sFailMalloc, &sFailSem, &sFailSpawn, &sFailInstall, &sFailEnable}) {
		reset(); *fault = true;
		void* cookie = (void*)1;
		assert(bus_setup_intr(&sDevice, &irq, INTR_MPSAFE, nullptr, handler, nullptr, &cookie) != 0);
		assert(cookie == nullptr);
		reset(); // Every failure must leave no worker, handler, semaphore or allocation.
	}
	for (bool msix : {false, true}) {
		reset(); sFailEnable = true;
		sRootSoftc.is_msi = !msix; sRootSoftc.is_msix = msix;
		irq.r_bustag = BUS_SPACE_TAG_MSI;
		void* cookie = nullptr;
		assert(bus_setup_intr(&sDevice, &irq, 0, nullptr, handler, nullptr, &cookie) == ENODEV);
		reset();
	}
	irq.r_bustag = BUS_SPACE_TAG_IRQ;
	for (bool filtered : {false, true}) {
		reset(); void* cookie = nullptr;
		assert(bus_setup_intr(&sDevice, &irq, 0, filtered ? filter : nullptr,
			handler, nullptr, &cookie) == 0);
		assert(sIRQ == 277 && event("install") < event("enable"));
		if (!filtered) {
			assert(event("enable") < event("resume"));
			assert(sInstalledHandler(sInstalledArg) == B_INVOKE_SCHEDULER);
			assert(sReleases == 1 && sIMR == 0 && sEvents.back() == "read_mask");
		}
		assert(bus_teardown_intr(&sDevice, &irq, cookie) == 0);
		assert(event("disable") < event("remove"));
		reset();
	}
	void* cookie = nullptr;
	assert(bus_setup_intr(&sDevice, &irq, 0, nullptr, handler, nullptr, &cookie) == 0);
	sFailDisable = true;
	bool stopped = false;
	try { bus_teardown_intr(&sDevice, &irq, cookie); }
	catch (const std::runtime_error&) { stopped = true; }
	assert(stopped && sInstalled && sSemAlive && sThreadAlive);
	sFailDisable = false;
	assert(bus_teardown_intr(&sDevice, &irq, cookie) == 0);
	reset();
	for (uint32 status : {0u, 0xffffffffu, 8u}) {
		sISR = status; sIMR = 4;
		assert(HAIKU_CHECK_DISABLE_INTERRUPTS(&sDevice) == 0 && sIMR == 4);
	}
	sISR = 4; sIMR = 4; sRge.rge_haiku_intr_count = INT32_MAX;
	assert(HAIKU_CHECK_DISABLE_INTERRUPTS(&sDevice) == 1 && sIMR == 0);
	assert(sRge.rge_haiku_intr_count == INT32_MIN);
	assert(sEvents.back() == "read_mask");
	puts("bus interrupts: wide IRQ, failure cleanup, route ordering and Realtek level masking passed");
}
