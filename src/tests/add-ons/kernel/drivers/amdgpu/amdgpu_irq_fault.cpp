/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Root-only destructive-to-this-driver-session diagnostic. Use a fresh boot
// and restore/reboot after either outcome. Never accept polling as an IRQ.
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "UvdFixture.h"

static int sVideoFD = -1;
static amdgpu_video_decode sDecode;
static uint8* sVideoOutput;
static int sDecodeResult, sDecodeError;
static int32 sDecodeDone;

static int32 Decode(void*)
{
	sDecodeResult = ioctl(sVideoFD, AMDGPU_VIDEO_DECODE, &sDecode, sizeof(sDecode));
	sDecodeError = errno;
	atomic_set(&sDecodeDone, 1);
	return B_OK;
}

static void Require(bool okay, const char* message)
{
	if (okay) return;
	fprintf(stderr, "FAIL: %s (%d: %s)\n", message, errno, strerror(errno));
	exit(1);
}
template<typename T> static T Request()
{
	T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r;
}
static amdgpu_buffer Buffer(int fd, uint64 address, uint32 permissions)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = 4096;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &b, sizeof(b)) == 0, "allocate wired RAM");
	Require(ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "map CPU RAM");
	auto m = Request<amdgpu_vm_mapping>();
	m.handle = b.handle; m.address = address; m.bytes = b.bytes; m.permissions = permissions;
	Require(ioctl(fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "map GPU RAM");
	return b;
}
static amdgpu_irq_info IRQ(int fd)
{
	auto r = Request<amdgpu_irq_info>();
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == 0, "read IRQ state"); return r;
}
static amdgpu_memory_info Memory(int fd)
{
	auto r = Request<amdgpu_memory_info>();
	Require(ioctl(fd, AMDGPU_MEMORY_INFO, &r, sizeof(r)) == 0, "read VRAM accounting"); return r;
}
static amdgpu_gart_info Gart(int fd)
{
	auto r = Request<amdgpu_gart_info>();
	Require(ioctl(fd, AMDGPU_GART_INFO, &r, sizeof(r)) == 0, "read RAM accounting"); return r;
}
static void Check(const amdgpu_buffer& b, bool work)
{
	volatile uint32* p = (volatile uint32*)(addr_t)b.address;
	for (uint32 i = 0; i < 1024; i++)
		Require(p[i] == (work ? 0x98765432 ^ (i * 0x10204081u) : 0),
			"victim and neighboring data unchanged by rejected GPU operation");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	bool absent = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	bool video = argc == 2 && (strcmp(argv[1], "--vm-write-video") == 0
		|| strcmp(argv[1], "--privileged-register-video") == 0);
	bool vm = argc == 2 && (strcmp(argv[1], "--vm-write") == 0
		|| strcmp(argv[1], "--vm-write-video") == 0);
	bool privileged = argc == 2 && (strcmp(argv[1], "--privileged-register") == 0
		|| strcmp(argv[1], "--privileged-register-video") == 0);
	Require(absent || ((vm || privileged) && geteuid() == 0),
		"usage: amdgpu_irq_fault --vm-write[-video] | --privileged-register[-video]"
		" (root, cold boot) | --expect-no-device");
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (absent) {
		Require(monitor < 0 && errno == ENOENT, "no AMD device in QEMU");
		puts("PASS: no-device interrupt fault handling"); return 0;
	}
	Require(monitor >= 0, "open monitor");
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open fault client");
	auto info = Request<amdgpu_vm_info>();
	Require(ioctl(fd, AMDGPU_VM_INFO, &info, sizeof(info)) == 0, "create client VM");
	const uint64 commandVA = 0x200000000ULL, victimVA = 0x300000000ULL, workVA = 0x400000000ULL;
	auto command = Buffer(fd, commandVA, AMDGPU_VM_READ | AMDGPU_VM_EXECUTE);
	auto victim = Buffer(fd, victimVA, AMDGPU_VM_READ);
	auto work = Buffer(fd, workVA, AMDGPU_VM_READ | AMDGPU_VM_WRITE);
	auto warm = Request<amdgpu_vm_test>(); warm.address = workVA; warm.value = 0x98765432;
	Require(ioctl(fd, AMDGPU_VM_TEST, &warm, sizeof(warm)) == 0 && warm.status == B_OK,
		"known shader completes before fault");
	Check(work, true); Check(victim, false);
	auto before = IRQ(monitor);
	Require(before.enabled == 1 && before.msi == 1 && before.status == B_OK
		&& before.completed_fences == 1 && before.waits == 1 && before.vm_faults == 0
		&& before.privileged_faults == 0, "fresh boot has exactly one successful IRQ job");
	if (video) {
		sVideoFD = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(sVideoFD >= 0, "open concurrent video client");
		auto c = Request<amdgpu_video_create>(); c.config = {864, 480, 100, 30, 2, 0};
		Require(ioctl(sVideoFD, AMDGPU_VIDEO_CREATE, &c, sizeof(c)) == 0, "create concurrent session");
		sVideoOutput = (uint8*)malloc(c.output_bytes);
		Require(sVideoOutput != NULL, "allocate output sentinel");
		memset(sVideoOutput, 0x5a, c.output_bytes);
		sDecode = Request<amdgpu_video_decode>(); sDecode.handle = c.handle;
		sDecode.bitstream = (addr_t)uvd_bitstream; sDecode.bitstream_bytes = sizeof(uvd_bitstream);
		sDecode.output = (addr_t)sVideoOutput; sDecode.output_capacity = c.output_bytes;
		auto& p = sDecode.picture;
		p.flags = AMDGPU_H264_IDR; p.sps_flags = 5; p.pps_flags = 0x88;
		p.log2_frame_num_minus4 = 1; p.log2_poc_lsb_minus4 = 3; p.initial_qp_minus26 = 2;
		memset(p.scaling4x4, 16, sizeof(p.scaling4x4));
		memset(p.scaling8x8, 16, sizeof(p.scaling8x8));
	}
	uint64 vram = Memory(monitor).allocated_bytes, ram = Gart(monitor).allocated_bytes;
	volatile uint32* words = (volatile uint32*)(addr_t)command.address;
	// WRITE_DATA to a read-only owned page, or to CP_INT_CNTL_RING0 using
	// its already-installed value. If privilege enforcement fails, the latter
	// leaves interrupt configuration unchanged and this test fails explicitly.
	words[0] = 0xc0033700;
	words[1] = (vm ? 5 << 8 : 0) | 1 << 20;
	words[2] = vm ? (uint32)victimVA : 0x306a;
	words[3] = vm ? victimVA >> 32 : 0;
	words[4] = vm ? 0x12345678 : (1 << 26) | (1 << 23) | (1 << 22);
	words[5] = 0xc0f91000; // NOP: 251 DWORDs including its header
	for (uint32 i = 6; i < 1024; i++) words[i] = 0;
	__sync_synchronize();
	thread_id decoder = -1;
	if (video) {
		decoder = spawn_thread(Decode, "fault concurrent decode", B_NORMAL_PRIORITY, NULL);
		Require(decoder >= 0 && resume_thread(decoder) == B_OK, "start concurrent decode");
		bigtime_t deadline = system_time() + 5000000;
		for (;;) {
			auto invalid = Request<amdgpu_video_destroy>();
			Require(ioctl(sVideoFD, AMDGPU_VIDEO_DESTROY, &invalid, sizeof(invalid)) == -1,
				"invalid destroy cannot change session");
			if (errno == B_BUSY) break;
			Require(errno == B_BAD_VALUE && atomic_get(&sDecodeDone) == 0 && system_time() < deadline,
				"observe active video ioctl before injecting fault");
			snooze(50);
		}
	}
	auto r = Request<amdgpu_gfx_submit>(); r.address = commandVA; r.dwords = 256;
	Require(ioctl(fd, AMDGPU_GFX_SUBMIT, &r, sizeof(r)) == 0, "submit deliberate fault");
	auto after = IRQ(monitor);
	printf("fault %s job %#x elapsed %llu us completion %llu VM %#x/%#x ring %u/%u;"
		" IRQ status %#x EOP %llu waits %llu VM %llu privileged %llu unknown %llu"
		" overflow %llu last %#x/%#x/%#x/%#x\n", vm ? "VM write" : "privileged register",
		(unsigned)r.status, (unsigned long long)r.elapsed_us, (unsigned long long)r.completion,
		(unsigned)r.vm_fault_status[0], (unsigned)r.vm_fault_status[1], (unsigned)r.rptr,
		(unsigned)r.wptr, (unsigned)after.status, (unsigned long long)after.eop_events,
		(unsigned long long)after.waits, (unsigned long long)after.vm_faults,
		(unsigned long long)after.privileged_faults, (unsigned long long)after.unknown,
		(unsigned long long)after.overflows, (unsigned)after.last[0], (unsigned)after.last[1],
		(unsigned)after.last[2], (unsigned)after.last[3]);
	Require(r.status == (vm ? B_BAD_DATA : B_NOT_ALLOWED) && after.status == r.status,
		"fault IRQ supplies job failure, not a polling timeout");
	Require(after.waits == 2 && (vm ? after.vm_faults > 0 : after.privileged_faults > 0)
		&& after.overflows == 0 && after.unknown == 0, "expected hardware fault notification");
	if (video) {
		status_t result;
		Require(wait_for_thread(decoder, &result) == B_OK && result == B_OK, "concurrent decode retires");
		printf("concurrent decode result %d error %#x\n", sDecodeResult, (unsigned)sDecodeError);
		Require(sDecodeResult == -1 && (sDecodeError == B_BAD_DATA || sDecodeError == B_DEV_NOT_READY),
			"decode sees concurrent fault (a completed decode means overlap was not observed)");
		for (uint32 i = 0; i < sDecode.output_capacity; i++)
			Require(sVideoOutput[i] == 0x5a, "faulted decode never publishes output");
		Require(close(sVideoFD) == 0, "close faulted video session");
		free(sVideoOutput);
	}
	Check(work, true); Check(victim, false);
	Require(ioctl(fd, AMDGPU_VM_TEST, &warm, sizeof(warm)) == -1 && errno == B_DEV_NOT_READY,
		"subsequent shader rejected by quarantined device");
	auto allocation = Request<amdgpu_buffer>(); allocation.bytes = 4096;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &allocation, sizeof(allocation)) == -1
		&& errno == B_DEV_NOT_READY, "new allocation rejected after fault");
	close(fd);
	Require(Memory(monitor).faulted == 1 && Memory(monitor).allocated_bytes == vram
		&& Gart(monitor).allocated_bytes == ram, "close preserves quarantined VRAM and wired RAM");
	amdgpu_buffer* buffers[] = {&command, &victim, &work};
	for (auto* b : buffers)
		Require(delete_area(b->area) == B_OK, "release revoked CPU clone");
	close(monitor);
	puts("PASS: hardware fault IRQ, unchanged victim/neighbor, failed fence and post-close quarantine");
	if (video) puts("PASS: concurrent video failure, untouched output and quarantined session storage");
	return 0;
}
