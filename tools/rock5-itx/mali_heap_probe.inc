/* Native heap checks, using the persistent queue and synchronization helpers. */
#include "mali_heap_commands.h"
static HeapInfo
HeapState(int fd, uint32_t vm = 0, uint32_t handle = 0)
{
	HeapInfo info{}; info.version = 1; info.vm = vm; info.handle = handle;
	CHECK(Call(fd, kGetHeapInfo, info) == 0); return info;
}
static HeapCreate
MakeHeap(Context& context, uint32_t bytes = 2 * 1024 * 1024, uint32_t initial = 5)
{
	HeapCreate request{}; request.version = 1; request.vm = context.vm;
	request.chunkSize = bytes; request.initialChunks = initial; request.maxChunks = 64;
	request.targetInFlight = 65535;
	CHECK(Call(context.fd, kCreateHeap, request) == 0);
	CHECK(request.handle != 0 && request.generation == context.generation + 1);
	context.generation = request.generation;
	auto info = HeapState(context.fd, context.vm, request.handle);
	CHECK(info.contextAddress == request.contextAddress && info.firstChunkAddress == request.firstChunkAddress);
	CHECK(info.chunkCount == initial && info.bytes == uint64_t(bytes) * initial + 4096);
	return request;
}
static void
DropHeap(Context& context, uint32_t handle)
{
	HeapHandle request{}; request.version = 1; request.vm = context.vm; request.handle = handle;
	CHECK(Call(context.fd, kDestroyHeap, request) == 0 && request.generation == context.generation + 1);
	context.generation = request.generation;
}
static void
HeapSameCounts(const HeapInfo& a, const HeapInfo& b)
{
	CHECK(a.clientHeaps == b.clientHeaps && a.globalHeaps == b.globalHeaps
		&& a.globalChunks == b.globalChunks && a.globalTablePages == b.globalTablePages
		&& a.clientBytes == b.clientBytes && a.globalBytes == b.globalBytes
		&& a.globalHeapGenerations == b.globalHeapGenerations);
}
static unsigned
HeapAreas()
{
	return Areas("Mali CSF heap context") + Areas("Mali CSF heap chunk")
		+ Areas("Mali CSF heap page tables") + Areas("Mali CSF heap membership table");
}
static void
RunNativeHeaps(int fd)
{
	auto baseline = HeapState(fd); auto buffers = Buffers(fd); auto vms = Vms(fd);
	auto sync = SyncState(fd); unsigned areas = HeapAreas(), queues = Areas("Mali CSF queue DMA");
	Context context(fd, 0x67124ace, NULL), independent(fd, 0x4329cb7a, NULL);
	// Readable-only request memory reaches the final copyout and must leave
	// heap membership, VM generation, allocations and counters untouched.
	void* protectedRequest = NULL;
	area_id protectedArea = create_area("Mali heap copyout", &protectedRequest, B_ANY_ADDRESS,
		4096, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	CHECK(protectedArea >= B_OK);
	HeapCreate denied{}; denied.version = 1; denied.vm = context.vm; denied.chunkSize = 131072;
	denied.initialChunks = 1; denied.maxChunks = 64; denied.targetInFlight = 65535;
	memcpy(protectedRequest, &denied, sizeof(denied));
	CHECK(set_area_protection(protectedArea, B_READ_AREA) == B_OK);
	CHECK(ioctl(fd, kCreateHeap, protectedRequest, sizeof(denied)) != 0 && errno == EFAULT);
	CHECK(delete_area(protectedArea) == B_OK);
	HeapSameCounts(baseline, HeapState(fd)); CHECK(HeapAreas() == areas);
	Buffer readback; readback.Allocate(fd, 65536);
	context.generation = Bind(fd, context.vm, {{kVmMapOperation, kVmNoExecute,
		kHeapReadbackAddress, 65536, readback.handle, 0, 0}});
	auto heap = MakeHeap(context);
	HeapSample* samples = (HeapSample*)calloc(8192, sizeof(HeapSample)); CHECK(samples != NULL);
	unsigned count = 0;
	for (unsigned i = 0; i < 4; i++) samples[count++] = {heap.contextAddress + i * 8, 0};
	for (unsigned chunk = 0; chunk < heap.initialChunks; chunk++) {
		uint64_t address = heap.contextAddress + kHeapChunksOffset + uint64_t(chunk) * heap.chunkSize;
		uint64_t next = chunk ? (address - heap.chunkSize) | (heap.chunkSize >> 12) : 0;
		for (unsigned i = 1; i < 8; i++) samples[count++] = {address + i * 8, 0};
		for (unsigned offset = 0; offset < heap.chunkSize; offset += 4096) {
			samples[count++] = {address + offset, offset == 0 ? next : 0};
			samples[count++] = {address + offset + 4088, 0};
		}
	}
	CHECK(count == 5159);
	uint64_t* words = (uint64_t*)readback.Data();
	const uint64_t guard = UINT64_C(0xda715bc3932e56a4);
	for (unsigned i = 0; i < 8192; i++) words[i] = guard;
	unsigned copied = 0, batches = 0;
	while (copied < count) {
		unsigned amount = count - copied < 512 ? count - copied : 512;
		unsigned bytes = HeapCopyProgram((uint64_t*)context.stream.Data(), samples, copied, amount);
		CHECK(bytes != 0);
		context.Wait(context.Submit(kStreamAddress, bytes));
		copied += amount; batches++;
		for (unsigned i = 0; i < 8192; i++) CHECK(words[i] == (i < copied ? samples[i].expected : guard));
	}
	CHECK(batches == 11);
	printf("ROCK5_MALI_HEAP_MEMORY_PASS heaps=1 initial_chunks=5 chunk_bytes=2097152 pages=2560 samples=%u guards=%u batches=%u copyout_rollback=1\n",
		count, 8192 - count, batches);
	// Execute Mesa's HEAP_SET and an actual GPU write into private heap data.
	// The opaque context is not inspected after firmware starts using it.
	uint64_t markerAddress = heap.contextAddress + kHeapChunksOffset + 128;
	const uint64_t marker = UINT64_C(0x0b7ca513de29);
	uint64_t initialize[] = {
		UINT64_C(0x0300000000ff0000), UINT64_C(0x1700000000000002),
		UINT64_C(0x0148000000000000) | heap.contextAddress, UINT64_C(0x3000480000000000),
		UINT64_C(0x0100000000000000) | markerAddress, UINT64_C(0x0102000000000000) | marker,
		UINT64_C(0x1502000000030000), UINT64_C(0x0300000000010000)
	};
	memcpy(context.stream.Data(), initialize, sizeof(initialize));
	context.Wait(context.Submit(kStreamAddress, sizeof(initialize)));
	for (unsigned i = 0; i < 8192; i++) words[i] = guard;
	samples[0] = {markerAddress, marker}; samples[1] = {markerAddress, 0};
	// A pending producer fence holds the heap consumer while membership changes.
	// Its command bytes remain intact in a distinct half of the stream buffer.
	uint32_t gate = MakeSync(fd, true);
	Buffer longStream; longStream.Allocate(fd, 1024 * 1024); memset(longStream.Data(), 0, 1024 * 1024);
	const uint64_t longAddress = UINT64_C(0x130000000);
	independent.generation = Bind(fd, independent.vm, {{kVmMapOperation, kVmNoExecute,
		longAddress, 1024 * 1024, longStream.handle, 0, 0}});
	for (unsigned i = 0; i < 32; i++) independent.Submit(longAddress, 1024 * 1024);
	SubmitSync(independent, 0, 0, {}, {{gate, 0, 0}});
	unsigned bytes = HeapCopyProgram((uint64_t*)context.stream.Data(), samples, 0, 1);
	CHECK(bytes != 0);
	SubmitSync(context, kStreamAddress, bytes, {{gate, 0, 0}}, {});
	unsigned pending = Info(fd, independent.queue).pending; CHECK(pending > 0);
	CHECK(Info(fd, context.queue).pending == 1);
	DropHeap(context, heap.handle);
	auto replacement = MakeHeap(context, 2 * 1024 * 1024, 1);
	CHECK(replacement.contextAddress == heap.contextAddress && replacement.handle != heap.handle);
	__sync_synchronize();
	CHECK(Info(fd, context.queue).pending == 1 && words[0] == guard);
	CHECK(HeapState(fd).globalHeaps == baseline.globalHeaps + 2);
	bytes = HeapCopyProgram((uint64_t*)((uint8_t*)context.stream.Data() + 32768), samples, 1, 1);
	CHECK(bytes != 0);
	context.Submit(kStreamAddress + 32768, bytes);
	context.Wait(context.sequence); independent.Wait(independent.sequence);
	CHECK(words[0] == marker && words[1] == 0);
	for (unsigned i = 2; i < 8192; i++) CHECK(words[i] == guard);
	CHECK(HeapState(fd).globalHeaps == baseline.globalHeaps + 1);
	printf("ROCK5_MALI_HEAP_GENERATION_PASS old_value=%" B_PRIx64 " new_value=0 reused_va=1 producer_pending=%u guards=8190 heap_set=1\n", marker, pending);
	DropSync(fd, gate); longStream.Drop(fd); longStream.Unmap();
	readback.Drop(fd); readback.Unmap(); free(samples);
	uint64_t checked = context.sequence + independent.sequence;
	CHECK(context.sequence == 14 && independent.sequence == 33 && checked == 47);
	context.Finish(); independent.Finish();
	HeapSameCounts(baseline, HeapState(fd)); CHECK(HeapAreas() == areas);
	pid_t children[2]; int ready[2][2], control[2][2];
	for (unsigned child = 0; child < 2; child++) {
		CHECK(pipe(ready[child]) == 0 && pipe(control[child]) == 0);
		children[child] = fork(); CHECK(children[child] >= 0);
		if (children[child] == 0) {
			close(ready[child][0]); close(control[child][1]);
			int childFd = open(kDevice, O_RDWR); CHECK(childFd >= 0);
			Context owned(childFd, 0x671293ab + child, NULL);
			auto first = MakeHeap(owned, 132 * 1024, 3); MakeHeap(owned);
			Buffer work; work.Allocate(childFd, 1024 * 1024); memset(work.Data(), 0, 1024 * 1024);
			owned.generation = Bind(childFd, owned.vm, {{kVmMapOperation, kVmNoExecute,
				longAddress, 1024 * 1024, work.handle, 0, 0}});
			uint64_t* last = (uint64_t*)((uint8_t*)work.Data() + 1024 * 1024 - 16);
			last[0] = UINT64_C(0x0148000000000000) | first.contextAddress;
			last[1] = UINT64_C(0x3000480000000000);
			for (unsigned i = 0; i < 32; i++) owned.Submit(longAddress, 1024 * 1024);
			work.Drop(childFd); owned.DropHandles();
			unsigned pending = Info(childFd, owned.queue).pending;
			CHECK(pending > 0 && write(ready[child][1], &pending, sizeof(pending)) == sizeof(pending));
			char value; CHECK(read(control[child][0], &value, 1) == 1); _exit(0);
		}
		close(ready[child][1]); close(control[child][0]);
	}
	unsigned childPending[2];
	for (unsigned i = 0; i < 2; i++) CHECK(read(ready[i][0], &childPending[i], sizeof(unsigned)) == sizeof(unsigned));
	CHECK(childPending[0] > 0 && childPending[1] > 0);
	CHECK(HeapState(fd).globalHeaps == baseline.globalHeaps + 4);
	CHECK(write(control[0][1], "x", 1) == 1 && kill(children[1], SIGKILL) == 0);
	for (unsigned i = 0; i < 2; i++) {
		int status; CHECK(waitpid(children[i], &status, 0) == children[i]);
		CHECK(i == 0 ? WIFEXITED(status) && WEXITSTATUS(status) == 0 : WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		close(ready[i][0]); close(control[i][1]);
	}
	HeapSameCounts(baseline, HeapState(fd)); SyncSameCounts(sync, SyncState(fd));
	auto afterBuffers = Buffers(fd); auto afterVms = Vms(fd);
	CHECK(afterBuffers.globalClients == buffers.globalClients && afterBuffers.globalBuffers == buffers.globalBuffers);
	CHECK(afterVms.globalVms == vms.globalVms && afterVms.globalGenerations == vms.globalGenerations
		&& afterVms.globalTablePages == vms.globalTablePages);
	CHECK(HeapAreas() == areas && Areas("Mali CSF queue DMA") == queues);
	printf("ROCK5_MALI_HEAP_EXIT_PASS normal=1 killed=1 heaps=4 pending=%u,%u leaked=0\n", childPending[0], childPending[1]);
	puts("ROCK5_MALI_HEAP_PASS contexts=4 submitted=111 completed_checked=47 heaps_created=6 firmware_oom_exercised=0 rendered=0");
}
