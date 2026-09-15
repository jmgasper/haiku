/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfQueue.h"
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace MaliCSF;
static const char* kDevice = "/dev/graphics/mali_csf/0";
static void Check(bool passed, int line)
{
	if (!passed) {
		fprintf(stderr, "ROCK5_MALI_VM_FAIL line=%d errno=%d\n", line, errno);
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
	ClientInfo info{}; info.version = 1;
	CHECK(Call(fd, kGetClientInfo, info) == 0);
	return info;
}
static VmInfo Info(int fd, uint32_t handle = 0)
{
	VmInfo info{}; info.version = 1; info.handle = handle;
	CHECK(Call(fd, kGetVmInfo, info) == 0);
	return info;
}
static uint32_t NewVm(int fd)
{
	VmCreate request{1, 0, 0, 0, 0, 0};
	CHECK(Call(fd, kCreateVm, request) == 0 && request.handle != 0);
	CHECK(request.userLimit == kVmUserLimit);
	return request.handle;
}
static uint32_t NewBuffer(int fd)
{
	BufferCreate request{1, 0, 65536, 0, 0, 0};
	CHECK(Call(fd, kCreateBuffer, request) == 0 && request.handle != 0);
	return request.handle;
}
struct Batch {
	VmBind header;
	VmOperation operations[kMaxVmOperations];
};
static int Bind(int fd, uint32_t handle, std::initializer_list<VmOperation> operations,
	uint64_t expected = 0)
{
	CHECK(operations.size() <= kMaxVmOperations);
	Batch batch{};
	batch.header = {1, handle, uint32_t(operations.size()), 0, expected, 0};
	unsigned i = 0;
	for (const auto& operation : operations) batch.operations[i++] = operation;
	int result = ioctl(fd, kBindVm, &batch, sizeof(VmBind) + i * sizeof(VmOperation));
	if (result == 0) CHECK(batch.header.newGeneration == Info(fd, handle).generation);
	return result;
}
static void DropBuffer(int fd, uint32_t handle)
{
	BufferHandle request{1, handle, 0};
	CHECK(Call(fd, kDestroyBuffer, request) == 0);
	CHECK(Call(fd, kDestroyBuffer, request) != 0 && errno == ENOENT);
}
static void DropVm(int fd, uint32_t handle)
{
	VmHandle request{1, handle, 0};
	CHECK(Call(fd, kDestroyVm, request) == 0);
	CHECK(Call(fd, kDestroyVm, request) != 0 && errno == ENOENT);
}
static unsigned KernelAreas(const char* name)
{
	ssize_t cookie = 0; area_info area; unsigned count = 0;
	while (get_next_area_info(B_SYSTEM_TEAM, &cookie, &area) == B_OK)
		if (strcmp(area.name, name) == 0) count++;
	return count;
}
static void Wait(pid_t child, bool killed)
{
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
		: WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main()
{
	setbuf(stdout, NULL);
	int fd = open(kDevice, O_RDWR);
	if (fd < 0 && errno == ENOENT) { puts("ROCK5_MALI_VM_NO_DEVICE"); return 77; }
	CHECK(fd >= 0);
	auto baseline = Info(fd); auto baselineBuffers = Buffers(fd);
	CHECK(baseline.clientVms == 0 && baselineBuffers.bufferCount == 0);
	CHECK(baselineBuffers.capabilities == (kClientCpuBuffers | kClientVmMappings | kClientQueues | kClientSynchronization | kClientHeaps));
	unsigned baselineAreas = KernelAreas("Mali CSF VM page tables");
	unsigned baselineBufferAreas = KernelAreas("Mali CSF client buffer");
	CHECK(baselineAreas == baseline.globalGenerations && baselineBufferAreas == baselineBuffers.globalBuffers);
	int other = open(kDevice, O_RDWR); CHECK(other >= 0);
	int readonly = open(kDevice, O_RDONLY); CHECK(readonly >= 0);
	VmCreate denied{1, 0, 0, 0, 0, 0};
	CHECK(Call(readonly, kCreateVm, denied) != 0 && errno == EPERM);
	CHECK(close(readonly) == 0);
	uint32_t vm = NewVm(fd), first = NewBuffer(fd), second = NewBuffer(fd);
	BufferMap view{}; view.version = 1; view.handle = first;
	CHECK(Call(fd, kMapBuffer, view) == 0);
	memset((void*)(uintptr_t)view.address, 0x78, view.bytes);
	VmInfo foreign{}; foreign.version = 1; foreign.handle = vm;
	CHECK(Call(other, kGetVmInfo, foreign) != 0 && errno == ENOENT);
	VmHandle foreignHandle{1, vm, 0};
	CHECK(Call(other, kDestroyVm, foreignHandle) != 0 && errno == ENOENT);
	CHECK(Info(fd, vm).generation == 1 && Info(fd, vm).tablePages == 1);
	uint64_t address = (UINT64_C(1) << 39) - 8192;
	VmOperation map{kVmMapOperation, kVmNoExecute, address, 16384, first, 0, 4096};
	CHECK(Bind(fd, vm, {map}, 99) != 0 && errno == EBUSY);
	CHECK(Bind(fd, vm, {map}, 1) == 0);
	auto mapped = Info(fd, vm);
	CHECK(mapped.generation == 2 && mapped.mappings == 1 && mapped.mappedBytes == 16384);
	CHECK(mapped.tablePages == 7 && mapped.globalGenerations == baseline.globalGenerations + 1);
	VmOperation replace{kVmMapOperation, kVmReadOnly | kVmUncached,
		address + 4096, 8192, second, 0, 8192};
	VmOperation invalid = replace; invalid.offset = 65536;
	CHECK(Bind(fd, vm, {replace, invalid}) != 0 && errno == EINVAL);
	CHECK(Info(fd, vm).generation == 2 && Info(fd, vm).mappings == 1);
	// Readable request RAM with no write permission forces the final copyout to
	// fail, after the candidate tables have been built. Verify rollback counts.
	void* output = NULL;
	area_id area = create_area("Mali VM read-only request", &output, B_ANY_ADDRESS,
		4096, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	CHECK(area >= 0);
	Batch* request = (Batch*)output;
	request->header = {1, vm, 1, 0, 2, 0}; request->operations[0] = replace;
	CHECK(set_area_protection(area, B_READ_AREA) == B_OK);
	CHECK(ioctl(fd, kBindVm, request, sizeof(VmBind) + sizeof(VmOperation)) != 0 && errno == EFAULT);
	CHECK(Info(fd, vm).generation == 2 && Info(fd, vm).mappings == 1);
	CHECK(Info(fd).globalTablePages == baseline.globalTablePages + 7);
	CHECK(KernelAreas("Mali CSF VM page tables") == baselineAreas + 1);
	CHECK(delete_area(area) == B_OK);
	CHECK(Bind(fd, vm, {replace}, 2) == 0);
	CHECK(Info(fd, vm).generation == 3 && Info(fd, vm).mappings == 3);
	DropBuffer(fd, first); DropBuffer(fd, second);
	CHECK(Buffers(fd).bufferCount == 2 && Buffers(fd).bufferBytes == 131072);
	CHECK(KernelAreas("Mali CSF client buffer") == baselineBufferAreas + 2);
	CHECK(Bind(fd, vm, {map}) != 0 && errno == ENOENT);
	VmOperation unmap{kVmUnmapOperation, 0, address + 8192, 8192, 0, 0, 0};
	CHECK(Bind(fd, vm, {unmap}) == 0);
	CHECK(Info(fd, vm).mappings == 2 && Info(fd, vm).mappedBytes == 8192);
	CHECK(Info(fd, vm).tablePages == 4);
	CHECK(Bind(fd, vm, {{kVmUnmapOperation, 0, address, 4096, 0, 0, 0}}) == 0);
	CHECK(Buffers(fd).bufferCount == 1 && Info(fd, vm).mappings == 1);
	DropVm(fd, vm);
	CHECK(Buffers(fd).bufferCount == 0 && Info(fd).clientVms == 0);
	// Releasing the GPU mapping leaves an independently owned CPU area intact.
	for (size_t i = 0; i < view.bytes; i++) CHECK(((volatile uint8_t*)(uintptr_t)view.address)[i] == 0x78);
	CHECK(delete_area(view.area) == B_OK);
	CHECK(close(other) == 0);
	puts("ROCK5_MALI_VM_MAPPING_PASS split=1 partial_unmap=1 retained_buffers=2"
		" invalid_batch_rollback=1 copyout_rollback=1 cpu_alias_survives=1");
	// 48 buffers survive removal of their handles through 12 VMs in four teams.
	// Both normal exit and forced exit must release tables and resident BOs.
	pid_t children[4]; int ready[4][2], control[4][2];
	for (unsigned n = 0; n < 4; n++) {
		CHECK(pipe(ready[n]) == 0 && pipe(control[n]) == 0);
		children[n] = fork(); CHECK(children[n] >= 0);
		if (children[n] == 0) {
			close(ready[n][0]); close(control[n][1]);
			VmInfo inherited{}; inherited.version = 1;
			CHECK(Call(fd, kGetVmInfo, inherited) != 0 && errno == EPERM);
			int childFd = open(kDevice, O_RDWR); CHECK(childFd >= 0);
			for (unsigned i = 0; i < 3; i++) {
				uint32_t childVm = NewVm(childFd);
				for (unsigned j = 0; j < 4; j++) {
					uint32_t item = NewBuffer(childFd);
					CHECK(Bind(childFd, childVm, {{kVmMapOperation, kVmNoExecute,
						uint64_t(j) << 30, 65536, item, 0, 0}}) == 0);
					DropBuffer(childFd, item);
				}
			}
			CHECK(Info(childFd).clientVms == 3 && Buffers(childFd).bufferCount == 12);
			CHECK(write(ready[n][1], "r", 1) == 1);
			char value; CHECK(read(control[n][0], &value, 1) == 1);
			_exit(0);
		}
		close(ready[n][1]); close(control[n][0]);
	}
	for (unsigned n = 0; n < 4; n++) {
		char value; CHECK(read(ready[n][0], &value, 1) == 1 && value == 'r'); close(ready[n][0]);
	}
	CHECK(Info(fd).globalVms == baseline.globalVms + 12);
	CHECK(Info(fd).globalGenerations == baseline.globalGenerations + 12);
	CHECK(Info(fd).globalTablePages == baseline.globalTablePages + 120);
	CHECK(Buffers(fd).globalBuffers == baselineBuffers.globalBuffers + 48);
	CHECK(KernelAreas("Mali CSF VM page tables") == baselineAreas + 12);
	CHECK(KernelAreas("Mali CSF client buffer") == baselineBufferAreas + 48);
	for (unsigned n = 0; n < 4; n++) {
		if (n % 2) CHECK(kill(children[n], SIGKILL) == 0);
		else CHECK(write(control[n][1], "x", 1) == 1);
		close(control[n][1]); Wait(children[n], n % 2);
	}
	auto final = Info(fd); auto finalBuffers = Buffers(fd);
	CHECK(final.globalVms == baseline.globalVms && final.globalGenerations == baseline.globalGenerations);
	CHECK(final.globalTablePages == baseline.globalTablePages && final.clientVms == 0);
	CHECK(finalBuffers.globalBuffers == baselineBuffers.globalBuffers);
	CHECK(finalBuffers.globalBufferBytes == baselineBuffers.globalBufferBytes);
	CHECK(finalBuffers.globalClients == baselineBuffers.globalClients);
	CHECK(KernelAreas("Mali CSF VM page tables") == baselineAreas);
	CHECK(KernelAreas("Mali CSF client buffer") == baselineBufferAreas);
	CHECK(close(fd) == 0);
	puts("ROCK5_MALI_VM_PASS child_clients=4 child_vms=12 child_buffers=48"
		" normal_exits=2 killed_exits=2 counters_restored=1 gpu_activated=0");
}
