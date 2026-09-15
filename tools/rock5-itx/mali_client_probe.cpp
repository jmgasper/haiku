/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfQueue.h"
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
		fprintf(stderr, "ROCK5_MALI_CLIENT_FAIL line=%d errno=%d\n", line, errno);
		exit(1);
	}
}
#define CHECK(value) Check((value), __LINE__)
template<typename T> static int Call(int fd, uint32_t op, T& value)
{
	return ioctl(fd, op, &value, sizeof(value));
}
static ClientInfo Info(int fd)
{
	ClientInfo value{}; value.version = 1;
	CHECK(Call(fd, kGetClientInfo, value) == 0);
	CHECK(value.version == 1 && value.reserved[0] == 0 && value.reserved[1] == 0);
	return value;
}
static BufferCreate Create(int fd, uint64_t bytes)
{
	BufferCreate value{}; value.version = 1; value.bytes = bytes;
	CHECK(Call(fd, kCreateBuffer, value) == 0);
	CHECK(value.handle != 0 && value.bytes == ((bytes + 4095) & ~UINT64_C(4095)));
	return value;
}
static BufferMap Map(int fd, uint32_t handle)
{
	BufferMap value{}; value.version = 1; value.handle = handle;
	CHECK(Call(fd, kMapBuffer, value) == 0);
	CHECK(value.address != 0 && value.area >= 0 && value.flags == kBufferNormalNoncacheable);
	area_info area;
	CHECK(get_area_info(value.area, &area) == B_OK);
	CHECK((uintptr_t)area.address == value.address && area.size == value.bytes);
	CHECK(area.team == getpid() && (area.protection & (B_READ_AREA | B_WRITE_AREA)) == 3);
	CHECK((area.protection & B_EXECUTE_AREA) == 0);
	return value;
}
static void Destroy(int fd, uint32_t handle)
{
	BufferHandle value{1, handle, 0};
	CHECK(Call(fd, kDestroyBuffer, value) == 0);
	CHECK(Call(fd, kDestroyBuffer, value) != 0 && errno == ENOENT);
}
static uint8_t Pattern(size_t offset, unsigned round)
{
	return uint8_t((offset * 17) ^ (offset >> 8) ^ (offset >> 16) ^ (round * 93 + 11));
}
static void CheckBytes(volatile uint8_t* data, size_t bytes, unsigned round)
{
	for (size_t i = 0; i < bytes; i++) CHECK(data[i] == Pattern(i, round));
}
static void Wait(pid_t child, bool killed)
{
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
		: WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static uint32_t KernelBuffers()
{
	ssize_t cookie = 0; area_info area; uint32_t count = 0;
	while (get_next_area_info(B_SYSTEM_TEAM, &cookie, &area) == B_OK)
		if (strcmp(area.name, "Mali CSF client buffer") == 0) count++;
	return count;
}
static void PlainAreas()
{
	void* first = NULL;
	area_id source = create_area("Mali probe ordinary RAM", &first, B_ANY_ADDRESS,
		8192, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	CHECK(source >= 0);
	void* second = NULL;
	area_id alias = clone_area("Mali probe RAM clone", &second, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, source);
	CHECK(alias >= 0 && first != second);
	memset(first, 0x5b, 8192);
	CHECK(memcmp(first, second, 8192) == 0);
	pid_t child = fork(); CHECK(child >= 0);
	if (child == 0) { memset(second, 0xa7, 8192); _exit(0); }
	Wait(child, false);
	CHECK(delete_area(source) == B_OK);
	for (unsigned i = 0; i < 8192; i++) CHECK(((uint8_t*)second)[i] == 0xa7);
	CHECK(delete_area(alias) == B_OK);
	puts("ROCK5_MALI_CLIENT_ORDINARY_AREAS_PASS bytes=8192 fork=1");
}

int main(int argc, char** argv)
{
	setbuf(stdout, NULL);
	PlainAreas();
	if (argc == 2 && strcmp(argv[1], "--areas") == 0) return 0;
	if (argc != 1) return 2;
	int fd = open(kDevice, O_RDWR);
	if (fd < 0 && errno == ENOENT) {
		puts("ROCK5_MALI_CLIENT_NO_DEVICE"); return 77;
	}
	CHECK(fd >= 0);
	ClientInfo baseline = Info(fd);
	uint32_t baselineAreas = KernelBuffers();
	CHECK(baselineAreas == baseline.globalBuffers);
	CHECK(baseline.capabilities == (kClientCpuBuffers | kClientVmMappings | kClientQueues) && baseline.bufferCount == 0);
	CHECK(baseline.maxBuffers == kMaxClientBuffers && baseline.maxBufferBytes == kMaxBufferBytes);
	int other = open(kDevice, O_RDWR); CHECK(other >= 0);
	int duplicate = dup(fd); CHECK(duplicate >= 0);
	CHECK(Info(fd).globalClients == baseline.globalClients + 1);
	int readonly = open(kDevice, O_RDONLY); CHECK(readonly >= 0);
	CHECK(Info(readonly).capabilities == 0);
	BufferCreate invalid{}; invalid.version = 1; invalid.bytes = 4096;
	CHECK(Call(readonly, kCreateBuffer, invalid) != 0 && errno == EPERM);
	CHECK(close(readonly) == 0);
	CHECK(ioctl(fd, kCreateBuffer, &invalid, sizeof(invalid) - 1) != 0 && errno == EINVAL);
	CHECK(ioctl(fd, kCreateBuffer, NULL, sizeof(invalid)) != 0 && errno == EFAULT);
	for (uint64_t size : {UINT64_C(0), UINT64_MAX, kMaxBufferBytes + 1}) {
		invalid.bytes = size;
		CHECK(Call(fd, kCreateBuffer, invalid) != 0 && errno == EINVAL);
	}
	unsigned index = 0;
	for (uint64_t bytes : {UINT64_C(1), UINT64_C(4096), UINT64_C(4097),
		UINT64_C(65536), UINT64_C(1048593), UINT64_C(4194304)}) {
		auto buffer = Create(fd, bytes);
		BufferInfo details{1, buffer.handle, 0, 0, 0};
		CHECK(Call(duplicate, kGetBufferInfo, details) == 0);
		CHECK(details.bytes == buffer.bytes && details.flags == kBufferNormalNoncacheable);
		BufferHandle foreign{1, buffer.handle, 0};
		CHECK(Call(other, kDestroyBuffer, foreign) != 0 && errno == ENOENT);
		auto x = Map(fd, buffer.handle), y = Map(duplicate, buffer.handle);
		CHECK(x.address != y.address && x.bytes == buffer.bytes && y.bytes == buffer.bytes);
		auto first = (volatile uint8_t*)(uintptr_t)x.address;
		auto second = (volatile uint8_t*)(uintptr_t)y.address;
		for (size_t i = 0; i < buffer.bytes; i++) CHECK(first[i] == 0 && second[i] == 0);
		for (unsigned round = 0; round < 3; round++) {
			for (size_t i = 0; i < buffer.bytes; i++) first[i] = Pattern(i, round);
			CheckBytes(second, buffer.bytes, round);
		}
		// A forked CPU mapping remains shared while descriptor ownership stays
		// with the original team. This exercises vm_copy_area as well as clone.
		pid_t child = fork(); CHECK(child >= 0);
		if (child == 0) {
			CHECK(Call(fd, kGetBufferInfo, details) != 0 && errno == EPERM);
			CheckBytes(second, buffer.bytes, 2);
			for (size_t i = 0; i < buffer.bytes; i++) second[i] = Pattern(i, 3);
			_exit(0);
		}
		Wait(child, false);
		CheckBytes(first, buffer.bytes, 3);
		Destroy(fd, buffer.handle);
		CheckBytes(second, buffer.bytes, 3);
		CHECK(delete_area(x.area) == B_OK && delete_area(y.area) == B_OK);
		printf("ROCK5_MALI_CLIENT_BUFFER index=%u requested=%" PRIu64 " bytes=%" PRIu64
			" zero=1 rounds=3 fork=1 mapping_survives_handle=1\n", index++, bytes, buffer.bytes);
	}
	CHECK(close(duplicate) == 0 && close(other) == 0);
	// Keep a CPU map across final descriptor close and check a fresh allocation
	// cannot reuse its backing RAM while that area still owns it.
	auto held = Create(fd, 8192);
	auto mapping = Map(fd, held.handle);
	memset((void*)(uintptr_t)mapping.address, 0xd3, mapping.bytes);
	CHECK(close(fd) == 0);
	fd = open(kDevice, O_RDWR); CHECK(fd >= 0);
	auto fresh = Create(fd, 8192); auto freshMap = Map(fd, fresh.handle);
	memset((void*)(uintptr_t)freshMap.address, 0x6c, freshMap.bytes);
	for (size_t i = 0; i < mapping.bytes; i++)
		CHECK(((volatile uint8_t*)(uintptr_t)mapping.address)[i] == 0xd3);
	Destroy(fd, fresh.handle);
	CHECK(delete_area(mapping.area) == B_OK && delete_area(freshMap.area) == B_OK);
	// Four simultaneous teams intentionally leave twelve handles and mappings
	// each. Two exit normally, two are killed after signalling readiness.
	pid_t children[4]; int control[4][2], ready[4][2];
	for (unsigned n = 0; n < 4; n++) {
		CHECK(pipe(control[n]) == 0 && pipe(ready[n]) == 0);
		children[n] = fork(); CHECK(children[n] >= 0);
		if (children[n] == 0) {
			close(control[n][1]); close(ready[n][0]);
			int childFd = open(kDevice, O_RDWR); CHECK(childFd >= 0);
			for (unsigned i = 0; i < 12; i++) {
				auto item = Create(childFd, 16385 + i * 4096);
				auto view = Map(childFd, item.handle);
				memset((void*)(uintptr_t)view.address, i + n, view.bytes);
			}
			CHECK(write(ready[n][1], "r", 1) == 1);
			char value; CHECK(read(control[n][0], &value, 1) == 1);
			_exit(0);
		}
		close(control[n][0]); close(ready[n][1]);
	}
	for (unsigned n = 0; n < 4; n++) {
		char value; CHECK(read(ready[n][0], &value, 1) == 1 && value == 'r');
		close(ready[n][0]);
	}
	CHECK(Info(fd).globalBuffers == baseline.globalBuffers + 48);
	CHECK(KernelBuffers() == baselineAreas + 48);
	for (unsigned n = 0; n < 4; n++) {
		if (n % 2) CHECK(kill(children[n], SIGKILL) == 0);
		else CHECK(write(control[n][1], "x", 1) == 1);
		close(control[n][1]); Wait(children[n], n % 2);
	}
	ClientInfo final = Info(fd);
	CHECK(final.bufferCount == 0 && final.bufferBytes == 0);
	CHECK(final.globalClients == baseline.globalClients && final.globalBuffers == baseline.globalBuffers);
	CHECK(final.globalBufferBytes == baseline.globalBufferBytes);
	CHECK(KernelBuffers() == baselineAreas);
	CHECK(close(fd) == 0);
	puts("ROCK5_MALI_CLIENT_PASS buffers=6 aliases=12 fork_mappings=6 close_mapping=1"
		" child_clients=4 child_buffers=48 normal_exits=2 killed_exits=2 counters_restored=1 gpu_submissions=0");
	return 0;
}
