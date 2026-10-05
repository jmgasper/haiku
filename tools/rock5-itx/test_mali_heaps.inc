/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
// Production CsfClient.cpp, with the OS allocation/user-copy boundaries from
// test_mali_client.cpp. The oracle follows the actual scattered physical pages;
// it does not consult the driver's heap/chunk/mapping lists.
struct HeapPhysicalOracle {
	std::map<uint64, uint8*> pages;
	std::map<uint64, uint64> leaves;
	std::set<uint64> tables;
	explicit HeapPhysicalOracle(const ClientVmLease& lease)
	{
		for (const auto& entry : sAreas) {
			const Area& area = entry.second;
			if (area.owner != B_SYSTEM_TEAM) continue;
			assert(area.noncacheable);
			for (size_t i = 0; i < area.ram->bytes; i += 4096)
				assert(pages.emplace(area.ram->physical + i * 2, (uint8*)area.address + i).second);
		}
		Walk(lease.rootPhysical, 0, 0);
	}
	void Walk(uint64 physical, unsigned level, uint64 prefix)
	{
		assert(level < 4 && tables.insert(physical).second);
		const uint64* entries = (uint64*)pages.at(physical);
		for (unsigned i = 0; i < 512; i++) {
			uint64 pte = entries[i];
			if (pte == 0) continue;
			assert((pte & 3) == 3);
			uint64 address = prefix | (uint64(i) << (39 - level * 9));
			if (level == 3) {
				assert((pte & ~UINT64_C(0xfffffff000)) == ((UINT64_C(3) << 53) | 0x747));
				assert(pages.count(pte & UINT64_C(0xfffffff000)) == 1);
				assert(leaves.emplace(address, pte).second);
			} else {
				assert((pte & ~UINT64_C(0xfffffff003)) == 0);
				Walk(pte & UINT64_C(0xfffffff000), level + 1, address);
			}
		}
	}
	uint8* At(uint64 address) const
	{
		uint64 pte = leaves.at(address & ~UINT64_C(4095));
		return pages.at(pte & UINT64_C(0xfffffff000)) + (address & 4095);
	}
	void Bytes(uint64 address, uint64 bytes, uint64 firstWord = 0) const
	{
		assert((address & 4095) == 0 && (bytes & 4095) == 0);
		for (uint64 offset = 0; offset < bytes; offset += 4096) {
			const uint64* page = (uint64*)At(address + offset);
			for (unsigned i = 0; i < 512; i++)
				assert(page[i] == (offset == 0 && i == 0 ? firstWord : 0));
		}
	}
};

static HeapCreate HeapRequest(uint32 vm, uint32 bytes = 128 * 1024, uint32 initial = 2, uint32 max = 64)
{
	HeapCreate request{};
	request.version = 1; request.vm = vm; request.chunkSize = bytes;
	request.initialChunks = initial; request.maxChunks = max; request.targetInFlight = 65535;
	return request;
}
static HeapCreate CreateHeapFor(void* client, uint32 vm, uint32 bytes = 128 * 1024,
	uint32 initial = 2, uint32 max = 64)
{
	auto request = HeapRequest(vm, bytes, initial, max);
	assert(Call(client, kCreateHeap, request) == B_OK);
	assert(request.handle && request.contextAddress >= kHeapBase);
	return request;
}
static HeapInfo HeapDetails(void* client, uint32 vm = 0, uint32 heap = 0)
{
	HeapInfo info{}; info.version = 1; info.vm = vm; info.handle = heap;
	assert(Call(client, kGetHeapInfo, info) == B_OK);
	return info;
}
static void DropHeap(void* client, uint32 vm, uint32 heap)
{
	HeapHandle request{}; request.version = 1; request.vm = vm; request.handle = heap;
	assert(Call(client, kDestroyHeap, request) == B_OK && request.generation > 1);
	request.generation = 0;
	assert(Call(client, kDestroyHeap, request) == B_ENTRY_NOT_FOUND);
}
static void HeapFailureBaseline(void* client, uint32 vm, uint64 number, size_t areas)
{
	assert(VmDetails(client, vm).generation == number);
	assert(sAreas.size() == areas && sHeaps == 0 && sHeapChunks == 0);
	assert(sHeapBytes == 0 && sHeapTablePages == 0 && sHeapGenerations == 0);
}
static void CheckHeaps()
{
	Empty();
	void *client, *foreign, *readonly;
	assert(OpenClient(true, &client) == B_OK && OpenClient(true, &foreign) == B_OK);
	assert(OpenClient(false, &readonly) == B_OK);
	auto vm = NewVm(client);
	auto request = HeapRequest(vm.handle);
	assert(Call(readonly, kCreateHeap, request) == B_NOT_ALLOWED);
	assert(Call(foreign, kCreateHeap, request) == B_ENTRY_NOT_FOUND);
	sTeam++;
	assert(Call(client, kCreateHeap, request) == B_NOT_ALLOWED);
	sTeam--;
	for (unsigned error = 0; error < 13; error++) {
		auto bad = request;
		if (error == 0) bad.version++;
		if (error == 1) bad.flags++;
		if (error == 2) bad.handle++;
		if (error == 3) bad.contextAddress++;
		if (error == 4) bad.firstChunkAddress++;
		if (error == 5) bad.generation++;
		if (error == 6) bad.reserved++;
		if (error == 7) bad.initialChunks = 0;
		if (error == 8) bad.initialChunks = bad.maxChunks + 1;
		if (error == 9) bad.maxChunks = 65;
		if (error == 10) bad.chunkSize++;
		if (error == 11) bad.chunkSize = 124 * 1024;
		if (error == 12) bad.chunkSize = 8 * 1024 * 1024 + 4096;
		assert(Call(client, kCreateHeap, bad) == B_BAD_VALUE);
	}
	assert(ControlClient(client, kCreateHeap, &request, sizeof(request) - 1) == B_BAD_VALUE);
	auto tooLarge = HeapRequest(vm.handle, 8 * 1024 * 1024, 32, 64);
	assert(Call(client, kCreateHeap, tooLarge) == B_NO_MEMORY); // context also consumes the quota
	for (unsigned failure = 1; failure <= 6; failure++) {
		sFailAllocation = failure;
		assert(Call(client, kCreateHeap, request) != B_OK);
		sFailAllocation = 0; HeapFailureBaseline(client, vm.handle, 1, 1);
	}
	for (unsigned failure = 1; failure <= 2; failure++) {
		sFailCopy = failure;
		assert(Call(client, kCreateHeap, request) == B_BAD_ADDRESS);
		sFailCopy = 0; HeapFailureBaseline(client, vm.handle, 1, 1);
	}
	// Fail each metadata and each backing-area allocation, including later chunks
	// and the final replacement generation. Every failed call preserves the VM.
	unsigned metadataCases = 0, areaCases = 0;
	for (bool areas : {false, true}) {
		for (unsigned failure = 1; failure < 64; failure++) {
			auto attempt = HeapRequest(vm.handle, 2 * 1024 * 1024, 5);
			sHeapCalls = sAreaCalls = 0;
			if (areas) sFailArea = failure; else sFailHeap = failure;
			status_t result = Call(client, kCreateHeap, attempt);
			sFailArea = sFailHeap = 0;
			if (result == B_OK) {
				DropHeap(client, vm.handle, attempt.handle);
				break;
			}
			assert(result == B_NO_MEMORY);
			if (areas) areaCases++; else metadataCases++;
			HeapFailureBaseline(client, vm.handle, areas ? 3 : 1, 1);
			assert(failure != 63);
		}
	}
	assert(metadataCases >= 20 && areaCases >= 10);
	for (uint32 chunkSize : {128u * 1024, 132u * 1024, 2u * 1024 * 1024, 8u * 1024 * 1024}) {
		auto heap = CreateHeapFor(client, vm.handle, chunkSize, 3);
		ClientVmLease lease{};
		assert(AcquireClientVm(client, vm.handle, heap.generation, lease) == B_OK);
		HeapPhysicalOracle before(lease);
		assert(before.leaves.size() == 1 + 3 * chunkSize / 4096);
		before.Bytes(heap.contextAddress, 4096);
		uint64 start = heap.contextAddress + kHeapChunksOffset;
		assert(heap.firstChunkAddress == start + 2 * chunkSize);
		for (unsigned i = 0; i < 3; i++)
			before.Bytes(start + i * chunkSize, chunkSize, i ? (start + (i - 1) * chunkSize) | (chunkSize >> 12) : 0);
		assert(!ClientVmRange(lease, heap.contextAddress, 8));
		assert(!ClientVmRange(lease, start, 8));
		BufferInfo hidden{1, heap.handle, 0, 0, 0};
		assert(Call(client, kGetBufferInfo, hidden) == B_ENTRY_NOT_FOUND);
		HeapInfo other{}; other.version = 1; other.vm = vm.handle; other.handle = heap.handle;
		assert(Call(foreign, kGetHeapInfo, other) == B_ENTRY_NOT_FOUND);
		// Simulated firmware context data must survive CPU-side growth and binds.
		*(uint64*)before.At(heap.contextAddress) = UINT64_C(0x1234567887654321);
		for (unsigned error = 0; error < 7; error++) {
			HeapGrowth bad{};
			uint64 address = heap.contextAddress;
			uint32 vtStart = 2, vtEnd = 1, fragEnd = 0;
			if (error == 0) address += 32;
			if (error == 1) address = kHeapBase - 1;
			if (error == 2) address = UINT64_MAX;
			if (error == 3) address += kHeapSlotBytes;
			if (error == 4) vtStart = vtEnd;
			if (error == 5) fragEnd = 2;
			if (error == 6) bad.bytes = 1;
			assert(PrepareClientHeapGrowth(lease, address, vtStart, vtEnd, fragEnd, bad) != B_OK);
			assert(bad.state == NULL && HeapPhysicalOracle(lease).leaves == before.leaves);
		}
		HeapGrowth growth{}, concurrent{};
		assert(PrepareClientHeapGrowth(lease, heap.contextAddress, 2, 1, 0, growth) == B_OK);
		assert(growth.address == start + 3 * chunkSize && growth.bytes == chunkSize);
		assert(growth.encodedChunk == (growth.address | (chunkSize >> 12)));
		assert(HeapPhysicalOracle(lease).leaves == before.leaves);
		assert(HeapDetails(client).globalChunks == 4);
		assert(PrepareClientHeapGrowth(lease, heap.contextAddress, 2, 1, 0, concurrent) == B_BUSY);
		AbortClientHeapGrowth(growth);
		assert(growth.state == NULL && HeapDetails(client).globalChunks == 3);
		assert(PrepareClientHeapGrowth(lease, heap.contextAddress, 2, 1, 0, growth) == B_OK);
		assert(CommitClientHeapGrowth(growth) == B_OK && growth.state == NULL);
		assert(CommitClientHeapGrowth(growth) == B_BAD_VALUE);
		HeapPhysicalOracle after(lease);
		assert(after.leaves.size() == 1 + 4 * chunkSize / 4096);
		for (const auto& entry : before.leaves) assert(after.leaves.at(entry.first) == entry.second);
		after.Bytes(start + 3 * chunkSize, chunkSize);
		assert(*(uint64*)after.At(heap.contextAddress) == UINT64_C(0x1234567887654321));
		// Destruction/copyout failure preserves membership. A successful destroy
		// retires the old object; the next generation may reuse its VA safely.
		HeapHandle destroy{}; destroy.version = 1; destroy.vm = vm.handle; destroy.handle = heap.handle;
		sFailCopy = 2;
		assert(Call(client, kDestroyHeap, destroy) == B_BAD_ADDRESS);
		sFailCopy = 0;
		assert(HeapDetails(client, vm.handle, heap.handle).chunkCount == 4);
		DropHeap(client, vm.handle, heap.handle);
		auto replacement = CreateHeapFor(client, vm.handle, chunkSize, 1);
		assert(replacement.contextAddress == heap.contextAddress && replacement.handle != heap.handle);
		ClientVmLease next{};
		assert(AcquireClientVm(client, vm.handle, 0, next) == B_OK);
		HeapPhysicalOracle fresh(next);
		fresh.Bytes(replacement.contextAddress, 4096);
		assert(fresh.leaves.at(heap.contextAddress) != after.leaves.at(heap.contextAddress));
		assert(HeapPhysicalOracle(lease).leaves == after.leaves && HeapDetails(client).globalHeaps == 2);
		DropHeap(client, vm.handle, replacement.handle);
		ReleaseClientVm(next); ReleaseClientVm(lease);
		assert(HeapDetails(client).globalHeaps == 0);
	}
	// CPU mapping generations carry heap membership without exposing private
	// ranges to user bind/unmap. Queued old roots retain both mappings.
	auto buffer = Create(client, 4096);
	assert(Bind(client, vm.handle, {{kVmMapOperation, kVmNoExecute, 0x10000, 4096, buffer.handle, 0, 0}}) == B_OK);
	auto sharedHeap = CreateHeapFor(client, vm.handle);
	ClientVmLease withBuffer{}, withoutBuffer{};
	assert(AcquireClientVm(client, vm.handle, 0, withBuffer) == B_OK);
	HeapPhysicalOracle combined(withBuffer);
	assert(combined.leaves.count(0x10000) == 1 && combined.leaves.count(sharedHeap.contextAddress) == 1);
	assert(Bind(client, vm.handle, {{kVmUnmapOperation, 0, sharedHeap.contextAddress, 4096, 0, 0, 0}}) == B_BAD_VALUE);
	assert(Bind(client, vm.handle, {{kVmUnmapOperation, 0, 0x10000, 4096, 0, 0, 0}}) == B_OK);
	assert(AcquireClientVm(client, vm.handle, 0, withoutBuffer) == B_OK);
	Destroy(client, buffer.handle);
	HeapPhysicalOracle removed(withoutBuffer);
	assert(removed.leaves.count(0x10000) == 0 && HeapPhysicalOracle(withBuffer).leaves == combined.leaves);
	assert(removed.leaves.at(sharedHeap.contextAddress) == combined.leaves.at(sharedHeap.contextAddress));
	// A new leaf and a reused leaf both pass allocation/abort failure paths.
	for (bool newLeaf : {false, true}) {
		uint32 size = newLeaf ? 2 * 1024 * 1024 : 128 * 1024;
		auto failHeap = CreateHeapFor(client, vm.handle, size, 1);
		ClientVmLease owner{};
		assert(AcquireClientVm(client, vm.handle, 0, owner) == B_OK);
		auto before = HeapPhysicalOracle(owner).leaves;
		auto initial = HeapDetails(client);
		for (unsigned fail = 1; fail < 8; fail++) {
			HeapGrowth growth{};
			sHeapCalls = 0; sFailHeap = fail;
			status_t result = PrepareClientHeapGrowth(owner, failHeap.contextAddress, 1, 0, 0, growth);
			sFailHeap = 0;
			if (result == B_OK) { AbortClientHeapGrowth(growth); break; }
			assert(result == B_NO_MEMORY && growth.state == NULL);
			assert(HeapPhysicalOracle(owner).leaves == before);
			assert(HeapDetails(client).globalBytes == initial.globalBytes);
			assert(HeapDetails(client).globalTablePages == initial.globalTablePages);
			assert(fail != 7);
		}
		for (unsigned fail = 1; fail <= 6; fail++) {
			HeapGrowth growth{};
			sFailAllocation = fail;
			assert(PrepareClientHeapGrowth(owner, failHeap.contextAddress, 1, 0, 0, growth) != B_OK);
			sFailAllocation = 0;
			assert(growth.state == NULL && HeapPhysicalOracle(owner).leaves == before);
			assert(HeapDetails(client).globalBytes == initial.globalBytes);
		}
		DropHeap(client, vm.handle, failHeap.handle); ReleaseClientVm(owner);
	}
	DropHeap(client, vm.handle, sharedHeap.handle);
	ReleaseClientVm(withBuffer); ReleaseClientVm(withoutBuffer);
	assert(sHeaps == 0 && sBuffers == 0);
	// Reach the default 64-chunk maximum across leaf-table boundaries. Each
	// committed growth becomes reachable, while zero fallback preserves state.
	auto heap = CreateHeapFor(client, vm.handle, 132 * 1024, 1);
	ClientVmLease retained{};
	assert(AcquireClientVm(client, vm.handle, 0, retained) == B_OK);
	for (unsigned i = 1; i < 64; i++) {
		HeapGrowth growth{};
		assert(PrepareClientHeapGrowth(retained, heap.contextAddress, 1, 0, 0, growth) == B_OK);
		assert(CommitClientHeapGrowth(growth) == B_OK);
	}
	HeapGrowth full{};
	assert(PrepareClientHeapGrowth(retained, heap.contextAddress, 1, 0, 0, full) == B_NO_MEMORY);
	HeapPhysicalOracle grown(retained);
	assert(grown.leaves.size() == 1 + 64 * 33);
	for (unsigned i = 0; i < 64; i++) grown.Bytes(heap.contextAddress + kHeapChunksOffset + i * 132 * 1024, 132 * 1024);
	DropHeap(client, vm.handle, heap.handle); ReleaseClientVm(retained);
	auto limitedRequest = HeapRequest(vm.handle); limitedRequest.targetInFlight = 0;
	assert(Call(client, kCreateHeap, limitedRequest) == B_OK);
	assert(AcquireClientVm(client, vm.handle, 0, retained) == B_OK);
	assert(PrepareClientHeapGrowth(retained, limitedRequest.contextAddress, 1, 0, 0, full) == B_NO_MEMORY);
	DropHeap(client, vm.handle, limitedRequest.handle); ReleaseClientVm(retained);
	// A pending transaction alone retains the heap/account after client exit.
	heap = CreateHeapFor(client, vm.handle);
	assert(AcquireClientVm(client, vm.handle, 0, retained) == B_OK);
	HeapGrowth pending{};
	assert(PrepareClientHeapGrowth(retained, heap.contextAddress, 1, 0, 0, pending) == B_OK);
	FreeClient(client); ReleaseClientVm(retained);
	assert(sAccounts == 3 && sHeaps == 1 && sHeapChunks == 3);
	AbortClientHeapGrowth(pending);
	assert(sAccounts == 2 && sHeaps == 0 && sAreas.empty());
	FreeClient(foreign); FreeClient(readonly); Empty();
	// Resident quotas include retired public handles until the old generation
	// releases them. Global quota also remains charged after final client close.
	assert(OpenClient(true, &client) == B_OK && OpenClient(true, &foreign) == B_OK);
	assert(OpenClient(true, &readonly) == B_OK);
	vm = NewVm(client); auto vm2 = NewVm(foreign), vm3 = NewVm(readonly);
	std::vector<uint32> handles;
	for (unsigned i = 0; i < 128; i++) {
		handles.push_back(CreateHeapFor(client, vm.handle, 128 * 1024, 1).handle);
		CreateHeapFor(foreign, vm2.handle, 128 * 1024, 1);
	}
	assert(sHeaps == 256);
	auto overflow = HeapRequest(vm3.handle, 128 * 1024, 1);
	assert(Call(readonly, kCreateHeap, overflow) == B_NO_MEMORY);
	assert(AcquireClientVm(client, vm.handle, 0, retained) == B_OK);
	DropHeap(client, vm.handle, handles[0]);
	auto reused = HeapRequest(vm.handle, 128 * 1024, 1);
	assert(Call(client, kCreateHeap, reused) == B_NO_MEMORY);
	ReleaseClientVm(retained);
	assert(Call(client, kCreateHeap, reused) == B_OK && reused.contextAddress == kHeapBase);
	assert(AcquireClientVm(client, vm.handle, 0, retained) == B_OK);
	FreeClient(client);
	assert(sHeaps == 256 && Call(readonly, kCreateHeap, overflow) == B_NO_MEMORY);
	ReleaseClientVm(retained);
	assert(sHeaps == 128 && Call(readonly, kCreateHeap, overflow) == B_OK);
	FreeClient(foreign); FreeClient(readonly); Empty();
	printf("MALI_CSF_HEAPS_TEST_PASS metadata_failures=%u area_failures=%u growth_limit=64 resident_limit=256\n", metadataCases, areaCases);
}
