// Compile the production contiguous allocator with a bounded VM substitute.
// This verifies allocation policy and lifetime, not physical hardware coherence.
#include "dma_host/host.h"
#include "dma_host/arch/arm64/cache_poc.h"
#include <algorithm>
#include <cassert>
#include <map>
#include <stdexcept>
#include <vector>

using area_id = int;
using status_t = int;
using spinlock = int;
constexpr int B_SPINLOCK_INITIALIZER = 0, B_OK = 0, B_SYSTEM_TEAM = 1;
constexpr int B_CONTIGUOUS = 1, B_KERNEL_READ_AREA = 1, B_KERNEL_WRITE_AREA = 2;
constexpr int B_OS_NAME_LENGTH = 32, CREATE_AREA_DONT_CLEAR = 1;
constexpr int CREATE_AREA_DONT_WAIT = 2, HEAP_DONT_WAIT_FOR_MEMORY = 1;
constexpr int B_WRITE_COMBINING_MEMORY = 3;
#define B_PRIx32 PRIx32
#define ROUNDUP(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
struct virtual_address_restrictions {};
struct physical_address_restrictions {
	uint64_t low_address, high_address, alignment, boundary;
};
struct physical_entry { uint64_t address, size; };
struct Area {
	void* cpu;
	size_t size;
	uint64_t physical;
	bool cached = true;
};
static std::map<int, Area> sAreas;
static int sAreaID = 1, sFailure = -1;
static unsigned sIRQDepth, sTypeChanges, sCacheOps, sPending, sVMCalls;
static size_t sLineSize = 64;
static uint64_t sNextPhysical = 0x100000000;
static bool sForbidVM;

extern "C" cpu_status disable_interrupts() { return sIRQDepth++; }
extern "C" void restore_interrupts(cpu_status state)
{
	assert(sIRQDepth == state + 1 && sPending == 0);
	sIRQDepth = state;
}
namespace BPrivate {
struct InterruptsSpinLocker {
	spinlock* lock;
	cpu_status state;
	explicit InterruptsSpinLocker(spinlock* p) : lock(p), state(disable_interrupts())
		{ assert(*lock == 0); *lock = 1; }
	~InterruptsSpinLocker() { *lock = 0; restore_interrupts(state); }
};
}
static size_t next_power_of_2(size_t size)
{
	size_t result = 1;
	while (result < size) result *= 2;
	return result;
}
static void* memalign_etc(size_t alignment, size_t size, int)
{
	if (sFailure == 0) return nullptr;
	void* address;
	assert(posix_memalign(&address, std::max(alignment, sizeof(void*)), size) == 0);
	return address;
}
static area_id create_area_etc(int team, const char*, size_t size, int wiring,
	int protection, uint32 flags, int, virtual_address_restrictions*,
	physical_address_restrictions* restriction, void** address)
{
	assert(!sForbidVM && sIRQDepth == 0);
	++sVMCalls;
	assert(team == B_SYSTEM_TEAM && wiring == B_CONTIGUOUS);
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA));
	assert(size % B_PAGE_SIZE == 0 && restriction->low_address % B_PAGE_SIZE == 0);
	if (sFailure == 1) return -1;
	uint64_t alignment = std::max(uint64_t(B_PAGE_SIZE), restriction->alignment);
	uint64_t physical = ROUNDUP(std::max(sNextPhysical, restriction->low_address), alignment);
	if (restriction->boundary && physical / restriction->boundary
		!= (physical + size - 1) / restriction->boundary)
		physical = ROUNDUP(physical, restriction->boundary);
	if (restriction->high_address && (physical >= restriction->high_address
		|| size > restriction->high_address - physical)) return -1;
	*address = aligned_alloc(B_PAGE_SIZE, size);
	assert(*address);
	memset(*address, flags & CREATE_AREA_DONT_CLEAR ? 0xd3 : 0, size);
	sAreas.emplace(sAreaID, Area{*address, size, physical});
	return sAreaID++;
}
static status_t delete_area(area_id area)
{
	assert(!sForbidVM && sIRQDepth == 0 && sPending == 0);
	++sVMCalls;
	free(sAreas.at(area).cpu);
	assert(sAreas.erase(area) == 1);
	return B_OK;
}
static area_id area_for(void* address)
{
	assert(!sForbidVM);
	for (auto& item : sAreas) if (item.second.cpu == address) return item.first;
	throw std::runtime_error("unknown area");
}
static status_t get_memory_map(void* address, size_t size, physical_entry* entry, int count)
{
	assert(!sForbidVM && sIRQDepth == 0 && count == 1);
	++sVMCalls;
	if (sFailure == 2) return -1;
	for (const auto& item : sAreas) {
		const auto& area = item.second;
		uintptr_t offset = (uintptr_t)address - (uintptr_t)area.cpu;
		if ((uintptr_t)address >= (uintptr_t)area.cpu && offset < area.size
			&& size <= area.size - offset) {
			*entry = {area.physical + offset, sFailure == 3 ? size - 1 : area.size - offset};
			return B_OK;
		}
	}
	return -1;
}
uint64_t arm64_current_data_cache_line_size()
	{ assert(sIRQDepth != 0); return sLineSize; }
void arm64_clean_invalidate_data_cache_line_poc(uintptr_t address)
{
	assert(sIRQDepth && address % sLineSize == 0);
	bool found = false;
	for (const auto& item : sAreas) {
		const auto& area = item.second;
		if (address >= (uintptr_t)area.cpu && address - (uintptr_t)area.cpu < area.size) {
			assert(area.cached && address - (uintptr_t)area.cpu + sLineSize <= area.size);
			found = true;
		}
	}
	assert(found);
	++sCacheOps; ++sPending;
}
extern "C" void memory_full_barrier() { sPending = 0; }
static status_t vm_set_area_memory_type(area_id area, phys_addr_t physical, int type)
{
	assert(!sForbidVM && sIRQDepth == 0 && sPending == 0);
	++sVMCalls; ++sTypeChanges;
	auto& allocation = sAreas.at(area);
	assert(type == B_WRITE_COMBINING_MEMORY && allocation.physical == physical);
	assert(sCacheOps == allocation.size / sLineSize);
	if (sFailure == 4) return -1;
	allocation.cached = false;
	return B_OK;
}
extern "C" void panic(const char* message, ...) { throw std::runtime_error(message); }

#include "fbsd-contigmalloc.inc"

static void* allocate(size_t size, bool cached, uint64_t low = 0,
	uint64_t high = UINT64_MAX, size_t alignment = 1, size_t boundary = 0)
{
	sCacheOps = sTypeChanges = 0;
	return _kernel_contigmalloc_etc(__FILE__, __LINE__, size, M_ZERO | M_NOWAIT,
		low, high, alignment, boundary, cached);
}
int main()
{
	for (bool cached : {false, true}) {
		for (size_t size : {1, 63, 64, 65, 4095, 4096, 4097, 9001, 16384}) {
			void* memory = allocate(size, cached);
			assert(memory && (uintptr_t)memory % B_PAGE_SIZE == 0);
			auto& area = sAreas.at(area_for(memory));
			assert(area.size == ROUNDUP(size, B_PAGE_SIZE));
			assert(std::all_of((char*)memory, (char*)memory + size, [](char v) { return v == 0; }));
#ifdef FBSD_NONCOHERENT_DMA
			assert(area.cached == cached);
			assert(sTypeChanges == (cached ? 0u : 1u));
			assert(sCacheOps == (cached ? 0u : area.size / sLineSize));
			unsigned vmCalls = sVMCalls;
			sForbidVM = true;
			for (size_t offset : {size_t(0), size - 1}) {
				uint64_t physical = 0x1234;
				bool found = _kernel_contig_dma_address((char*)memory + offset, size - offset, &physical);
				assert(found == !cached);
				assert(physical == (cached ? 0x1234 : area.physical + offset));
				assert(!_kernel_contig_dma_address((char*)memory + offset, size - offset + 1, &physical));
			}
			uint64_t physical;
			assert(!_kernel_contig_dma_address((char*)memory + size, 1, &physical));
			assert(!_kernel_contig_dma_address(memory, 0, &physical));
			assert(!_kernel_contig_dma_address(nullptr, 1, &physical));
			assert(sVMCalls == vmCalls);
			sForbidVM = false;
#else
			assert(area.cached && sTypeChanges == 0 && sCacheOps == 0);
#endif
			_kernel_contigfree(memory, size);
			assert(sAreas.empty());
		}
		for (int failure = 0; failure <= 4; ++failure) {
#ifdef FBSD_NONCOHERENT_DMA
			if (cached && failure == 4) continue;
#else
			if (failure != 1) continue;
#endif
			sFailure = failure;
			assert(allocate(9001, cached) == nullptr);
			assert(sAreas.empty() && sIRQDepth == 0 && sPending == 0);
		}
		sFailure = -1;
		assert(!allocate(0, cached));
		assert(!allocate(SIZE_MAX, cached));
		assert(!allocate(4096, cached, 8192, 4096));
		assert(!allocate(4096, cached, 0, UINT64_MAX, 0));
		assert(!allocate(4096, cached, 0, UINT64_MAX, 3));
		assert(!allocate(4096, cached, 0, UINT64_MAX, 1, 4000));
		assert(!allocate(4097, cached, 0, UINT64_MAX, 1, 4096));
		assert(!allocate(4096, cached, UINT64_MAX - 1023));
		assert(!allocate(4096, cached, 4097, 8191));
		sNextPhysical = 0;
		void* memory = allocate(4096, cached, 4097, 12287);
		assert(memory && sAreas.at(area_for(memory)).physical == 8192);
		_kernel_contigfree(memory, 4096);
		sNextPhysical = 0x100000000;
		assert(!allocate(4096, cached, 0, 0xffffffff));
	}
	sCacheOps = sTypeChanges = 0;
	void* original = _kernel_contigmalloc(__FILE__, __LINE__, 4096, 0, 0,
		UINT64_MAX, 1, 0);
	assert(original);
#ifdef FBSD_NONCOHERENT_DMA
	assert(!sAreas.at(area_for(original)).cached);
#endif
	_kernel_contigfree(original, 4096);
	_kernel_contigfree(nullptr, 0);
#ifdef FBSD_NONCOHERENT_DMA
	for (bool cached : {false, true}) {
		sLineSize = 8192;
		assert(!allocate(4096, cached));
		assert(sAreas.empty() && sIRQDepth == 0 && sPending == 0);
	}
	assert(sDMAAllocations == nullptr);
#endif
	assert(sAreas.empty());
	puts("fbsd contigmalloc: cached/coherent policy, bounds, failures and cleanup passed");
}
