/* Exercise actual initialization against dirty pages, not zero-filled QEMU RAM. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <setjmp.h>
#include <sys/mman.h>

static constexpr size_t B_PAGE_SIZE = 4096;
static constexpr unsigned PAGE_SHIFT = 12;
static uint8_t* sMemory;
static uintptr_t KERNEL_PMAP_BASE;
static uint64_t sEmptyTable, sPhysical;
static bool sFailAllocation, sStoreBarrier, sInstalled, sInstructionBarrier;
static jmp_buf sPanic;

static uint64_t
vm_allocate_early_physical_page(void*)
{
	return sFailAllocation ? 0 : sPhysical >> PAGE_SHIFT;
}

[[noreturn]] static void
panic(const char*)
{
	longjmp(sPanic, 1);
}

static void
StoreBarrier()
{
	assert(!sInstalled);
	for (size_t i = 0; i < B_PAGE_SIZE; i++)
		assert(sMemory[B_PAGE_SIZE + i] == 0);
	sStoreBarrier = true;
}

static void
Install(uint64_t value)
{
	assert(sStoreBarrier && value == sPhysical);
	sInstalled = true;
}

static void
InstructionBarrier(const char* instruction)
{
	assert(sInstalled && strcmp(instruction, "isb") == 0);
	sInstructionBarrier = true;
}

#define WRITE_SPECIALREG(reg, value) Install(value)
#define arm64_dsb(limit) StoreBarrier()
#define asm(instruction) InstructionBarrier(instruction)
#include "empty_table.inc"
#undef asm

int
main()
{
	sMemory = (uint8_t*)mmap(nullptr, 5 * B_PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(sMemory != MAP_FAILED);
	assert(mprotect(sMemory, B_PAGE_SIZE, PROT_NONE) == 0);
	assert(mprotect(sMemory + 4 * B_PAGE_SIZE, B_PAGE_SIZE, PROT_NONE) == 0);
	for (uint64_t physical : {uint64_t(0x41000000), uint64_t(0x123400000)}) {
		sPhysical = physical;
		KERNEL_PMAP_BASE = (uintptr_t)sMemory + B_PAGE_SIZE - physical;
		memset(sMemory + B_PAGE_SIZE, 0xff, B_PAGE_SIZE);
		memset(sMemory + 2 * B_PAGE_SIZE, 0xa5, 2 * B_PAGE_SIZE);
		sFailAllocation = sStoreBarrier = sInstalled = sInstructionBarrier = false;
		Initialize(nullptr);
		assert(sStoreBarrier && !sInstalled);
		arch_vm_install_empty_table_ttbr0();
		assert(sInstalled && sInstructionBarrier);
		for (size_t i = 2 * B_PAGE_SIZE; i < 4 * B_PAGE_SIZE; i++)
			assert(sMemory[i] == 0xa5);
	}
	sFailAllocation = true;
	sStoreBarrier = sInstalled = sInstructionBarrier = false;
	memset(sMemory + B_PAGE_SIZE, 0xa5, B_PAGE_SIZE);
	if (setjmp(sPanic) == 0) {
		Initialize(nullptr);
		assert(false);
	}
	assert(!sStoreBarrier && !sInstalled);
	for (size_t i = B_PAGE_SIZE; i < 4 * B_PAGE_SIZE; i++)
		assert(sMemory[i] == 0xa5);
	assert(munmap(sMemory, 5 * B_PAGE_SIZE) == 0);
	puts("ARM64 empty table: dirty memory, publication and allocation failure passed");
}
