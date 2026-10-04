/* Geometry and explicit cache flush for disposable AHCI qualification disks. */
#include <Drivers.h>
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
			&& strcmp(argv[1], "flush") != 0)
		|| strncmp(argv[2], "/dev/disk/scsi/", 15) != 0) {
		fprintf(stderr, "Usage: %s geometry|flush /dev/disk/scsi/.../raw\n", argv[0]);
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
	printf("AHCI_GEOMETRY path=%s sector=%" PRIu32 " bytes=%zu readonly=%u\n",
		argv[2], geometry.bytes_per_sector, bytes, unsigned(geometry.read_only));
	int status = 0;
	if (strcmp(argv[1], "flush") == 0) {
		// devfs fsync does not dispatch the drive-cache command.
		status = ioctl(fd, B_FLUSH_DRIVE_CACHE, NULL, 0);
		if (status != 0)
			perror("B_FLUSH_DRIVE_CACHE");
		printf("AHCI_FLUSH path=%s method=B_FLUSH_DRIVE_CACHE status=%s\n",
			argv[2], status == 0 ? "pass" : "fail");
	}
	close(fd);
	return status == 0 ? 0 : 1;
}
