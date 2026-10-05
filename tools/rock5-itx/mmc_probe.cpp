/* Geometry and explicit MMC cache flush for disposable qualification cards. */
#include <Drivers.h>
#include <Errors.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int
main(int argc, char** argv)
{
	if (argc != 3 || (strcmp(argv[1], "geometry") != 0
			&& strcmp(argv[1], "flush") != 0 && strcmp(argv[1], "sd-flush") != 0)
		|| strncmp(argv[2], "/dev/disk/mmc/", 14) != 0) {
		fprintf(stderr, "Usage: %s geometry|flush|sd-flush /dev/disk/mmc/.../raw\n", argv[0]);
		return 2;
	}
	int fd = open(argv[2], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	device_geometry geometry;
	size_t bytes;
	if (ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) != 0
		|| ioctl(fd, B_GET_DEVICE_SIZE, &bytes, sizeof(bytes)) != 0) {
		perror("geometry");
		close(fd);
		return 1;
	}
	// devfs translates B_GET_GEOMETRY for partitions but passes the legacy
	// B_GET_DEVICE_SIZE through to the raw driver. Get the exact partition
	// extent explicitly instead of treating that raw capacity as its size.
	partition_info partition;
	if (ioctl(fd, B_GET_PARTITION_INFO, &partition, sizeof(partition)) == 0) {
		if (partition.offset < 0 || partition.size <= 0
			|| uint64_t(partition.size) > bytes
			|| uint64_t(partition.offset) > bytes - uint64_t(partition.size)
			|| partition.logical_block_size != int32(geometry.bytes_per_sector)
			|| memchr(partition.device, 0, sizeof(partition.device)) == NULL) {
			fprintf(stderr, "Invalid partition extent: offset=%" PRId64
				" size=%" PRId64 " raw-bytes=%zu logical=%" PRId32
				" sector=%" PRIu32 "\n", int64_t(partition.offset),
				int64_t(partition.size), bytes, partition.logical_block_size,
				geometry.bytes_per_sector);
			close(fd);
			return 1;
		}
		bytes = size_t(partition.size);
		printf("MMC_PARTITION offset=%" PRId64 " bytes=%zu block=%" PRId32 " raw=%s\n",
			int64_t(partition.offset), bytes, partition.logical_block_size, partition.device);
	} else if (errno != B_BAD_VALUE) {
		perror("partition info");
		close(fd);
		return 1;
	}
	printf("MMC_GEOMETRY path=%s sector=%" PRIu32 " bytes=%zu readonly=%u\n",
		argv[2], geometry.bytes_per_sector, bytes, unsigned(geometry.read_only));
	bool pass = true;
	if (strcmp(argv[1], "geometry") != 0) {
		// devfs fsync does not dispatch B_FLUSH_DRIVE_CACHE. SD currently
		// reports unsupported; that must not be accepted as an eMMC flush.
		int status = ioctl(fd, B_FLUSH_DRIVE_CACHE, NULL, 0);
		bool unsupported = status != 0 && errno == B_NOT_SUPPORTED;
		pass = strcmp(argv[1], "sd-flush") == 0 ? unsupported : status == 0;
		printf("MMC_FLUSH path=%s method=B_FLUSH_DRIVE_CACHE result=%s\n",
			argv[2], status == 0 ? "pass" : unsupported ? "unsupported" : "error");
		if (!pass)
			perror("B_FLUSH_DRIVE_CACHE expectation");
	}
	close(fd);
	return pass ? 0 : 1;
}
