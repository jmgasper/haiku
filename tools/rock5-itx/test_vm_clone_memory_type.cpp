/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

using uint32 = uint32_t;
using addr_t = uintptr_t;
using phys_addr_t = uint64_t;
using area_id = int32_t;
using status_t = int32_t;
using team_id = int32_t;
static const int B_OK = 0, B_NOT_ALLOWED = -1, B_BAD_VALUE = -2,
	B_KERNEL_AREA = 256, B_CLONEABLE_AREA = 512, B_SHARED_AREA = 1024,
	B_WRITE_AREA = 2, B_KERNEL_WRITE_AREA = 32, B_USER_PROTECTION = 7,
	B_FULL_LOCK = 1, CACHE_TYPE_NULL = 0, CACHE_TYPE_RAM = 1, CACHE_TYPE_DEVICE = 2,
	REGION_PRIVATE_MAP = 1, CREATE_AREA_DONT_COMMIT_MEMORY = 1, B_PAGE_SIZE = 4096,
	VM_PRIORITY_SYSTEM = 0, VM_PRIORITY_USER = 1, PAGE_SHIFT = 12;
#define KDEBUG 0
#define ASSERT_ALWAYS(x) assert(x)
#define DEBUG_PAGE_ACCESS_START(x) ((void)(x))
#define DEBUG_PAGE_ACCESS_END(x) ((void)(x))
struct vm_page_reservation {};
static void vm_page_reserve_pages(vm_page_reservation*, size_t, int) {}
static void vm_page_unreserve_pages(vm_page_reservation*) {}
static uint32 sExpectedType;
static unsigned sMappings, sArchSets;
struct VMTranslationMap {
	void Lock() {}
	void Unlock() {}
	void Query(addr_t, phys_addr_t* physical, uint32* protection)
	{ *physical = UINT64_C(0x182300000); *protection = 3; }
	size_t MaxPagesNeededToMap(addr_t, addr_t) { return 3; }
	void Map(addr_t, phys_addr_t, uint32, uint32 memoryType, vm_page_reservation*)
	{ assert(memoryType == sExpectedType); sMappings++; }
};
struct VMAddressSpace {
	VMTranslationMap map;
	VMTranslationMap* TranslationMap() { return &map; }
	static VMAddressSpace* Kernel();
};
static VMAddressSpace sKernel, sUser;
VMAddressSpace* VMAddressSpace::Kernel() { return &sKernel; }
struct VMArea {
	int protection = 3, protection_max = 0, cache_type = CACHE_TYPE_RAM, wiring = B_FULL_LOCK;
	uint32 memoryType = 0;
	uint64_t cache_offset = 0;
	VMAddressSpace* address_space = &sKernel;
	area_id id = 10;
	uint32 MemoryType() { return memoryType; }
	void SetMemoryType(uint32 type) { memoryType = type; }
	addr_t Base() { return address_space == &sKernel ? 0x100000 : 0x200000; }
	size_t Size() { return 8192; }
};
static VMArea sSource, sClone;
struct vm_page { bool busy = false; uint64_t cache_offset; };
struct VMCachePagesTree {
	std::vector<vm_page> pages{{false, 0}, {false, 1}};
	struct Iterator {
		VMCachePagesTree* tree;
		size_t index = 0;
		vm_page* Next() { return index == tree->pages.size() ? NULL : &tree->pages[index++]; }
	};
	Iterator GetIterator() { return Iterator{this}; }
};
struct VMCache {
	VMCachePagesTree pages;
	int references = 1;
	void AcquireRefLocked() { references++; }
};
static VMCache sCache;
struct AddressSpaceWriteLocker {
	status_t SetFromArea(area_id id, VMArea*& area)
	{ assert(id == 10); area = &sSource; return B_OK; }
};
struct MultiAddressSpaceLocker {
	status_t AddArea(area_id id, bool write, VMAddressSpace** space)
	{ assert(id == 10 && !write); *space = &sKernel; return B_OK; }
	status_t AddTeam(team_id team, bool write, VMAddressSpace** space)
	{ assert(team == 42 && write); *space = &sUser; return B_OK; }
	status_t Lock() { return B_OK; }
};
static VMArea* lookup_area(VMAddressSpace* space, area_id id)
{ assert(space == &sKernel && id == 10); return &sSource; }
struct AreaCacheLocker {
	explicit AreaCacheLocker(VMArea* area) { assert(area == &sSource); }
	VMCache* Get() { return &sCache; }
};
static status_t check_protection(team_id team, uint32*) { assert(team == 42); return B_OK; }
struct virtual_address_restrictions { void* address; uint32 address_specification; };
static status_t vm_map_cache(VMAddressSpace* space, VMCache* cache, uint64_t offset,
	const char*, size_t bytes, int wiring, uint32 protection, int, uint32 mapping,
	uint32 flags, virtual_address_restrictions*, bool kernel, VMArea** area, void** address)
{
	assert(space == &sUser && cache == &sCache && offset == 0 && bytes == 8192);
	assert(kernel && flags == (mapping == REGION_PRIVATE_MAP ? 0u : 1u));
	sClone = VMArea{};
	sClone.address_space = space;
	sClone.protection = protection;
	sClone.wiring = wiring;
	sClone.id = 11;
	*area = &sClone; *address = (void*)sClone.Base();
	return B_OK;
}
static status_t arch_vm_set_memory_type(VMArea* area, phys_addr_t, uint32 type, void*)
{ assert(area == &sClone && type == sExpectedType); sArchSets++; return B_OK; }
static void map_page(VMArea* area, vm_page* page, addr_t address, uint32 protection,
	vm_page_reservation* reservation)
{
	assert(area == &sClone);
	area->address_space->TranslationMap()->Map(address,
		UINT64_C(0x182300000) + page->cache_offset * 8192,
		protection, area->MemoryType(), reservation);
}

#include "clone.inc"

int main()
{
	// These are memory-type tokens: the fixture checks propagation, not an
	// emulation of ARM MAIR or device cache coherence.
	for (int cache : {CACHE_TYPE_RAM, CACHE_TYPE_DEVICE}) {
		for (uint32 type : {0u, 0x10000000u, 0x20000000u}) {
			for (int wiring : {B_FULL_LOCK, 0}) {
				sSource = VMArea{}; sSource.cache_type = cache;
				sSource.memoryType = type; sSource.wiring = wiring;
				sExpectedType = type; sMappings = sArchSets = 0;
				sCache.references = 1;
				void* address = NULL;
				assert(vm_clone_area(42, "clone", &address, 0, 3, 0, 10, true) == 11);
				assert(sClone.MemoryType() == type && sClone.cache_type == cache);
				assert(sMappings == (wiring == B_FULL_LOCK ? 2u : 0u));
				assert(sArchSets == (cache == CACHE_TYPE_DEVICE && wiring == B_FULL_LOCK && type != 0 ? 1u : 0u));
				assert(sCache.references == 2);
			}
		}
	}
	sSource = VMArea{}; sSource.protection = B_KERNEL_AREA;
	void* address = NULL;
	assert(vm_clone_area(42, "clone", &address, 0, 3, 0, 10, false) == B_NOT_ALLOWED);
	sSource = VMArea{}; sSource.cache_type = CACHE_TYPE_NULL;
	assert(vm_clone_area(42, "clone", &address, 0, 3, 0, 10, true) == B_NOT_ALLOWED);
	puts("VM_CLONE_MEMORY_TYPE_TEST_PASS");
}
