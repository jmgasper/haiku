/* Host architectural substitutes for the production ARM64 ASID switch path. */
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using uint64 = uint64_t;
using phys_addr_t = uint64_t;
using spinlock = int;
#define B_SPINLOCK_INITIALIZER 0
#define B_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))
#define TRACE(...) do {} while (0)
#define ASSERT(condition) do { if (!(condition)) panic("production assertion"); } while (0)
static int sLockDepth, sPinDepth;
static unsigned sCPU;
static constexpr unsigned kCPUs = 8;
static constexpr uint64_t kVA = 0x400000;
static uint64_t sEmptyTable = 0x1000;
static std::array<uint64_t, kCPUs> sTTBR, sPendingTTBR;
static std::array<bool, kCPUs> sWritePending;
static std::array<std::map<std::pair<unsigned, uint64_t>, uint64_t>, kCPUs> sTLB;
static std::map<uint64_t, uint64_t> sTables;
static std::vector<unsigned> sPendingInvalidations, sCompletedInvalidations;
static bool sStoreBarrier;

struct InterruptsSpinLocker {
	explicit InterruptsSpinLocker(spinlock&) { assert(sLockDepth++ == 0); }
	~InterruptsSpinLocker() { assert(--sLockDepth == 0); }
};
struct ThreadCPUPinner {
	explicit ThreadCPUPinner(void*) { assert(sPinDepth++ == 0); }
	~ThreadCPUPinner() { assert(--sPinDepth == 0); }
};
static void* thread_get_current_thread() { return nullptr; }
struct vm_page_reservation {};
static void vm_page_unreserve_pages(vm_page_reservation*) {}
[[noreturn]] static void panic(const char* reason) { throw std::runtime_error(reason); }

static void WriteTTBR(uint64_t value)
{
	assert(sLockDepth == 1);
	sPendingTTBR[sCPU] = value;
	sWritePending[sCPU] = true;
}
static void ISB()
{
	assert(sPendingInvalidations.empty());
	if (sWritePending[sCPU]) {
		sTTBR[sCPU] = sPendingTTBR[sCPU];
		sWritePending[sCPU] = false;
	}
}
static void DSB(const char* domain)
{
	if (strcmp(domain, "ishst") == 0) {
		sStoreBarrier = true;
		return;
	}
	assert(strcmp(domain, "ish") == 0 && sStoreBarrier);
	for (unsigned asid : sPendingInvalidations) {
		for (auto& cpu : sTLB) {
			for (auto i = cpu.begin(); i != cpu.end();) {
				if (i->first.first == asid) i = cpu.erase(i);
				else ++i;
			}
		}
		sCompletedInvalidations.push_back(asid);
	}
	sPendingInvalidations.clear();
	sStoreBarrier = false;
}
static void Invalidate(uint64_t operand)
{
	assert(sLockDepth == 1 && sStoreBarrier);
	assert((operand & ((uint64_t(1) << 48) - 1)) == 0);
	assert((operand >> 48) < 256);
	sPendingInvalidations.push_back(operand >> 48);
}
static uint64_t Lookup()
{
	assert(!sWritePending[sCPU] && sPendingInvalidations.empty());
	unsigned asid = (sTTBR[sCPU] >> 48) & 255;
	auto key = std::make_pair(asid, kVA);
	auto hit = sTLB[sCPU].find(key);
	if (hit != sTLB[sCPU].end()) return hit->second;
	uint64_t table = sTTBR[sCPU] & 0x0000fffffffff000ULL;
	auto page = sTables.find(table);
	if (page == sTables.end()) return 0;
	return sTLB[sCPU][key] = page->second;
}

struct VMSAv8TranslationMap {
	bool fIsKernel;
	phys_addr_t fPageTable;
	int fPageBits, fVaBits, fMinBlockLevel, fInitialLevel, fASID, fRefcount;
	static uint32_t fHwFeature;
	static constexpr unsigned HW_COMMON_NOT_PRIVATE = 4;
	VMSAv8TranslationMap(bool, phys_addr_t, int, int, int);
	~VMSAv8TranslationMap();
	static void SwitchUserMap(VMSAv8TranslationMap*, VMSAv8TranslationMap*);
	static int CalcStartLevel(int, int);
	void FreeTable(phys_addr_t table, uint64_t, int, vm_page_reservation*)
	{
		assert(sLockDepth == 0 && sPinDepth == 0);
		sTables.erase(table);
	}
};
uint32_t VMSAv8TranslationMap::fHwFeature;
#define WRITE_SPECIALREG(reg, value) WriteTTBR(value)
#define arm64_isb() ISB()
#define arm64_dsb(domain) DSB(#domain)
/* Only the TLBI and ISB assembly statements are replaced by the extractor. */
#include "asid_production.inc"

using Map = VMSAv8TranslationMap;
alignas(Map) static uint8_t sKernelStorage[sizeof(Map)];
static Map* sKernel;
static std::array<Map*, kCPUs> sCurrent;
static std::array<uint64_t, 4> sInitialBits;

static void Switch(unsigned cpu, Map* to)
{
	sCPU = cpu;
	Map::SwitchUserMap(sCurrent[cpu], to);
	sCurrent[cpu] = to;
	assert(sLockDepth == 0 && !sWritePending[cpu]);
}
static std::unique_ptr<Map> NewMap(unsigned index)
{
	uint64_t table = 0x10000 + uint64_t(index) * 0x1000;
	sTables[table] = 0xa0000000 + index;
	return std::make_unique<Map>(false, table, 12, 48, 1);
}
static void Reset(bool cnp)
{
	assert(sLockDepth == 0 && sPinDepth == 0);
	for (unsigned i = 0; i < 4; ++i) sAsidBitMap[i] = sInitialBits[i];
	for (auto& p : sAsidMapping) p = nullptr;
	sTables.clear();
	for (auto& cache : sTLB) cache.clear();
	sTTBR.fill(sEmptyTable);
	sPendingTTBR.fill(0);
	sWritePending.fill(false);
	sCurrent.fill(sKernel);
	sPendingInvalidations.clear();
	sCompletedInvalidations.clear();
	sStoreBarrier = false;
	Map::fHwFeature = cnp ? Map::HW_COMMON_NOT_PRIVATE : 0;
}
static void EmptySeparation()
{
	auto map = NewMap(1);
	Switch(0, map.get());
	assert(Lookup() == 0xa0000001);
	int asid = map->fASID;
	size_t flushes = sCompletedInvalidations.size();
	Switch(0, sKernel);
	uint64_t value = Lookup();
	printf("empty-table switch: user ASID=%d lookup=%#llx\n", asid,
		(unsigned long long)value);
	fflush(stdout);
	assert(value == 0 && "empty table retained a stale user translation");
	assert(asid > 0 && map->fRefcount == 0);
	Switch(0, map.get());
	assert(Lookup() == 0xa0000001 && map->fASID == asid);
	assert(sCompletedInvalidations.size() == flushes);
	Switch(0, sKernel);
}
static void Exhaustion()
{
	std::vector<std::unique_ptr<Map>> maps;
	for (unsigned i = 0; i < 255; ++i) {
		maps.push_back(NewMap(i + 1));
		Switch(0, maps.back().get());
		assert(maps.back()->fASID == int(i + 1));
		assert(Lookup() == 0xa0000001 + i);
		Switch(0, sKernel);
	}
	assert(alloc_first_free_asid() == 256);
	/* Retain active maps on seven other CPUs while CPU 0 recycles idle maps. */
	for (unsigned cpu = 1; cpu < kCPUs; ++cpu) {
		Switch(cpu, maps[cpu - 1].get());
		assert(Lookup() == 0xa0000000 + cpu);
	}
	for (unsigned i = 255; i < 280; ++i) maps.push_back(NewMap(i + 1));
	for (unsigned pass = 0; pass < 3; ++pass) {
		for (unsigned i = 7; i < maps.size(); ++i) {
			Switch(0, maps[i].get());
			assert(Lookup() == 0xa0000001 + i);
			assert(maps[i]->fASID > 0 && maps[i]->fRefcount == 1);
			for (unsigned cpu = 1; cpu < kCPUs; ++cpu) {
				assert(maps[cpu - 1]->fASID == int(cpu));
				assert(maps[cpu - 1]->fRefcount == 1);
				sCPU = cpu;
				assert(Lookup() == 0xa0000000 + cpu);
			}
		}
	}
	for (unsigned cpu = 0; cpu < kCPUs; ++cpu) Switch(cpu, sKernel);
	for (const auto& map : maps) assert(map->fRefcount == 0);
	maps.clear();
	assert(sAsidBitMap[0] == 1);
	for (unsigned i = 1; i < 4; ++i) assert(sAsidBitMap[i] == 0);
	for (auto p : sAsidMapping) assert(p == nullptr);
	/* Reusing a released ID must broadcast a flush before the new lookup. */
	for (unsigned i = 1; i <= 64; ++i) {
		auto map = NewMap(1000 + i);
		Switch(0, map.get());
		assert(map->fASID == 1 && Lookup() == 0xa0000000 + 1000 + i);
		/* Other CPUs retain an inactive entry for this reused ASID. */
		for (unsigned cpu = 1; cpu < kCPUs; ++cpu) {
			Switch(cpu, map.get());
			assert(Lookup() == 0xa0000000 + 1000 + i);
		}
		assert(map->fRefcount == int(kCPUs));
		for (unsigned cpu = 0; cpu < kCPUs; ++cpu) Switch(cpu, sKernel);
		assert(map->fRefcount == 0);
	}
}
static void ReservedFree()
{
	bool rejected = false;
	try { free_asid(0); } catch (const std::exception&) { rejected = true; }
	assert(rejected && sAsidBitMap[0] == 1);
}
int main(int argc, char** argv)
{
	assert(argc == 2);
	sKernel = new (sKernelStorage) Map(true, sEmptyTable, 12, 48, 1);
	for (unsigned i = 0; i < 4; ++i) sInitialBits[i] = sAsidBitMap[i];
	for (bool cnp : {false, true}) {
		Reset(cnp);
		if (strcmp(argv[1], "empty") == 0) EmptySeparation();
		else if (strcmp(argv[1], "exhaustion") == 0) Exhaustion();
		else if (strcmp(argv[1], "reserved-free") == 0) ReservedFree();
		else assert(false);
	}
	puts("ARM64 production ASID case passed with CnP off and on");
}
