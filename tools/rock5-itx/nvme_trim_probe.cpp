/*
 * DSM ioctl checks for an explicitly marked disposable QEMU namespace.
 * Distributed under the terms of the MIT License.
 */
#include <Drivers.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>


static const uint64 kCapacity = UINT64_C(8) * 1024 * 1024 * 1024;
static const uint64 kMarkerOffset = UINT64_C(7) * 1024 * 1024 * 1024;
static const uint64 kGuardOffset = kMarkerOffset + 1024 * 1024;
static const char kMarker[] = "ROCK5_QEMU_TRIM_SCRATCH_V1";


static bool
Trim(int fd, const char* name, fs_trim_data* data, uint64 expectedBytes,
	int expectedError = 0)
{
	size_t bytes = sizeof(fs_trim_data)
		+ (data->range_count - 1) * sizeof(data->ranges[0]);
	data->trimmed_size = 0;
	errno = 0;
	int status = ioctl(fd, B_TRIM_DEVICE, data, bytes);
	if (expectedError != 0) {
		if (status == 0 || errno != expectedError) {
			fprintf(stderr, "%s: expected error %d, got status=%d errno=%d\n",
				name, expectedError, status, errno);
			return false;
		}
	} else if (status != 0 || data->trimmed_size != expectedBytes) {
		fprintf(stderr, "%s: status=%d errno=%d trimmed=%" B_PRIu64
			" expected=%" B_PRIu64 "\n", name, status, errno,
			data->trimmed_size, expectedBytes);
		return false;
	}
	printf("ROCK5_QEMU_TRIM_CASE_PASS %s trimmed=%" B_PRIu64 "\n",
		name, data->trimmed_size);
	fflush(stdout);
	return true;
}


int
main(int argc, char** argv)
{
	if (argc != 2 || strcmp(argv[1], "--qemu-scratch") != 0)
		return 2;
	int fd = open("/dev/disk/nvme/0/raw", O_RDWR);
	if (fd < 0)
		return 1;
	device_geometry geometry = {};
	size_t capacity = 0;
	char marker[sizeof(kMarker)];
	if (ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) != 0
		|| geometry.bytes_per_sector != 512 || geometry.read_only
		|| ioctl(fd, B_GET_DEVICE_SIZE, &capacity, sizeof(capacity)) != 0
		|| capacity != kCapacity
		|| pread(fd, marker, sizeof(marker), kMarkerOffset) != sizeof(marker)
		|| memcmp(marker, kMarker, sizeof(marker)) != 0) {
		fprintf(stderr, "Missing marked 8 GiB QEMU scratch namespace.\n");
		close(fd);
		return 2;
	}
	alarm(60);
	printf("ROCK5_QEMU_TRIM_BEGIN\n");

	const uint32 kMaxTestRanges = 65537;
	fs_trim_data* data = (fs_trim_data*)calloc(1, sizeof(fs_trim_data)
		+ (kMaxTestRanges - 1) * sizeof(fs_trim_data::range));
	if (data == NULL)
		return 1;
	data->range_count = 1;
	data->ranges[0] = {kGuardOffset + 17, 1};
	bool passed = Trim(fd, "partial-sector-noop", data, 0);
	data->ranges[0] = {kGuardOffset, 0};
	passed = passed && Trim(fd, "zero-length-noop", data, 0);
	data->ranges[0] = {kCapacity - 17, UINT64_MAX};
	passed = passed && Trim(fd, "clipped-partial-sector-noop", data, 0);
	data->ranges[0] = {kCapacity, 1};
	passed = passed && Trim(fd, "outside-namespace", data, 0, B_BAD_VALUE);

	data->range_count = 257;
	passed = passed && Trim(fd, "too-many-ranges", data, 0, B_BAD_VALUE);
	data->range_count = kMaxTestRanges;
	passed = passed && Trim(fd, "range-count-narrowing", data, 0, B_BAD_VALUE);

	data->range_count = 2;
	data->ranges[0] = {kGuardOffset + 16384, 512};
	data->ranges[1] = {kCapacity, 512};
	passed = passed && Trim(fd, "invalid-later-range", data, 0, B_BAD_VALUE);

	data->range_count = 1;
	data->ranges[0] = {kGuardOffset + 17, 1007};
	passed = passed && Trim(fd, "complete-inner-sector", data, 512);
	data->range_count = 3;
	data->ranges[0] = {kGuardOffset + 17, 1};
	data->ranges[1] = {kGuardOffset + 4096, 1024};
	data->ranges[2] = {kGuardOffset + 8192, 0};
	passed = passed && Trim(fd, "compact-empty-ranges", data, 1024);
	data->range_count = 1;
	data->ranges[0] = {kCapacity - 512, UINT64_MAX};
	passed = passed && Trim(fd, "clip-at-namespace-end", data, 512);
	passed = passed && ioctl(fd, B_FLUSH_DRIVE_CACHE, NULL, 0) == 0;
	free(data);
	close(fd);
	puts(passed ? "ROCK5_QEMU_TRIM_PASS" : "ROCK5_QEMU_TRIM_FAIL");
	return passed ? 0 : 1;
}
