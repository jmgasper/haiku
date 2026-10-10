/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void Require(bool okay, const char* message)
{
	if (!okay) {
		fprintf(stderr, "FAIL: %s (%s)\n", message, strerror(errno));
		exit(1);
	}
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 5) {
		fprintf(stderr, "usage: amdgpu_gfx CE.bin PFP.bin ME.bin RLC.bin\n");
		return 2;
	}
	amdgpu_gfx_test request = {};
	request.version = AMDGPU_HAIKU_ABI_VERSION;
	request.size = sizeof(request);
	uint8* images[4] = {};
	for (unsigned i = 0; i < 4; i++) {
		FILE* file = fopen(argv[i + 1], "rb");
		Require(file != NULL, "open firmware");
		Require(fseek(file, 0, SEEK_END) == 0, "firmware seek");
		long size = ftell(file);
		Require(size >= 44 && size <= 65536, "firmware size");
		rewind(file);
		images[i] = (uint8*)malloc(size);
		Require(images[i] != NULL, "allocate firmware input");
		Require(fread(images[i], 1, size, file) == (size_t)size, "read firmware");
		fclose(file);
		request.firmware[i] = (addr_t)images[i];
		request.firmware_size[i] = size;
	}
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open amdgpu");
	Require(ioctl(fd, AMDGPU_GFX_TEST, NULL, sizeof(request)) == -1
		&& errno == B_BAD_ADDRESS, "null request rejected");
	for (unsigned i = 0; i < 4; i++) {
		auto bad = request;
		bad.firmware[i] = 0;
		Require(ioctl(fd, AMDGPU_GFX_TEST, &bad, sizeof(bad)) == 0
			&& bad.stage == 0 && bad.status == B_BAD_ADDRESS, "invalid firmware pointer rejected");
		uint8 saved = images[i][12];
		images[i][12] = 0xff;
		bad = request;
		Require(ioctl(fd, AMDGPU_GFX_TEST, &bad, sizeof(bad)) == 0
			&& bad.stage == 0 && bad.status == B_BAD_DATA, "wrong firmware IP rejected");
		images[i][12] = saved;
	}
	puts("PASS: invalid GFX firmware requests rejected before hardware writes");
	for (unsigned round = 0; round < 48; round++) {
		auto result = request;
		Require(ioctl(fd, AMDGPU_GFX_TEST, &result, sizeof(result)) == 0, "GFX ioctl");
		printf("GFX stage %u status %#x (%s) seq %u, checked %u, mismatches %u\n",
			(unsigned)result.stage, (unsigned)result.status, strerror(result.status),
			(unsigned)result.sequence, (unsigned)result.checked_bytes, (unsigned)result.mismatches);
		printf("CP %#x ring %#x pointers %u/%u GRBM %#x RLC %#x\n",
			(unsigned)result.cp_control, (unsigned)result.ring_control,
			(unsigned)result.rptr, (unsigned)result.wptr,
			(unsigned)result.grbm_status, (unsigned)result.rlc_status);
		amdgpu_gart_info gart = {};
		gart.version = AMDGPU_HAIKU_ABI_VERSION;
		gart.size = sizeof(gart);
		Require(ioctl(fd, AMDGPU_GART_INFO, &gart, sizeof(gart)) == 0, "VM fault query");
		printf("VM fault status %#x, VMID %u, GPU page %#x, client %#x\n",
			(unsigned)result.vm_fault_status,
			(unsigned)((result.vm_fault_status >> 25) & 15),
			(unsigned)result.vm_fault_address,
			(unsigned)result.vm_fault_client);
		if (result.status != B_OK || result.stage != 5 || result.checked_bytes != 12288
			|| result.mismatches != 0) {
			errno = result.status != B_OK ? result.status : B_BAD_DATA;
			Require(false, "GFX execution, data, guards and VM faults");
		}
	}
	close(fd);
	for (uint8* image : images)
		free(image);
	puts("PASS: 48 GFX8 submissions including direct writes, compute shaders and indirect buffers, complete data/guard checks");
	return 0;
}
