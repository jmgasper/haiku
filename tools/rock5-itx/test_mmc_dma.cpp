/* Production ARM64 allocator with observable VM and cache-maintenance boundaries. */
#include "test_mmc_stubs.h"

static constexpr size_t kSdhciDmaSize = 512 * 1024;
static constexpr int B_SYSTEM_TEAM = 1, B_CONTIGUOUS = 2, B_KERNEL_READ_AREA = 4,
    B_KERNEL_WRITE_AREA = 8, B_WRITE_COMBINING_MEMORY = 16;
struct virtual_address_restrictions { uint64 address, address_specification, alignment; };
struct physical_address_restrictions { uint64 low_address, high_address, alignment, boundary; };
struct physical_entry { uint64 address, size; };
static std::array<uint8, kSdhciDmaSize> ram;
static std::string fault;
static int allocated, freed, evicted, converted, barriers;
static physical_entry mapping;
static area_id create_area_etc(int team, const char*, size_t size, int lock,
    int protection, int flags, int cache, virtual_address_restrictions* vr,
    physical_address_restrictions* pr, void** out)
{
    assert(team == B_SYSTEM_TEAM && size == kSdhciDmaSize && lock == B_CONTIGUOUS);
    assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA) && !flags && !cache);
    assert(!vr->address && !vr->address_specification && !vr->alignment && !pr->low_address);
    assert(pr->high_address == UINT64_C(0x100000000));
    assert(pr->alignment == size && pr->boundary == size);
    if (fault == "allocate") return B_NO_MEMORY;
    allocated++; *out = ram.data(); return 71;
}
static status_t get_memory_map(void* memory, size_t size, physical_entry* entry, size_t count)
{
    assert(memory == ram.data() && size == kSdhciDmaSize && count == 1 && allocated && !freed);
    if (fault == "map") return B_IO_ERROR;
    *entry = mapping; return B_OK;
}
static void delete_area(area_id area) { assert(area == 71 && allocated == 1 && !freed); freed++; }
static void evict_allocation(void* memory, size_t size)
{
    assert(memory == ram.data() && size == kSdhciDmaSize && !converted && !freed); evicted++;
}
static status_t vm_set_area_memory_type(area_id area, phys_addr_t physical, int type)
{
    assert(area == 71 && physical == mapping.address && type == B_WRITE_COMBINING_MEMORY);
    assert(evicted == 1 && !freed && !barriers); converted++;
    return fault == "memory-type" ? B_IO_ERROR : B_OK;
}
static void memory_full_barrier() { assert(converted && !freed); barriers++; }
#define __aarch64__ 1
#define dprintf(...) do {} while (false)
#include "dma.inc"
#undef dprintf
#undef __aarch64__

int main()
{
    for (const char* failure : {"", "last-page", "allocate", "map", "short", "alignment",
            "too-high", "memory-type"}) {
        fault = failure; allocated = freed = evicted = converted = barriers = 0;
        mapping = {0x800000, kSdhciDmaSize};
        if (fault == "last-page") mapping.address = UINT64_C(0x100000000) - kSdhciDmaSize;
        if (fault == "short") mapping.size--;
        if (fault == "alignment") mapping.address += 512;
        if (fault == "too-high") mapping.address = UINT64_C(0x100000000);
        area_id area = -1; void* memory = nullptr; phys_addr_t address = 0;
        status_t status = sdhci_allocate_dma(&area, &memory, &address);
        bool success = fault.empty() || fault == "last-page";
        assert((status == B_OK) == success);
        if (success) {
            assert(area == 71 && memory == ram.data() && address == mapping.address);
            assert(evicted == 1 && converted == 1 && barriers == 1 && !freed);
            delete_area(area);
        } else {
            assert(area == -1 && memory == nullptr && address == 0);
            if (fault != "memory-type") assert(!converted && !evicted);
        }
        assert(allocated == freed);
    }
    puts("SDHCI DMA allocation and cleanup passed");
}
