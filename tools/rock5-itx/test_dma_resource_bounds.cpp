/* Production DMA translation with prepared physical buffers and no hardware I/O. */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using uint32 = uint32_t;
using int32 = int32_t;
using generic_addr_t = uint64_t;
using generic_size_t = uint64_t;
using phys_addr_t = uint64_t;
using phys_size_t = uint64_t;
using status_t = int32;
struct generic_io_vec { uint64_t base, length; };
struct physical_entry { uint64_t address, size; };
constexpr status_t B_OK = 0, B_BUSY = -1, B_ERROR = -2, B_BAD_VALUE = -3;
#define ASSERT(x) assert(x)
#define TRACE(...) do {} while (false)
#define kprintf(...) do {} while (false)
#define panic(...) assert(false)
#define min_c(a, b) ((a) < (b) ? (a) : (b))
#define max_c(a, b) ((a) > (b) ? (a) : (b))
using mutex = int;
static void mutex_init(mutex*, const char*) {}
static void mutex_lock(mutex*) {}
static void mutex_destroy(mutex*) {}
struct MutexLocker { explicit MutexLocker(mutex&) {} };
template<class T> struct DoublyLinkedListLinkImpl {};
template<class T> struct DoublyLinkedList {
    std::vector<T*> entries;
    T* Head() { return entries.empty() ? nullptr : entries.front(); }
    T* RemoveHead() {
        T* item = Head();
        if (item) entries.erase(entries.begin());
        return item;
    }
    void Add(T* item) { entries.push_back(item); }
};

#define private public
#include "dma_resources.h"
#undef private

struct IOBuffer {
    std::vector<generic_io_vec> vecs;
    bool virtualMemory = false;
    bool IsVirtual() const { return virtualMemory; }
    bool IsMemoryLocked() const { return true; }
    bool IsUser() const { return false; }
    uint32 VecCount() const { return vecs.size(); }
    generic_io_vec& VecAt(uint32 index) { return vecs.at(index); }
    generic_io_vec* Vecs() { return vecs.data(); }
};

struct IORequest {
    IOBuffer buffer;
    off_t offset;
    generic_size_t length, advanced = 0;
    bool write;
    IOBuffer* Buffer() { return &buffer; }
    off_t Offset() const { return offset; }
    generic_size_t Length() const { return length; }
    generic_size_t RemainingBytes() const { return length - advanced; }
    uint32 VecIndex() const { return 0; }
    uint32 VecOffset() const { return advanced; }
    bool IsWrite() const { return write; }
    int TeamID() const { return 1; }
    void Advance(generic_size_t bytes) {
        assert(bytes <= RemainingBytes());
        advanced += bytes;
    }
};

struct IOOperation {
    DMABuffer* buffer = nullptr;
    generic_size_t blockSize = 0, originalLength = 0, length = 0;
    off_t originalOffset = 0, offset = 0;
    bool partialBegin = false, partialEnd = false, bounce = false;
    void SetBuffer(DMABuffer* value) { buffer = value; }
    void SetBlockSize(generic_size_t value) { blockSize = value; }
    void SetOriginalRange(off_t start, generic_size_t size) {
        originalOffset = start; originalLength = size;
    }
    void SetRange(off_t start, generic_size_t size) { offset = start; length = size; }
    void SetPartial(bool begin, bool end) { partialBegin = begin; partialEnd = end; }
    void SetUsesBounceBuffer(bool value) { bounce = value; }
    bool UsesBounceBuffer() const { return bounce; }
    generic_size_t OriginalLength() const { return originalLength; }
    status_t Prepare(IORequest*) { return B_OK; }
};

static phys_addr_t mappedAddress;
static void get_memory_map_etc(int team, void*, generic_size_t size,
    physical_entry* entry, uint32* count)
{
    assert(team == 1 && *count == 1);
    *entry = {mappedAddress, size};
}

#include "dma_bounds.inc"

struct Fixture {
    DMAResource resource;
    DMABuffer* buffer = DMABuffer::Create(4);
    DMABounceBuffer bounce;
    Fixture() {
        assert(buffer);
        resource.fBlockSize = 512;
        resource.fRestrictions = {UINT64_C(0x100000000), UINT64_MAX,
            512, 512 * 1024, UINT64_MAX, 4, 512 * 1024 - 512, 0};
        resource.fBounceBufferSize = 16384;
        resource.fScratchVecs = static_cast<generic_io_vec*>(calloc(4, sizeof(generic_io_vec)));
        assert(resource.fScratchVecs);
        bounce.address = nullptr;
        bounce.physical_address = UINT64_C(0x110000000);
        bounce.size = resource.fBounceBufferSize;
        resource.fDMABuffers.Add(buffer);
        resource.fBounceBuffers.Add(&bounce);
    }
    ~Fixture() { free(buffer); }
};

static IOOperation translate(Fixture& fixture, generic_size_t size, off_t offset,
    bool write, phys_addr_t address, bool virtualMemory = false,
    generic_size_t limit = 0)
{
    IORequest request;
    request.offset = offset; request.length = size; request.write = write;
    request.buffer.vecs.push_back({address, size});
    request.buffer.virtualMemory = virtualMemory;
    mappedAddress = address;
    IOOperation operation;
    assert(fixture.resource.TranslateNext(&request, &operation, limit) == B_OK);
    assert(operation.originalOffset == offset && operation.originalLength == request.advanced);
    generic_size_t translated = 0;
    for (uint32 i = 0; i < operation.buffer->VecCount(); i++) {
        const generic_io_vec& vec = operation.buffer->VecAt(i);
        assert(vec.base >= fixture.resource.fRestrictions.low_address);
        assert(vec.length > 0 && vec.length % 512 == 0);
        translated += vec.length;
    }
    assert(translated == operation.length);
    return operation;
}

int main()
{
    constexpr off_t cardSize = 7818182656;
    for (bool write : {false, true}) for (bool virtualMemory : {false, true}) {
        for (generic_size_t size : {512u, 4096u, 12288u}) {
            Fixture fixture;
            IOOperation op = translate(fixture, size, cardSize - size, write,
                0x200000, virtualMemory);
            // In particular, a 4 KiB request must not consume the entire
            // 16 KiB bounce buffer or submit blocks past the device end.
            assert(op.length == size && op.offset + off_t(op.length) == cardSize);
            assert(op.originalLength == size && !op.partialBegin && !op.partialEnd);
            assert(op.bounce && op.buffer->VecCount() == 1);
        }
        { Fixture fixture;
          IOOperation op = translate(fixture, 513, cardSize - 1024 + 17, write,
              0x200000, virtualMemory);
          assert(op.offset == cardSize - 1024 && op.length == 1024);
          assert(op.originalLength == 513 && op.partialBegin && op.partialEnd);
        }
        for (bool operationLimit : {false, true}) {
            Fixture fixture;
            if (!operationLimit) fixture.resource.fRestrictions.max_transfer_size = 1024;
            IOOperation op = translate(fixture, 4096, 4096, write, 0x200000,
                virtualMemory, operationLimit ? 1024 : 0);
            assert(op.length == 1024 && op.originalLength == 1024);
        }
        { Fixture fixture;
          IOOperation op = translate(fixture, 1024, 0, write,
              UINT64_C(0x100000000) - 512, virtualMemory);
          assert(op.length == 1024 && op.originalLength == 1024);
          assert(op.buffer->VecCount() == 2 && op.bounce);
          assert(op.buffer->VecAt(0).length == 512);
          assert(op.buffer->VecAt(1).base == UINT64_C(0x100000000));
        }
        { Fixture fixture;
          IOOperation op = translate(fixture, 4096, 0, write,
              UINT64_C(0x120000000), virtualMemory);
          assert(op.length == 4096 && !op.bounce && op.buffer->VecCount() == 1);
          assert(op.buffer->VecAt(0).base == UINT64_C(0x120000000));
        }
    }
    puts("DMA low-address request and device-end bounds passed");
}
