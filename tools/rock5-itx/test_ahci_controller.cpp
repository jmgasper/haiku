/* Inject faults into the production controller initialization and teardown. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int32 = int32_t;
using uchar = unsigned char;
using status_t = int;
using area_id = int;
using port_id = int;
using phys_addr_t = uint64_t;
struct device_node;
struct pci_device;
struct scsi_ccb;
struct module_info {};
constexpr int B_OK = 0, B_ERROR = -1, B_NO_MEMORY = -2;
constexpr int PCI_cap_id_sata = 0x12, PCI_command = 4;
constexpr int PCI_command_io = 1, PCI_command_memory = 2, PCI_command_master = 4,
	PCI_command_int_disable = 0x400;
constexpr int PCI_VENDOR_JMICRON = 0x197b, PCI_JMICRON_CONTROLLER_CONTROL_1 = 0x40;
constexpr int B_KERNEL_READ_AREA = 1, B_KERNEL_WRITE_AREA = 2;
constexpr const char* B_PCI_INTX_MODULE_NAME = "intx";
#define _PACKED __attribute__((packed))
#define TRACE(...)
#define FLOW(...)
#define dprintf(...)
#define ASSERT assert
static void memory_full_barrier() {}
static int fls(uint32 n) { return n == 0 ? 0 : 32 - __builtin_clz(n); }
#include "definitions.inc"
struct pci_info {
	uint16 vendor_id = 0x1b21, device_id = 0x1164;
	uint8 bus = 3, device = 0, function = 0;
	struct { struct {
		uint8 interrupt_line = 31;
		phys_addr_t base_registers[6] = {0, 0, 0, 0, 0, 0xf1002000};
		size_t base_register_sizes[6] = {0, 0, 0, 0, 0, 8192};
	} h0; } u;
};
static pci_info info;
static ahci_hba regs;
static uint16 command;
static int irq, areas, references, instance, portObjects, interruptsEnabled;
static bool allocatedMSI, msiEnabled;
static std::string fault;
static std::vector<std::string> events;
static void event(const char* s) { events.emplace_back(s); }
static int find_port(const char*) { return fault == "duplicate" ? 5 : -1; }
static int create_port(int, const char*)
{
	if (fault == "instance") return B_ERROR;
	instance++; return 7;
}
static void delete_port(int n) { assert(n == 7 && instance == 1); instance--; }
static void get_device_info(uint16, uint16, const char**, uint32*) {}
static int install_io_interrupt_handler(uint32 n, int32 (*)(void*), void*, int)
{
	assert(!irq && !(regs.ghc & GHC_IE) && !msiEnabled);
	if (fault == "handler") return B_ERROR;
	irq = n; event("install"); return B_OK;
}
static void remove_io_interrupt_handler(uint32 n, int32 (*)(void*), void*)
{
	assert(irq == int(n) && !(regs.ghc & GHC_IE) && !interruptsEnabled && !msiEnabled);
	assert(command & PCI_command_int_disable);
	irq = 0; event("remove");
}
static int map_mem(void** out, phys_addr_t, size_t, uint32, const char*)
{
	if (fault == "map") return B_ERROR;
	*out = &regs; areas++; return 8;
}
static void delete_area(int n)
{
	assert(n == 8 && areas == 1 && !irq && !portObjects);
	areas--; event("unmap");
}
struct pci_device_module_info {
	void get_pci_info(pci_device*, pci_info* out) { *out = info; }
	int find_pci_capability(pci_device*, int, uchar*) { return B_ERROR; }
	uint32 read_pci_config(pci_device*, int offset, int)
	{
		assert(offset == PCI_command); return command;
	}
	void write_pci_config(pci_device*, int offset, int, uint32 value)
	{
		assert(offset == PCI_command);
		if (!(value & PCI_command_int_disable)) assert(irq && !allocatedMSI);
		command = value;
	}
	int get_msi_count(pci_device*) { return fault.rfind("msi", 0) == 0 ? 1 : 0; }
	int configure_msi(pci_device*, int, uint32* vector)
	{
		if (fault == "msi-config") return B_ERROR;
		assert(!allocatedMSI); allocatedMSI = true; *vector = 9000; return B_OK;
	}
	int enable_msi(pci_device*)
	{
		assert(allocatedMSI && irq == 9000 && !(regs.ghc & GHC_IE));
		msiEnabled = true;
		return fault == "msi-enable" ? B_ERROR : B_OK;
	}
	void disable_msi(pci_device*) { assert(allocatedMSI); msiEnabled = false; event("msi-off"); }
	void unconfigure_msi(pci_device*)
	{
		assert(allocatedMSI && !msiEnabled && !irq);
		allocatedMSI = false; event("msi-free");
	}
};
struct pci_intx_module_info {
	int get_irq(uint8 bus, uint8 device, uint8 function, uint32* n)
	{
		assert(bus == info.bus && device == info.device && function == info.function);
		*n = fault == "line-host" ? 31 : 287;
		return fault == "provider" ? B_ERROR : B_OK;
	}
	int set_enabled(uint8, uint8, uint8, bool enabled)
	{
		if (enabled) {
			assert(irq && !(regs.ghc & GHC_IE) && !allocatedMSI);
			if (fault == "enable") return B_ERROR;
		}
		interruptsEnabled = enabled; event(enabled ? "intx-on" : "intx-off");
		return B_OK;
	}
};
static pci_intx_module_info intx;
static int get_module(const char*, module_info** out)
{
	if (fault == "no-module" || fault == "no-line") return B_ERROR;
	*out = reinterpret_cast<module_info*>(&intx); references++; return B_OK;
}
static void put_module(const char*) { assert(references == 1); references--; }
class AHCIController;
class AHCIPort {
public:
	AHCIPort(AHCIController*, int) { portObjects++; }
	~AHCIPort() { portObjects--; }
	int Init1() { return B_OK; }
	int Init2() { return fault == "port" ? B_ERROR : B_OK; }
	void Uninit() { assert(!irq && !(regs.ghc & GHC_IE)); event("port-stop"); }
};
#define private public
#include "ahci_controller.h"
#undef private
status_t AHCIController::ResetController() { return fault == "reset" ? B_ERROR : B_OK; }
int32 AHCIController::Interrupt(void*) { return 0; }
#include "controller.inc"

int main()
{
	pci_device_module_info pci;
	for (const char* test : {"ok", "line-host", "no-module", "msi-ok", "duplicate",
		"instance", "no-line", "provider", "map", "reset", "handler", "enable",
		"msi-enable", "msi-config", "port"}) {
		fault = test; info = {}; regs = {}; regs.cap = CAP_S64A; regs.pi = 15;
		command = PCI_command_memory | PCI_command_int_disable;
		if (fault == "no-line") info.u.h0.interrupt_line = 0xff;
		events.clear();
		bool success = fault == "ok" || fault == "line-host" || fault == "no-module"
			|| fault == "msi-ok" || fault == "msi-config";
		{
			AHCIController controller(nullptr, &pci, nullptr);
			assert((controller.Init() == B_OK) == success);
			if (success) {
				assert(regs.ghc & GHC_IE);
				assert(irq == (fault == "msi-ok" ? 9000
					: fault == "line-host" || fault == "no-module" ? 31 : 287));
				if (fault != "msi-ok") assert(!(command & PCI_command_int_disable));
			}
			controller.Uninit();
			assert(!irq && !references && !instance && !areas && !portObjects
				&& !allocatedMSI && !msiEnabled && !interruptsEnabled);
			auto count = events.size();
			controller.Uninit();
			assert(events.size() == count);
		}
	}
	puts("AHCI interrupt setup and partial-init cleanup passed");
}
