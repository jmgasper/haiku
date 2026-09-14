/* Architectural substitutes and an independent page/mapping lifetime oracle. */
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using uint64 = uint64_t;
using int64 = int64_t;
using uint32 = uint32_t;
using addr_t = uint64_t;
using phys_addr_t = uint64_t;
using status_t = int;
static constexpr size_t B_PAGE_SIZE = 4096;
static constexpr status_t B_OK = 0, B_ENTRY_NOT_FOUND = -1;
static constexpr uint32 PAGE_PRESENT = 1, PAGE_ACCESSED = 2, PAGE_MODIFIED = 4;
#define TRACE(...) do {} while (0)
#define ASSERT(value) assert(value)
#include "aging_constants.inc"

static int sMapLock, sAsidLock, sPinDepth;
static bool sStoreBarrier, sPendingFlush;
static bool sAccessOnExchange;
static unsigned sInvalidations;
static std::array<bool, 8> sTLB;
static constexpr addr_t kAddress = 0x818be04000;
static constexpr phys_addr_t kPhysical = 0xa833000;

struct RecursiveLocker {
	int& depth;
	bool held = true;
	explicit RecursiveLocker(int& value) : depth(value) { assert(++depth == 1); }
	~RecursiveLocker() { if (held) assert(--depth == 0); }
	void Detach() { held = false; }
};
struct InterruptsSpinLocker {
	int& depth;
	explicit InterruptsSpinLocker(int& value) : depth(value) { assert(++depth == 1); }
	~InterruptsSpinLocker() { assert(--depth == 0); }
};
struct ThreadCPUPinner {
	bool held = true;
	explicit ThreadCPUPinner(void*) { assert(++sPinDepth == 1); }
	~ThreadCPUPinner() { if (held) Unlock(); }
	void Unlock() { assert(held && --sPinDepth == 0); held = false; }
};
static void* thread_get_current_thread() { return nullptr; }
static void DSB(const char* kind)
{
	if (strcmp(kind, "ishst") == 0) sStoreBarrier = true;
	else {
		assert(strcmp(kind, "ish") == 0 && sStoreBarrier);
		if (sPendingFlush) sTLB.fill(false);
		sPendingFlush = false;
		sStoreBarrier = false;
	}
}
static void ISB() { assert(!sPendingFlush); }
static void Invalidate(bool global, uint64_t operand)
{
	assert(sAsidLock == 1 && sStoreBarrier);
	assert((operand & ((uint64_t(1) << 48) - 1)) == (kAddress >> 12));
	assert((operand >> 48) == (global ? 0 : 7));
	sPendingFlush = true;
	++sInvalidations;
}
#define arm64_dsb(kind) DSB(#kind)
#define arm64_isb() ISB()
static int64 atomic_get64(int64* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static int64 atomic_get_and_set64(int64* p, int64 value)
{
	return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
}
static int64 atomic_test_and_set64(int64* p, int64 value, int64 expected)
{
	if (sAccessOnExchange) {
		sAccessOnExchange = false;
		__atomic_fetch_or(p, kAttrAF | kAttrSWDIRTY, __ATOMIC_SEQ_CST);
	}
	__atomic_compare_exchange_n(p, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}
struct VMArea {};
struct VMSAv8TranslationMap {
	int& fLock = sMapLock;
	int fASID = 7, fPageBits = 12, fInitialLevel = 0;
	phys_addr_t fPageTable = 0x1000;
	uint64_t pte = 0;
	bool tablePresent = true, softwareMapping = false;
	unsigned agingUnmaps = 0, cowUnmaps = 0;
	bool ValidateVa(addr_t address) { return address == kAddress; }
	template<class Callback>
	void ProcessRange(phys_addr_t, int, addr_t address, size_t size, void*, Callback&& callback)
	{
		assert(sMapLock == 1 && sPinDepth == 1 && size == B_PAGE_SIZE);
		if (tablePresent) callback(&pte, address);
	}
	bool FlushVAIfAccessed(uint64_t, addr_t);
	bool ClearAccessedAndModified(VMArea*, addr_t, bool, bool&);
	status_t UnmapPage(VMArea*, addr_t, bool, bool, uint32*);
	void UnaccessedPageUnmapped(VMArea*, uint64_t page)
	{
		assert(sMapLock == 1);
		assert(page == kPhysical / B_PAGE_SIZE && softwareMapping);
		softwareMapping = false;
		++agingUnmaps;
		--sMapLock;
	}
	void PageUnmapped(VMArea*, uint64_t page, bool, bool, bool)
	{
		assert(sMapLock == 1 && page == kPhysical / B_PAGE_SIZE);
		if (!softwareMapping)
			throw std::runtime_error("PageUnmapped has no matching page-area record");
		softwareMapping = false;
		++cowUnmaps;
		--sMapLock;
	}
};
#include "aging_production.inc"

static void AccessedCase(int asid, bool global, bool unmap, bool concurrentAccess = false)
{
	VMSAv8TranslationMap map;
	map.fASID = asid;
	map.pte = kPhysical | kPteTypeL3Page | (concurrentAccess ? 0 : kAttrAF) | kAttrSWDIRTY
		| kAttrAPReadOnly | (global ? 0 : kAttrNG);
	map.softwareMapping = true;
	sInvalidations = 0;
	sStoreBarrier = sPendingFlush = false;
	sAccessOnExchange = concurrentAccess;
	sTLB.fill(global || asid != -1);
	VMArea area;
	bool modified = false;
	bool accessed = map.ClearAccessedAndModified(&area, kAddress, unmap, modified);
	assert(!sAccessOnExchange);
	printf("accessed page: ASID=%d global=%d unmap=%d returned=%d PTE=%#llx software=%d\n",
		asid, global, unmap, accessed, (unsigned long long)map.pte, map.softwareMapping);
	fflush(stdout);
	// The page was recently accessed, so it must still have both representations.
	// Try a later COW unmap as the native shell did after the sleeping interval.
	map.UnmapPage(&area, kAddress, true, false, nullptr);
	assert(accessed && modified && map.agingUnmaps == 0 && map.cowUnmaps == 1);
	assert(map.pte == 0 && !map.softwareMapping && sMapLock == 0);
	assert(sInvalidations == unsigned(global || asid != -1));
	for (bool cached : sTLB) assert(!cached);
}

static void UnaccessedCases()
{
	for (bool unmap : {false, true}) {
		VMSAv8TranslationMap map;
		map.pte = kPhysical | kPteTypeL3Page | kAttrNG | kAttrAPReadOnly;
		map.softwareMapping = true;
		VMArea area;
		bool modified = true;
		assert(!map.ClearAccessedAndModified(&area, kAddress, unmap, modified));
		assert(!modified && map.softwareMapping == !unmap);
		assert((map.pte != 0) == !unmap && map.agingUnmaps == unsigned(unmap));
	}
	for (bool table : {false, true}) {
		for (bool unmap : {false, true}) {
			VMSAv8TranslationMap map;
			map.tablePresent = table;
			VMArea area;
			bool modified = true;
			assert(!map.ClearAccessedAndModified(&area, kAddress, unmap, modified));
			assert(!modified && map.pte == 0 && map.agingUnmaps == 0 && !map.softwareMapping);
		}
	}
}

int main(int argc, char** argv)
{
	assert(argc == 2);
	try {
		std::string name = argv[1];
		if (name == "evicted") {
			AccessedCase(-1, false, true);
			AccessedCase(-1, false, false);
		} else if (name == "mapped") {
			for (bool unmap : {true, false}) {
				AccessedCase(7, false, unmap);
				AccessedCase(0, true, unmap);
			}
		} else if (name == "concurrent-access") {
			AccessedCase(7, false, true, true);
		} else {
			assert(name == "unaccessed");
			UnaccessedCases();
		}
		assert(sMapLock == 0 && sAsidLock == 0 && sPinDepth == 0);
		puts("ARM64 production page aging case passed");
		return 0;
	} catch (const std::exception& error) {
		fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
