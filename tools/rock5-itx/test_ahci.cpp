/* Production AHCI methods, with physical memory and HBA completions substituted. */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using uchar = unsigned char;
using status_t = int;
using area_id = int;
using sem_id = int;
using spinlock = bool;
using cpu_status = int;
using bigtime_t = int64_t;
using phys_addr_t = uint64_t;
using addr_t = uintptr_t;
struct physical_entry { phys_addr_t address; size_t size; };
struct scsi_ccb {
	size_t data_length = 0;
	physical_entry* sg_list = nullptr;
	int sg_count = 0;
	uint8 cdb[16] = {};
	uint8 cdb_length = 12;
};
struct ata_device_infoblock;
struct scsi_unmap_parameter_list;
constexpr status_t B_OK = 0, B_ERROR = -1, B_BAD_VALUE = -2, B_BAD_ADDRESS = -3,
	B_BAD_DATA = -4, B_TIMED_OUT = -5, B_INTERRUPTED = -6;
constexpr int B_RELATIVE_TIMEOUT = 1, SCSI_OP_TEST_UNIT_READY = 0;
constexpr int ATA_STATUS_BUSY = 0x80, ATA_STATUS_DATA_REQUEST = 8,
	ATA_STATUS_ERROR = 1, ATA_STATUS_DEVICE_FAULT = 0x20;
#define _PACKED __attribute__((packed))
#define __aarch64__ 1
#define B_INITIALIZE_SPINLOCK(p) (*(p) = false)
#define FLOW(...)
#define TRACE(...)
#define ERROR(...)
#define T_PORT(...)
#define LO32(value) uint32(value)
#define HI32(value) uint32(uint64(value) >> 32)
#define min_c(a, b) std::min<size_t>((a), (b))

static int barriers, copies, failCopy, submissions, resets, finishes, aborts;
static int held = -1;
static std::vector<int> semaphores;
static status_t completionStatus, resetStatus;
static uint32 completionTFD, completionBytes;
static bool busy, leaveCI, resetChangesCount;
static std::vector<uint8> host(1024 * 1024, 0x63);
static constexpr uint64 physicalBase = UINT64_C(0x235800000);
static int memory_full_barrier() { return ++barriers; }
static sem_id create_sem(int count, const char*)
{
	semaphores.push_back(count); return semaphores.size() - 1;
}
static void delete_sem(int id) { semaphores.at(id) = -1; }
static status_t acquire_sem(int id)
{
	assert(held == -1 && semaphores.at(id) == 1);
	semaphores[id]--; held = id; return B_OK;
}
static status_t acquire_sem_etc(int id, int, int, bigtime_t)
{
	if (semaphores.at(id) == 0) return B_TIMED_OUT;
	semaphores[id]--; return B_OK;
}
static void release_sem(int id)
{
	assert(held == id && semaphores.at(id) == 0);
	semaphores[id]++; held = -1;
}
static int disable_interrupts() { return 0; }
static void restore_interrupts(cpu_status) {}
static void acquire_spinlock(spinlock* lock) { assert(!*lock); *lock = true; }
static void release_spinlock(spinlock* lock) { assert(*lock); *lock = false; }
static status_t wait_until_clear(volatile uint32*, uint32, bigtime_t)
{
	return busy ? B_TIMED_OUT : B_OK;
}
static status_t physical_copy(uint64 address, void* data, size_t size, bool toHost)
{
	assert(address >= physicalBase && address - physicalBase <= host.size());
	assert(size <= host.size() - (address - physicalBase));
	if (++copies == failCopy) return B_BAD_ADDRESS;
	if (toHost) memcpy(host.data() + address - physicalBase, data, size);
	else memcpy(data, host.data() + address - physicalBase, size);
	return B_OK;
}
static status_t vm_memcpy_to_physical(uint64 p, const void* v, size_t n, bool)
{
	return physical_copy(p, const_cast<void*>(v), n, true);
}
static status_t vm_memcpy_from_physical(void* v, uint64 p, size_t n, bool)
{
	return physical_copy(p, v, n, false);
}
#include "definitions.inc"
class AHCIController { public: ahci_hba* fRegs; };
#define private public
#include "ahci_port.h"
#include "sata_request.h"
#undef private

sata_request::sata_request(scsi_ccb* ccb)
	: fCcb(ccb), fFis{}, fIsATAPI(false), fCompletionSem(-1),
	fCompletionStatus(0), fData(nullptr), fDataSize(0) {}
sata_request::~sata_request() {}
void sata_request::Finish(int, size_t n)
{
	assert(held == -1); finishes++; fCompletionStatus = n;
}
void sata_request::Abort() { assert(held == -1); aborts++; }
status_t AHCIPort::PortReset()
{
	resets++;
	if (resetChangesCount) fCommandList->prdbc = UINT32_MAX;
	return resetStatus;
}
status_t AHCIPort::WaitForTransfer(int* tfd, bigtime_t)
{
	assert(held == fRequestSem && fRegs->ci == 1 && barriers > 0);
	submissions++;
	if (fCommandList->prdtl != 0) {
		assert(fPRDTable->dba == LO32(fDMAAddress));
		assert(fPRDTable->dbau == HI32(fDMAAddress));
		assert(fPRDTable->dbc < kAHCIMaxDMATransfer);
		if (!fCommandList->w)
			memset(fDMABuffer, 0xa7, fPRDTable->dbc + 1);
	}
	fCommandList->prdbc = completionBytes;
	fRegs->ci = leaveCI ? 1 : 0;
	*tfd = completionTFD;
	return completionStatus;
}
#include "production.inc"

struct Fixture {
	ahci_hba regs = {};
	AHCIController controller{&regs};
	AHCIPort port{&controller, 0};
	command_list_entry command = {};
	command_table table = {};
	prd entries[PRD_TABLE_ENTRY_COUNT] = {};
	std::vector<uint8> dma = std::vector<uint8>(kAHCIMaxDMATransfer + 2, 0x5d);
	physical_entry sg[3] = {{physicalBase + 3, 1}, {physicalBase + 101, 7},
		{physicalBase + 1024, kAHCIMaxDMATransfer - 8}};
	scsi_ccb ccb;
	sata_request request{&ccb};
	Fixture()
	{
		assert(held == -1);
		barriers = copies = submissions = resets = finishes = aborts = failCopy = 0;
		completionStatus = resetStatus = B_OK;
		completionTFD = 0; completionBytes = 512;
		busy = leaveCI = resetChangesCount = false;
		std::fill(host.begin(), host.end(), 0x63);
		regs.cap = CAP_S64A;
		port.fCommandList = &command; port.fCommandTable = &table;
		port.fPRDTable = entries; port.fDMABuffer = dma.data() + 1;
		port.fDMAAddress = UINT64_C(0x135800000);
		ccb.data_length = 512; ccb.sg_list = sg; ccb.sg_count = 3;
	}
	~Fixture()
	{
		assert(held == -1 && dma.front() == 0x5d && dma.back() == 0x5d);
	}
	void run(bool write = false) { port.ExecuteSataRequest(&request, write); }
	void rejected() { assert(aborts == 1 && finishes == 0 && submissions == 0); }
};

int main()
{
	for (size_t n : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(511),
			size_t(512), size_t(4097), kAHCIMaxDMATransfer}) {
		for (bool write : {false, true}) {
			Fixture f; f.ccb.data_length = n; completionBytes = n; f.run(write);
			assert(finishes == 1 && aborts == 0 && submissions == 1 && resets == 0);
			assert(f.request.fCompletionStatus == int(n));
			assert(f.command.prdtl == (n != 0));
			for (size_t i = 0; i < host.size(); i++) {
				bool changed = !write && ((n > 0 && i == 3)
					|| (n > 1 && i >= 101 && i < 101 + std::min(n - 1, size_t(7)))
					|| (n > 8 && i >= 1024 && i < 1024 + n - 8));
				assert(host[i] == (changed ? 0xa7 : 0x63));
			}
			if (write) {
				for (size_t i = 0; i < n; i++) assert(f.dma[i + 1] == 0x63);
				if (n & 1) assert(f.dma[n + 1] == 0);
			}
		}
	}
	{ Fixture f; f.ccb.data_length++; completionBytes = 7; f.run();
		assert(finishes == 1 && f.request.fCompletionStatus == 7);
		assert(host[106] == 0xa7 && host[107] == 0x63 && host[1024] == 0x63); }
	{ Fixture f; f.ccb.data_length = kAHCIMaxDMATransfer + 1; f.run(); f.rejected(); }
	{ Fixture f; f.ccb.sg_list = nullptr; f.run(); f.rejected(); }
	{ Fixture f; f.ccb.sg_count = -1; f.run(); f.rejected(); }
	{ Fixture f; f.ccb.sg_count = 2; f.run(true); f.rejected(); assert(copies == 0); }
	{ Fixture f; f.sg[1].size = 0; f.run(); f.rejected(); }
	{ Fixture f; f.sg[2].address = UINT64_MAX - 5; f.run(true); f.rejected(); assert(copies == 0); }
	{ Fixture f; failCopy = 2; f.run(true); f.rejected(); }
	{ Fixture f; failCopy = 1; f.run(); assert(aborts == 1 && submissions == 1); }
	{ Fixture f; completionBytes = 513; f.run(); assert(aborts == 1 && resets == 1 && copies == 0); }
	{ Fixture f; leaveCI = true; f.run(); assert(aborts == 1 && resets == 1 && copies == 0); }
	{ Fixture f; completionStatus = B_TIMED_OUT; resetStatus = B_ERROR; f.run();
		assert(aborts == 1 && f.port.fDMAFailed && copies == 0);
		f.run(); assert(aborts == 2 && submissions == 1); }
	{ Fixture f; completionStatus = B_ERROR; completionTFD = ATA_STATUS_ERROR;
		resetChangesCount = true; f.run();
		assert(finishes == 1 && resets == 1 && copies == 0 && f.request.fCompletionStatus == 0); }
	{ Fixture f; busy = true; f.run(); f.rejected(); assert(resets == 1); }
	{ Fixture f; f.port.fPortReset = true; resetStatus = B_ERROR; f.run();
		assert(aborts == 1 && copies == 0 && f.port.fDMAFailed); }
	{ Fixture f; f.request.fIsATAPI = true; f.ccb.cdb_length = 17; f.run(); f.rejected(); }
	{ Fixture f; f.request.fIsATAPI = true; f.ccb.cdb_length = 6; f.run(); assert(finishes == 1); }
	{ Fixture f; f.request.fCcb = nullptr; f.request.fData = host.data() + 11;
		f.request.fDataSize = 512; f.run(); assert(finishes == 1);
		assert(host[10] == 0x63 && host[11] == 0xa7 && host[522] == 0xa7 && host[523] == 0x63); }
	{ Fixture f; f.request.fCcb = nullptr; f.request.fDataSize = 512; f.run(); f.rejected(); }
	{ Fixture f; f.request.fCcb = nullptr; f.request.fDataSize = 512;
		f.request.fData = reinterpret_cast<void*>(UINTPTR_MAX - 7); f.run(); f.rejected(); }
	{ Fixture f; semaphores[f.port.fResponseSem] = 2; f.run();
		assert(semaphores[f.port.fResponseSem] == 0 && finishes == 1); }
	{ Fixture f; bool atapi, sense; uint32 max;
		for (uint32 sector : {0u, 512u, 4096u}) {
			f.port.fSectorSize = sector; f.port.fUse48BitCommands = true;
			f.port.ScsiGetRestrictions(&atapi, &sense, &max);
			assert(max == (sector == 4096 ? 32u : 256u));
		} }
	{ Fixture f; int count; physical_entry sg = {UINT64_C(0x135800000), 0x400002};
		assert(f.port.FillPrdTable(f.entries, &count, 2, &sg, 1, sg.size) == B_OK);
		assert(count == 2 && f.entries[0].dbc == 0x3fffff && f.entries[1].dbc == 1);
		assert(f.port.FillPrdTable(f.entries, &count, 1, &sg, 1, sg.size) != B_OK);
		f.regs.cap = 0;
		assert(f.port.FillPrdTable(f.entries, &count, 2, &sg, 1, sg.size) != B_OK);
		sg = {0x1000, 3}; assert(f.port.FillPrdTable(f.entries, &count, 2, &sg, 1, 3) != B_OK);
		sg = {0x1001, 2}; assert(f.port.FillPrdTable(f.entries, &count, 2, &sg, 1, 2) != B_OK);
		sg = {UINT64_MAX - 1, 4}; f.regs.cap = CAP_S64A;
		assert(f.port.FillPrdTable(f.entries, &count, 2, &sg, 1, 4) != B_OK);
	}
	puts("AHCI DMA ownership, bounds and failure cases passed");
}
