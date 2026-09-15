/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfQueue.h"
#include "CsfShader.h"
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace MaliCSF;
static const char* kDevice = "/dev/graphics/mali_csf/0";
static const uint64_t kStreamAddress = UINT64_C(0x120000000);
static void Check(bool passed, int line)
{
	if (!passed) {
		fprintf(stderr, "ROCK5_MALI_QUEUE_FAIL line=%d errno=%d\n", line, errno);
		exit(1);
	}
}
#define CHECK(value) Check((value), __LINE__)
template<typename T> static int Call(int fd, uint32_t op, T& request)
{
	return ioctl(fd, op, &request, sizeof(request));
}
static ClientInfo Buffers(int fd)
{
	ClientInfo info{}; info.version = 1; CHECK(Call(fd, kGetClientInfo, info) == 0); return info;
}
static VmInfo Vms(int fd)
{
	VmInfo info{}; info.version = 1; CHECK(Call(fd, kGetVmInfo, info) == 0); return info;
}
static QueueInfo Info(int fd, uint32_t handle)
{
	QueueInfo info{}; info.version = 1; info.handle = handle;
	CHECK(Call(fd, kGetQueueInfo, info) == 0); return info;
}
static unsigned Areas(const char* name)
{
	ssize_t cookie = 0; area_info area; unsigned count = 0;
	while (get_next_area_info(B_SYSTEM_TEAM, &cookie, &area) == B_OK)
		if (strcmp(area.name, name) == 0) count++;
	return count;
}
struct Buffer {
	uint32_t handle = 0;
	BufferMap view{};
	void Allocate(int fd, unsigned bytes)
	{
		BufferCreate create{1, 0, bytes, 0, 0, 0}; CHECK(Call(fd, kCreateBuffer, create) == 0);
		handle = create.handle; view.version = 1; view.handle = handle;
		CHECK(Call(fd, kMapBuffer, view) == 0);
	}
	void Drop(int fd)
	{
		if (handle == 0) return;
		BufferHandle request{1, handle, 0}; CHECK(Call(fd, kDestroyBuffer, request) == 0); handle = 0;
	}
	void Unmap() { CHECK(delete_area(view.area) == B_OK); }
	void* Data() const { return (void*)(uintptr_t)view.address; }
};
struct Batch { VmBind header; VmOperation operations[8]; };
static uint64_t Bind(int fd, uint32_t vm, std::initializer_list<VmOperation> operations)
{
	Batch batch{}; batch.header = {1, vm, uint32_t(operations.size()), 0, 0, 0};
	unsigned i = 0; for (const auto& op : operations) batch.operations[i++] = op;
	CHECK(i <= 8 && ioctl(fd, kBindVm, &batch, sizeof(VmBind) + i * sizeof(VmOperation)) == 0);
	return batch.header.newGeneration;
}
static uint32_t CreateQueue(int fd, uint32_t vm, const char* firmware)
{
	uint32_t bytes = 0; FILE* file = NULL;
	if (firmware != NULL) {
		file = fopen(firmware, "rb"); CHECK(file != NULL);
		struct stat info; CHECK(fstat(fileno(file), &info) == 0 && info.st_size > 0 && info.st_size <= 2 * 1024 * 1024);
		bytes = info.st_size;
	}
	QueueCreate* request = (QueueCreate*)calloc(1, sizeof(QueueCreate) + bytes); CHECK(request != NULL);
	request->version = 1; request->vm = vm; request->firmwareBytes = bytes;
	if (file != NULL) {
		CHECK(fread(request + 1, 1, bytes, file) == bytes); CHECK(fclose(file) == 0);
	}
	printf("ROCK5_MALI_QUEUE_CREATE vm=%u firmware_bytes=%u\n", vm, bytes);
	CHECK(ioctl(fd, kCreateQueue, request, sizeof(QueueCreate) + bytes) == 0);
	uint32_t handle = request->handle; CHECK(handle != 0); free(request); return handle;
}
static void DropVm(int fd, uint32_t vm)
{
	VmHandle request{1, vm, 0}; CHECK(Call(fd, kDestroyVm, request) == 0);
}
struct Context {
	int fd;
	uint32_t vm, queue, seed;
	uint64_t generation, sequence = 0;
	Buffer shader, stream, data, replacement;
	bool replaced = false;
	Context(int descriptor, unsigned value, const char* firmware) : fd(descriptor), seed(value)
	{
		VmCreate create{1, 0, 0, 0, 0, 0}; CHECK(Call(fd, kCreateVm, create) == 0); vm = create.handle;
		shader.Allocate(fd, 4096); stream.Allocate(fd, 65536); data.Allocate(fd, 8192);
		BuildStoreShader(shader.Data()); memset(stream.Data(), 0, 65536);
		generation = Bind(fd, vm, {
			{kVmMapOperation, kVmReadOnly, kCommandAddress, 4096, shader.handle, 0, 0},
			{kVmMapOperation, kVmNoExecute, kStreamAddress, 65536, stream.handle, 0, 0},
			{kVmMapOperation, kVmNoExecute, kDataAddress, 8192, data.handle, 0, 0}});
		__sync_synchronize(); queue = CreateQueue(fd, vm, firmware);
	}
	uint32_t* Data() { return (uint32_t*)(replaced ? replacement.Data() : data.Data()); }
	uint64_t Submit(uint64_t address, unsigned bytes, uint64_t expected = 0)
	{
		__sync_synchronize();
		QueueSubmit request{1, queue, expected, address, bytes, 0, 0, 0};
		CHECK(Call(fd, kSubmitQueue, request) == 0);
		CHECK(request.sequence == ++sequence && request.generation == generation); return sequence;
	}
	void Wait(uint64_t target)
	{
		QueueWait wait{1, queue, target, 10000000, 0, 0, 0};
		CHECK(Call(fd, kWaitQueue, wait) == 0);
		if (wait.result != B_OK) {
			auto info = Info(fd, queue);
			printf("ROCK5_MALI_QUEUE_ERROR handle=%u result=%" B_PRId32 " state=%u submitted=%" B_PRIu64
				" completed=%" B_PRIu64 "\n", queue, wait.result, info.state, info.submitted, info.completed);
		}
		CHECK(wait.result == B_OK && wait.completed >= target); __sync_synchronize();
	}
	unsigned Program(uint64_t* code, unsigned round, bool initialize, unsigned word = 31)
	{
		unsigned n = 0;
		code[n++] = UINT64_C(0x0300000000ff0000);
		code[n++] = UINT64_C(0x1700000000000002);
		if (initialize) code[n++] = UINT64_C(0x0250000000000000) | seed; // persistent r80
		code[n++] = UINT64_C(0x0100000000000000) | kDataAddress;
		code[n++] = UINT64_C(0x1550000000010004); // STORE r80 at data+4
		code[n++] = UINT64_C(0x0300000000010000);
		code[n++] = UINT64_C(0x0202000000000000) | (seed ^ (round * 0x174123u));
		code[n++] = UINT64_C(0x1502000000010000) | (word * 4);
		code[n++] = UINT64_C(0x0300000000010000);
		return n * 8;
	}
	void Store(unsigned round, bool initialize = false)
	{
		for (unsigned i = 0; i < 2048; i++) Data()[i] = CommandGuard(round, i);
		unsigned bytes = Program((uint64_t*)stream.Data(), round, initialize);
		Wait(Submit(kStreamAddress, bytes));
		for (unsigned i = 0; i < 2048; i++)
			CHECK(Data()[i] == (i == 1 ? seed : i == 31 ? seed ^ (round * 0x174123u) : CommandGuard(round, i)));
	}
	void Replace()
	{
		replacement.Allocate(fd, 8192);
		generation = Bind(fd, vm, {{kVmMapOperation, kVmNoExecute, kDataAddress, 8192, replacement.handle, 0, 0}});
		data.Drop(fd); replaced = true;
	}
	void Compute(unsigned round)
	{
		for (unsigned i = 0; i < 2048; i++) Data()[i] = CommandGuard(round, i);
		Wait(Submit(kCommandAddress + round * 256, 256));
		for (unsigned i = 0; i < 2048; i++) {
			uint32_t expected = CommandGuard(round, i);
			for (unsigned j = 0; j < 8; j++) if (i == kStoreWordIndices[j]) expected = CommandValue(round, j);
			CHECK(Data()[i] == expected);
		}
	}
	void DropHandles()
	{
		shader.Drop(fd); stream.Drop(fd); data.Drop(fd); replacement.Drop(fd); DropVm(fd, vm);
	}
	void Finish()
	{
		QueueHandle request{1, queue, 0}; CHECK(Call(fd, kDestroyQueue, request) == 0);
		DropHandles(); shader.Unmap(); stream.Unmap(); data.Unmap(); if (replaced) replacement.Unmap();
	}
};

int main(int argc, char** argv)
{
	setbuf(stdout, NULL);
	int fd = open(kDevice, O_RDWR);
	if (fd < 0 && errno == ENOENT) { puts("ROCK5_MALI_QUEUE_NO_DEVICE"); return 77; }
	CHECK(fd >= 0 && argc == 2);
	auto baseline = Buffers(fd); auto baselineVms = Vms(fd);
	CHECK(baseline.capabilities == (kClientCpuBuffers | kClientVmMappings | kClientQueues));
	CHECK(Areas("Mali CSF firmware DMA") == 0 && Areas("Mali CSF queue DMA") == 0);
	Context a(fd, 0x61b256ad, argv[1]), b(fd, 0x9852da31, NULL);
	a.Store(0, true); b.Store(0, true); a.Store(1); a.Replace(); a.Store(2); b.Store(1);
	CHECK(Info(fd, a.queue).activeGeneration == a.generation);
	puts("ROCK5_MALI_QUEUE_CONTEXT_PASS contexts=2 vm_changes=1 persistent_register=80");
	for (unsigned i = 0; i < 520; i++) {
		a.Store(10 + i);
		if ((i + 1) % 128 == 0) printf("ROCK5_MALI_QUEUE_PROGRESS stores=%u\n", i + 1);
	}
	a.Compute(0); b.Compute(0); a.Compute(1); b.Compute(1);
	a.Store(600); b.Store(600); a.Wait(a.Submit(0, 0));
	auto ai = Info(fd, a.queue), bi = Info(fd, b.queue);
	CHECK(ai.completed == a.sequence && bi.completed == b.sequence && ai.pending == 0 && bi.pending == 0);
	CHECK(ai.insert > 65536 && ai.extract == ai.insert && ai.insert == ai.completed * 128);
	CHECK(ai.suspends > 0 && ai.resumes > 0 && bi.suspends > 0 && bi.resumes > 0);
	CHECK(ai.interrupts >= ai.completed && ai.syncEvents >= ai.completed);
	printf("ROCK5_MALI_QUEUE_EXECUTION_PASS a=%" B_PRIu64 " b=%" B_PRIu64
		" insert=%" B_PRIu64 " extract=%" B_PRIu64 " shaders=4 empty=1 interrupts=%u sync=%u suspends=%u resumes=%u\n",
		ai.completed, bi.completed, ai.insert, ai.extract, ai.interrupts, ai.syncEvents, ai.suspends, ai.resumes);
	// Preserve the exact immutable generation of already submitted work across
	// a mapping update. The old target stays inspectable through its CPU alias.
	Context retained(fd, 0x153790ab, NULL);
	retained.Store(0, true);
	uint32_t* oldData = retained.Data();
	for (unsigned i = 0; i < 2048; i++) oldData[i] = 0x7a7a7a7a;
	uint64_t oldGeneration = retained.generation;
	for (unsigned i = 0; i < 32; i++) {
		uint64_t* code = (uint64_t*)((uint8_t*)retained.stream.Data() + i * 256);
		unsigned bytes = retained.Program(code, i + 1, false, 32 + i);
		retained.Submit(kStreamAddress + i * 256, bytes, oldGeneration);
	}
	retained.Replace(); retained.Wait(retained.sequence);
	CHECK(oldData[1] == retained.seed);
	for (unsigned i = 0; i < 2048; i++) if (i != 1) {
		uint32_t expected = i >= 32 && i < 64 ? retained.seed ^ ((i - 31) * 0x174123u) : 0x7a7a7a7a;
		CHECK(oldData[i] == expected);
	}
	retained.Store(100);
	QueueSubmit stale{1, retained.queue, oldGeneration, kStreamAddress, 8, 0, 0, 0};
	CHECK(Call(fd, kSubmitQueue, stale) != 0 && errno == EBUSY);
	retained.Finish();
	puts("ROCK5_MALI_QUEUE_GENERATION_PASS queued=32 replaced=1 stale_rejected=1");

	auto liveBuffers = Buffers(fd); auto liveVms = Vms(fd);
	unsigned liveAreas = Areas("Mali CSF queue DMA");
	pid_t children[2]; int ready[2][2], control[2][2];
	for (unsigned i = 0; i < 2; i++) {
		CHECK(pipe(ready[i]) == 0 && pipe(control[i]) == 0);
		children[i] = fork(); CHECK(children[i] >= 0);
		if (children[i] == 0) {
			close(ready[i][0]); close(control[i][1]);
			QueueInfo inherited{}; inherited.version = 1; inherited.handle = a.queue;
			CHECK(Call(fd, kGetQueueInfo, inherited) != 0 && errno == EPERM);
			int childFd = open(kDevice, O_RDWR); CHECK(childFd >= 0);
			Context context(childFd, 0x16734acd + i, NULL);
			// A long stream of valid NOPs leaves time to observe pending work.
			context.Program((uint64_t*)((uint8_t*)context.stream.Data() + 65536 - 128), 0, true);
			for (unsigned n = 0; n < 32; n++) context.Submit(kStreamAddress, 65536);
			context.DropHandles();
			uint32_t pending = Info(childFd, context.queue).pending;
			CHECK(write(ready[i][1], &pending, sizeof(pending)) == sizeof(pending));
			char value; CHECK(read(control[i][0], &value, 1) == 1); _exit(0);
		}
		close(ready[i][1]); close(control[i][0]);
	}
	unsigned pending[2];
	for (unsigned i = 0; i < 2; i++) CHECK(read(ready[i][0], &pending[i], sizeof(pending[i])) == sizeof(pending[i]));
	CHECK(Areas("Mali CSF queue DMA") == liveAreas + 2);
	CHECK(write(control[0][1], "x", 1) == 1 && kill(children[1], SIGKILL) == 0);
	for (unsigned i = 0; i < 2; i++) {
		int status; CHECK(waitpid(children[i], &status, 0) == children[i]);
		CHECK(i == 1 ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL : WIFEXITED(status) && WEXITSTATUS(status) == 0);
		close(ready[i][0]); close(control[i][1]);
	}
	auto after = Buffers(fd); auto afterVms = Vms(fd);
	CHECK(after.globalClients == liveBuffers.globalClients && after.globalBuffers == liveBuffers.globalBuffers);
	CHECK(afterVms.globalVms == liveVms.globalVms && afterVms.globalGenerations == liveVms.globalGenerations
		&& afterVms.globalTablePages == liveVms.globalTablePages);
	CHECK(Areas("Mali CSF queue DMA") == liveAreas);
	printf("ROCK5_MALI_QUEUE_EXIT_PASS normal=1 killed=1 pending_at_ready=%u,%u leaked=0\n", pending[0], pending[1]);
	a.Store(700); b.Store(700); a.Finish(); b.Finish();
	after = Buffers(fd); afterVms = Vms(fd);
	CHECK(after.globalBuffers == baseline.globalBuffers && after.globalClients == baseline.globalClients);
	CHECK(afterVms.globalVms == baselineVms.globalVms && afterVms.globalGenerations == baselineVms.globalGenerations
		&& afterVms.globalTablePages == baselineVms.globalTablePages);
	CHECK(Areas("Mali CSF queue DMA") == 0 && Areas("Mali CSF firmware DMA") == 0);
	CHECK(Areas("Mali CSF VM page tables") == baselineVms.globalGenerations);
	CHECK(Areas("Mali CSF client buffer") == baseline.globalBuffers);
	CHECK(close(fd) == 0);
	puts("ROCK5_MALI_QUEUE_PASS persistent_firmware=1 contexts=5 submitted=632 completed_checked=568 rendered=0");
	return 0;
}
