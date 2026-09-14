// Exercise the production mapper with independent physical-memory and allocation
// substitutes. This cannot establish real ARM64 cache or PCIe coherency.
#include "dma_host/host.h"
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

struct Region {
	char* cpu;
	size_t size;
	uint64_t physical;
	bool cacheable = false;
	std::vector<char> dram = {};
};
static std::map<void*, Region> sDMA;
static std::vector<Region> sNormal;
static std::set<void*> sHeap;
static int sFailAfter = -1;
static int sBarriers;
static bool sForbidLookup;
static uint64_t sNextPhysical = 0x1000000;
static bool sSettingsPresent, sSettingsValue;
static unsigned sSettingsLoads, sSettingsUnloads;
static unsigned sIRQDepth;
static size_t sLineSize = 64;
struct CacheOperation { uintptr_t address; char op; std::vector<char> bytes; };
static std::vector<CacheOperation> sPendingCache;
static std::vector<std::pair<uintptr_t, char>> sCacheHistory;

static bool failAllocation()
{
	if (sFailAfter < 0) return false;
	if (sFailAfter == 0) return true;
	--sFailAfter;
	return false;
}
extern "C" void* _kernel_malloc(size_t size, int flags)
{
	if (failAllocation()) return nullptr;
	void* p = malloc(size);
	assert(p != nullptr);
	if (flags & M_ZERO) memset(p, 0, size);
	sHeap.insert(p);
	return p;
}
extern "C" void _kernel_free(void* p)
{
	if (p == nullptr) return;
	assert(sHeap.erase(p) == 1);
	free(p);
}
extern "C" void* _kernel_contigmalloc(const char*, int, size_t size, int flags,
	uint64_t low, uint64_t high, unsigned long alignment, unsigned long boundary)
{
	return _kernel_contigmalloc_etc(__FILE__, __LINE__, size, flags, low, high,
		alignment, boundary, false);
}
extern "C" void* _kernel_contigmalloc_etc(const char*, int, size_t size, int flags,
	uint64_t low, uint64_t high, unsigned long alignment, unsigned long boundary,
	bool cacheable)
{
	if (failAllocation()) return nullptr;
	alignment = std::max(alignment, B_PAGE_SIZE);
	uint64_t address = (std::max(sNextPhysical, low) + alignment - 1)
		& ~(alignment - 1);
	if (boundary && ((address ^ (address + size - 1)) & ~(boundary - 1)))
		address = (address + boundary - 1) & ~(boundary - 1);
	if (address > high || size - 1 > high - address) return nullptr;
	size_t capacity = (size + B_PAGE_SIZE - 1) & ~PAGE_MASK;
	void* p = aligned_alloc(B_PAGE_SIZE, capacity);
	assert(p != nullptr);
	memset(p, flags & M_ZERO ? 0 : 0xcd, capacity);
	Region r{(char*)p, size, address, cacheable};
	if (cacheable) r.dram.assign(capacity, 0xe2);
	sDMA.emplace(p, std::move(r));
	sNextPhysical = address + ((size + PAGE_MASK) & ~PAGE_MASK);
	return p;
}
extern "C" void _kernel_contigfree(void* p, size_t)
{
	if (p == nullptr) return;
	assert(sPendingCache.empty());
	assert(sDMA.erase(p) == 1);
	free(p);
}
extern "C" bool _kernel_contig_dma_address(const void* p, size_t size,
	uint64_t* physical)
{
	for (const auto& item : sDMA) {
		const auto& r = item.second;
		uintptr_t offset = (uintptr_t)p - (uintptr_t)r.cpu;
		if (!r.cacheable && (uintptr_t)p >= (uintptr_t)r.cpu && offset < r.size
			&& size <= r.size - offset) {
			*physical = r.physical + offset;
			return true;
		}
	}
	return false;
}
extern "C" uint64_t pmap_kextract(uintptr_t p)
{
	assert(!sForbidLookup);
	for (const auto& item : sDMA) {
		const auto& r = item.second;
		if (p >= (uintptr_t)r.cpu && p - (uintptr_t)r.cpu < r.size)
			return r.physical + p - (uintptr_t)r.cpu;
	}
	for (const auto& r : sNormal) {
		if (p >= (uintptr_t)r.cpu && p - (uintptr_t)r.cpu < r.size)
			return r.physical + p - (uintptr_t)r.cpu;
	}
	throw std::runtime_error("unregistered virtual address");
}
extern "C" cpu_status disable_interrupts() { return sIRQDepth++; }
extern "C" void restore_interrupts(cpu_status old)
{
	assert(sIRQDepth == old + 1 && sPendingCache.empty());
	sIRQDepth = old;
}
uint64_t arm64_current_data_cache_line_size()
{
	assert(sIRQDepth != 0);
	return sLineSize;
}
static Region& cacheRegion(uintptr_t address)
{
	for (auto& item : sDMA) {
		auto& r = item.second;
		if (r.cacheable && address >= (uintptr_t)r.cpu
			&& address - (uintptr_t)r.cpu < r.dram.size()) return r;
	}
	throw std::runtime_error("cache operation outside private cached DMA pages");
}
static void cacheOperation(uintptr_t address, char op)
{
	assert(sIRQDepth != 0 && address % sLineSize == 0);
	auto& r = cacheRegion(address);
	assert(address - (uintptr_t)r.cpu + sLineSize <= r.dram.size());
	CacheOperation pending{address, op, {}};
	if (op != 'i') pending.bytes.assign((char*)address, (char*)address + sLineSize);
	sPendingCache.push_back(std::move(pending));
	sCacheHistory.emplace_back(address, op);
}
void arm64_clean_data_cache_line_poc(uintptr_t address) { cacheOperation(address, 'c'); }
void arm64_invalidate_data_cache_line_poc(uintptr_t address) { cacheOperation(address, 'i'); }
void arm64_clean_invalidate_data_cache_line_poc(uintptr_t address) { cacheOperation(address, 'b'); }
extern "C" void memory_full_barrier()
{
	++sBarriers;
	for (const auto& p : sPendingCache) {
		auto& r = cacheRegion(p.address);
		size_t offset = p.address - (uintptr_t)r.cpu;
		if (p.op != 'i') memcpy(r.dram.data() + offset, p.bytes.data(), sLineSize);
		if (p.op != 'c') memcpy((void*)p.address, r.dram.data() + offset, sLineSize);
	}
	sPendingCache.clear();
}
extern "C" void* load_driver_settings(const char* name)
{
	assert(strcmp(name, "test_driver") == 0 && !sForbidLookup && sIRQDepth == 0);
	++sSettingsLoads;
	return sSettingsPresent ? &sSettingsValue : nullptr;
}
extern "C" bool get_driver_boolean_parameter(void* handle, const char* key,
	bool missing, bool noarg)
{
	assert(handle == &sSettingsValue && strcmp(key, "cached_packet_buffers") == 0);
	assert(!missing && !noarg);
	return sSettingsValue;
}
extern "C" int unload_driver_settings(void* handle)
{
	assert(handle == &sSettingsValue);
	++sSettingsUnloads;
	return 0;
}
extern "C" uint64_t vm_page_max_address() { return UINT64_C(0x400000000); }
extern "C" void dma_debug(const char*, ...) {}
extern "C" void panic(const char* message, ...) { throw std::runtime_error(message); }

#include "../../src/libs/compat/freebsd_network/bus_dma.cpp"

struct Completion {
	int calls = 0;
	int error = -1;
	std::vector<bus_dma_segment_t> segments;
	static void callback(void* arg, bus_dma_segment_t* segments, int count, int error)
	{
		auto& c = *(Completion*)arg;
		++c.calls;
		c.error = error;
		c.segments.clear();
		if (count) c.segments.assign(segments, segments + count);
	}
};
static bus_dma_tag_t tag(size_t size = 8192, int segments = 8,
	size_t maxSegment = 4096, uint64_t low = UINT64_MAX, uint64_t high = UINT64_MAX)
{
	bus_dma_tag_t result;
	assert(bus_dma_tag_create(nullptr, 1, 0, low, high, nullptr, nullptr,
		size, segments, maxSegment, 0, nullptr, nullptr, &result) == 0);
	return result;
}
static void clean(bus_dma_tag_t t, bus_dmamap_t m)
{
	bus_dmamap_unload(t, m);
	assert(bus_dmamap_destroy(t, m) == 0);
	assert(bus_dma_tag_destroy(t) == 0);
}
static char* devicePointer(uint64_t address)
{
	for (auto& item : sDMA) {
		auto& r = item.second;
		if (address >= r.physical && address - r.physical < r.size)
			return (r.cacheable ? r.dram.data() : r.cpu) + address - r.physical;
	}
	throw std::runtime_error("unknown device address");
}
static void checkFailures()
{
	auto t = tag(8192, 8, 4096, 0xffffffff);
	for (int fail = 0; fail < 3; ++fail) {
		sFailAfter = fail;
		void* buffer = (void*)1;
		bus_dmamap_t map = (bus_dmamap_t)1;
		assert(bus_dmamem_alloc(t, &buffer, BUS_DMA_NOWAIT, &map) == ENOMEM);
		assert(buffer == nullptr && map == nullptr && t->map_count == 0);
		map = (bus_dmamap_t)1;
		sFailAfter = fail;
		assert(bus_dmamap_create(t, BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW, &map) == ENOMEM);
		assert(map == nullptr && t->map_count == 0);
		assert(sDMA.empty() && sHeap.size() == 1);
	}
	sFailAfter = -1;
	assert(bus_dma_tag_destroy(t) == 0);
	bus_dma_tag_t bad = (bus_dma_tag_t)1;
	assert(bus_dma_tag_create(nullptr, 0, 0, UINT64_MAX, UINT64_MAX,
		nullptr, nullptr, 4096, 1, 4096, 0, nullptr, nullptr, &bad) == EINVAL);
	assert(bad == nullptr);
}

static void checkUnrestrictedParent()
{
	bus_dma_tag_t parent;
	assert(bus_dma_tag_create(nullptr, 1, 0, 0xffffffff, UINT64_MAX,
		nullptr, nullptr, 0xffffffff, BUS_SPACE_UNRESTRICTED, 0xffffffff,
		0, nullptr, nullptr, &parent) == 0);
	assert(parent != nullptr && parent->maxsegments == INT32_MAX);
	bus_dma_tag_t child;
	assert(bus_dma_tag_create(parent, 1, 0, UINT64_MAX, UINT64_MAX,
		nullptr, nullptr, 16384, 1, 16384, 0, nullptr, nullptr, &child) == 0);
	assert(child->lowaddr == 0xffffffff && child->highaddr == UINT64_MAX);
	assert(!_validate_address(child, UINT64_C(0x100000000), 4096));
	assert(_validate_address(child, UINT64_C(0xfffff000), 4096));
	assert(bus_dma_tag_destroy(child) == 0);
	assert(bus_dma_tag_destroy(parent) == 0);
}
static void checkDescriptor()
{
	auto t = tag(4096);
	void* ring;
	// Match OpenBSD: allocate with no map, then load into a separate map.
	assert(bus_dmamem_alloc(t, &ring, BUS_DMA_ZERO, nullptr) == 0);
	bus_dmamap_t map;
	assert(bus_dmamap_create(t, BUS_DMA_ALLOCNOW, &map) == 0);
	Completion c;
	assert(bus_dmamap_load(t, map, ring, 4096, c.callback, &c, 0) == 0);
	assert(c.calls == 1 && c.error == 0 && c.segments.size() == 1);
	assert(devicePointer(c.segments[0].ds_addr) == ring);
	assert(bus_dmamap_destroy(t, map) == EBUSY);
	auto other = tag();
	assert(bus_dmamap_destroy(other, map) == EINVAL);
	Completion busy;
	assert(bus_dmamap_load(t, map, ring, 128, busy.callback, &busy, 0) == EBUSY);
	assert(map->buffer_length == 4096 && busy.error == EBUSY);
	assert(bus_dma_tag_destroy(other) == 0);
	memset((char*)ring + 3, 0x71, 37);
	int before = sBarriers;
	size_t cacheBefore = sCacheHistory.size();
	for (int op : {BUS_DMASYNC_PREREAD, BUS_DMASYNC_PREWRITE,
		BUS_DMASYNC_POSTREAD, BUS_DMASYNC_POSTWRITE})
		bus_dmamap_sync_etc(t, map, 3, 37, op);
	assert(sBarriers == before + 4);
	assert(sCacheHistory.size() == cacheBefore);
	assert(((char*)ring)[3] == 0x71);
	bool rejected = false;
	try { bus_dmamap_sync_etc(t, map, UINT64_MAX, 16, BUS_DMASYNC_PREWRITE); }
	catch (const std::runtime_error&) { rejected = true; }
	assert(rejected);
	bus_dmamap_unload(t, map);
	bus_dmamem_free_tagless(ring, 4096);
	assert(bus_dmamap_destroy(t, map) == 0);
	assert(bus_dma_tag_destroy(t) == 0);
}
static void checkPacket()
{
	std::vector<char> first(128, 0x31), second(192, 0x52);
	sNormal = {{first.data(), first.size(), UINT64_C(0x100001001)},
		{second.data(), second.size(), UINT64_C(0x200002003)}};
	mbuf b{nullptr, second.data(), 192, 0, {0}};
	mbuf empty{&b, nullptr, 0, 0, {0}};
	mbuf a{&empty, first.data(), 128, M_PKTHDR, {320}};
	auto t = tag(8192, 8, 4096, 0xffffffff);
	bus_dmamap_t map;
	assert(bus_dmamap_create(t, BUS_DMA_ALLOCNOW, &map) == 0);
	sFailAfter = 0; // Neither load nor sync may allocate for this reserved map.
#ifdef FBSD_NONCOHERENT_DMA
	sForbidLookup = true;
#endif
	bus_dma_segment_t segments[8];
	int count = -1;
	assert(bus_dmamap_load_mbuf_sg(t, map, &a, segments, &count, 0) == 0);
	assert(count == 1);
	char* device = devicePointer(segments[0].ds_addr);
	bus_dmamap_sync(t, map, BUS_DMASYNC_PREWRITE);
	assert(memcmp(device, first.data(), 128) == 0);
	assert(memcmp(device + 128, second.data(), 192) == 0);
	memset(device + 111, 0x67, 42);
	bus_dmamap_sync_etc(t, map, 111, 42, BUS_DMASYNC_POSTREAD);
	for (int i = 0; i < 128; ++i) assert(first[i] == (i >= 111 ? 0x67 : 0x31));
	for (int i = 0; i < 192; ++i) assert(second[i] == (i < 25 ? 0x67 : 0x52));
	// The original mapping length must survive a packet-header length change.
	a.m_pkthdr.len = 1;
	memset(device, 0x29, 320);
	bus_dmamap_sync(t, map, BUS_DMASYNC_POSTREAD);
	assert(std::all_of(first.begin(), first.end(), [](char c) { return c == 0x29; }));
	assert(std::all_of(second.begin(), second.end(), [](char c) { return c == 0x29; }));
	bus_dmamap_unload(t, map);
	a.m_pkthdr.len = 319;
	count = 22;
	assert(bus_dmamap_load_mbuf_sg(t, map, &a, segments, &count, 0) == EINVAL);
	assert(count == 0 && !map->loaded);
	sFailAfter = -1;
	sForbidLookup = false;
	clean(t, map);
	sNormal.clear();
}
static void checkRollback()
{
	std::vector<char> bytes(8192, 0x11);
	sNormal = {{bytes.data(), bytes.size(), UINT64_C(0x100000000)}};
	auto t = tag(8192, 1, 1024, 0xffffffff);
	bus_dmamap_t map;
	assert(bus_dmamap_create(t, BUS_DMA_ALLOCNOW, &map) == 0);
	Completion c;
	assert(bus_dmamap_load(t, map, bytes.data(), 5000, c.callback, &c, 0) == 0);
	assert(c.error == EFBIG && c.segments.empty() && !map->loaded);
	assert(bus_dmamap_load(t, map, bytes.data(), 512, c.callback, &c, 0) == 0);
	assert(c.error == 0 && map->loaded);
	bus_dmamap_sync_etc(t, map, 7, 63, BUS_DMASYNC_PREWRITE);
	char* device = devicePointer(c.segments[0].ds_addr);
	assert(device[6] == (char)0xcd && device[7] == 0x11 && device[70] == (char)0xcd);
	memset(device + 19, 0x43, 25);
	bus_dmamap_sync_etc(t, map, 19, 25, BUS_DMASYNC_POSTREAD);
	assert(bytes[18] == 0x11 && bytes[19] == 0x43 && bytes[44] == 0x11);
	clean(t, map);
	sNormal.clear();
}
static void checkIntervals()
{
	auto t = tag();
	std::mt19937_64 random(0x3588);
	for (int i = 0; i < 50000; ++i) {
		uint64_t start = i % 2 ? random() : UINT64_MAX - (random() % 8192);
		uint64_t length = random() % 16384;
		uint64_t low = random(), high = random();
		if (low > high) std::swap(low, high);
		if (i % 3 == 0) { low = start; high = UINT64_MAX; }
		t->lowaddr = low; t->highaddr = high;
		__uint128_t end = (__uint128_t)start + length;
		bool expected = length != 0 && end <= (__uint128_t)UINT64_MAX + 1;
		if (expected && low < high && end > (__uint128_t)low + 1 && start <= high)
			expected = false;
		assert(_validate_address(t, start, length) == expected);
	}
	t->lowaddr = t->highaddr = UINT64_MAX;
	for (size_t boundary : {0, 64, 512, 4096}) {
		t->boundary = boundary;
		t->maxsegsz = boundary ? boundary : 4096;
		t->maxsegments = 128;
		for (size_t offset : {0, 3, 63, 127, 4095}) {
			bus_dma_segment_t segments[128];
			int lastIndex = 0; uint64_t last = 0;
			uint64_t physical = UINT64_C(0x100000000) + offset;
			assert(_bus_load_buffer(t, (void*)0x10000, 5001, last, segments,
				lastIndex, true, true, physical) == 0);
			size_t total = 0;
			for (int i = 0; i <= lastIndex; ++i) {
				const auto& s = segments[i];
				assert(s.ds_addr == physical + total && s.ds_len > 0);
				assert(s.ds_len <= t->maxsegsz);
				if (boundary) assert(s.ds_addr / boundary == (s.ds_addr + s.ds_len - 1) / boundary);
				total += s.ds_len;
			}
			assert(total == 5001);
		}
	}
	assert(bus_dma_tag_destroy(t) == 0);
}
static void checkCachedOwnership()
{
#ifdef FBSD_NONCOHERENT_DMA
	const size_t length = 12289;
	std::vector<char> bytes(length, 0x31);
	auto t = tag(16384, 8);
	bus_dmamap_t map;
	assert(bus_dmamap_create(t, 0, &map) == 0);
	assert(map->cacheable_bounce);
	void* bounce = map->bounce_buffer;
	uint64_t ignored = 0x1234;
	assert(!_kernel_contig_dma_address(bounce, length, &ignored) && ignored == 0x1234);
	// Policy is captured at creation, never consulted while transferring packets.
	sSettingsValue = false;
	_fbsd_init_bus_dma("test_driver");
	assert(map->cacheable_bounce);
	unsigned settingsLoads = sSettingsLoads;
	sFailAfter = 0;
	sForbidLookup = true;
	Completion c;
	assert(bus_dmamap_load(t, map, bytes.data(), length, c.callback, &c, 0) == 0);
	assert(c.error == 0);
	char* device = devicePointer(c.segments[0].ds_addr);
	assert(device != bounce);
	for (size_t lineSize : {4, 16, 32, 64, 128, 256, 4096}) {
		sLineSize = lineSize;
		for (size_t offset : {0, 1, 63, 64, 65, 4095, 4096, 12288}) {
			for (size_t count : {0, 1, 7, 63, 64, 65, 1501, 8193}) {
				if (count > length - offset) continue;
				for (int pre : {BUS_DMASYNC_PREWRITE, BUS_DMASYNC_PREREAD,
					BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD}) {
					std::fill(bytes.begin(), bytes.end(), 0x31);
					memset(bounce, 0x75, length);
					sCacheHistory.clear();
					bus_dmamap_sync_etc(t, map, offset, count, pre);
					size_t expectedLines = count ?
						(offset + count - 1) / lineSize - offset / lineSize + 1 : 0;
					assert(sCacheHistory.size() == expectedLines && sPendingCache.empty());
					for (size_t i = 0; i < expectedLines; ++i) {
						assert(sCacheHistory[i].first == (uintptr_t)bounce
							+ (offset / lineSize + i) * lineSize);
						assert(sCacheHistory[i].second ==
							(pre & BUS_DMASYNC_PREREAD ? 'b' : 'c'));
					}
					if (pre & BUS_DMASYNC_PREWRITE)
						assert(memcmp(device + offset, bytes.data() + offset, count) == 0);
					memset(device + offset, 0x68, count);
					auto& region = sDMA.at(bounce);
					const auto deviceSnapshot = region.dram;
					if (count) assert(((char*)bounce)[offset] != device[offset]);
					sCacheHistory.clear();
					int barriers = sBarriers;
					bus_dmamap_sync_etc(t, map, offset, count, BUS_DMASYNC_POSTREAD);
					assert(sBarriers == barriers + 2 && sIRQDepth == 0);
					assert(sCacheHistory.size() == expectedLines && sPendingCache.empty());
					for (const auto& item : sCacheHistory) assert(item.second == 'i');
					// Invalidation must not write stale CPU bytes into device RAM,
					// including padding in either partial boundary cache line.
					assert(region.dram == deviceSnapshot);
					for (size_t i = 0; i < length; ++i)
						assert(bytes[i] == (i >= offset && i - offset < count ? 0x68 : 0x31));
				}
			}
		}
	}
	sLineSize = 8192;
	bool rejected = false;
	try { bus_dmamap_sync(t, map, BUS_DMASYNC_PREREAD); }
	catch (const std::runtime_error&) { rejected = true; }
	assert(rejected && sIRQDepth == 0 && sPendingCache.empty());
	sLineSize = 64;
	sCacheHistory.clear();
	bus_dmamap_sync(t, map, BUS_DMASYNC_POSTWRITE);
	assert(sCacheHistory.empty());
	assert(sSettingsLoads == settingsLoads && map->bounce_buffer == bounce);
	sFailAfter = -1;
	sForbidLookup = false;
	clean(t, map);
	sSettingsValue = true;
	_fbsd_init_bus_dma("test_driver");
#endif
}

struct proc;
#include "../../src/libs/compat/openbsd_network/compat/machine/bus.h"

static void checkOpenBSD()
{
	auto parent = tag(8192, 8, 4096, 0xffffffff);
	for (int fail = 0; fail < 5; ++fail) {
		sFailAfter = fail;
		bus_dmamap_t map = (bus_dmamap_t)1;
		assert(bus_dmamap_create(parent, 8192, 8, 4096, 0,
			BUS_DMA_ALLOCNOW | BUS_DMA_NOWAIT, &map) == ENOMEM);
		assert(map == nullptr && sDMA.empty() && sHeap.size() == 1);
		assert(parent->ref_count == 1 && parent->map_count == 0);
	}
	sFailAfter = -1;
	bus_dma_segment_t segment;
	int count;
	assert(bus_dmamem_alloc(parent, 4096, 64, 0, &segment, 1, &count,
		BUS_DMA_ZERO) == 0);
	char* ring;
	assert(bus_dmamem_map(parent, &segment, count, 4096, &ring, 0) == 0);
	bus_dmamap_t map;
	assert(bus_dmamap_create(parent, 8192, 4, 256, 256,
		BUS_DMA_ALLOCNOW | BUS_DMA_NOWAIT, &map) == 0);
	assert(bus_dmamap_load(parent, map, ring, 4096, nullptr, 0) == EFBIG);
	assert(map->dm_mapsize == 0 && map->dm_nsegs == 0 && !map->_dmamp->loaded);
	assert(bus_dmamap_load(parent, map, ring, 512, nullptr, 0) == 0);
	assert(map->dm_mapsize == 512 && map->dm_nsegs == 2);
	assert(devicePointer(map->dm_segs[0].ds_addr) == ring);
	bus_dmamap_sync(parent, map, 5, 123, BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	bus_dmamap_unload(parent, map);
	mbuf invalid{nullptr, nullptr, 0, M_PKTHDR, {0}};
	assert(bus_dmamap_load_mbuf(parent, map, &invalid, 0) == EINVAL);
	assert(map->dm_mapsize == 0 && map->dm_nsegs == 0);
	bus_dmamap_destroy(parent, map);
	bus_dmamem_unmap(parent, ring, 4096);
	bus_dmamem_free(parent, &segment, count);
	assert(bus_dma_tag_destroy(parent) == 0);
}

int main()
{
	_fbsd_init_bus_dma("test_driver"); // Missing file retains the original path.
	checkFailures(); checkUnrestrictedParent(); checkDescriptor(); checkPacket();
	checkRollback(); checkIntervals(); checkOpenBSD();
	assert(sCacheHistory.empty());
	sSettingsPresent = true;
	sSettingsValue = false;
	_fbsd_init_bus_dma("test_driver");
	checkFailures(); checkDescriptor(); checkPacket(); checkRollback(); checkOpenBSD();
	assert(sCacheHistory.empty());
	sSettingsValue = true;
	_fbsd_init_bus_dma("test_driver");
	checkFailures(); checkDescriptor(); checkPacket(); checkRollback(); checkOpenBSD();
	checkCachedOwnership();
	assert(sDMA.empty() && sHeap.empty());
	assert(sPendingCache.empty() && sIRQDepth == 0);
#ifdef FBSD_NONCOHERENT_DMA
	assert(sSettingsLoads == 5 && sSettingsUnloads == 4);
#else
	assert(sSettingsLoads == 0 && sSettingsUnloads == 0 && sCacheHistory.empty());
#endif
	printf("bus_dma: allocation, rings, packets, cache ownership, rollback and 50000 interval checks passed\n");
}
