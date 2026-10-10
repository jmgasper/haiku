/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: amdgpu_sdma polaris10_sdma.bin\n");
		return 2;
	}
	FILE* file = fopen(argv[1], "rb");
	if (file == NULL) {
		perror(argv[1]);
		return 1;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	if (size < 52 || size > 65536) {
		fclose(file);
		return 1;
	}
	rewind(file);
	void* data = malloc(size);
	if (data == NULL) {
		fclose(file);
		return 1;
	}
	bool read = fread(data, 1, size, file) == (size_t)size;
	fclose(file);
	if (!read) {
		free(data);
		return 1;
	}
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (fd < 0) {
		perror("open amdgpu");
		free(data);
		return 1;
	}
	amdgpu_sdma_test_result result = {};
	result.version = AMDGPU_HAIKU_ABI_VERSION;
	result.size = sizeof(result);
	result.firmware = (uint64)(addr_t)data;
	result.firmware_size = size;
	int status = ioctl(fd, AMDGPU_SDMA_TEST, &result, sizeof(result));
	free(data);
	close(fd);
	if (status != 0) {
		perror("AMDGPU_SDMA_TEST");
		return 1;
	}
	printf("SDMA stage %" B_PRIu32 ", status %" B_PRId32 " (%s)\n",
		result.stage, result.status, strerror(result.status));
	printf("firmware %" B_PRIu32 ", scratch GPU %#" B_PRIx64
		", fence %#" B_PRIx32 ", ring %" B_PRIu32 "/%" B_PRIu32
		", engine %#" B_PRIx32 ", halted %" B_PRIu32 "\n",
		result.firmware_version, result.scratch_gpu, result.fence,
		result.rptr, result.wptr, result.engine_status, result.halted);
	printf("%" B_PRIu32 " bytes checked, %" B_PRIu32
		" mismatches, %" B_PRIu64 " us\n", result.checked_bytes,
		result.mismatches, result.elapsed_us);
	bool pass = result.status == B_OK && result.stage == 5
		&& result.fence == 0xdeadbeef && result.halted == 1
		&& result.checked_bytes == 1024 * 1024 - 4096 && result.mismatches == 0;
	puts(pass ? "PASS: GPU copy, fill, fence and scratch guards" : "FAIL: SDMA test");
	return pass ? 0 : 1;
}
