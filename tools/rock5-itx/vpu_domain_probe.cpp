/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Observe RK3588 video power domains through the display driver's existing
 * read-only PMU snapshot. No decoder or power-controller register is written.
 */

#include <OS.h>

#include "DisplayObservation.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>


int
main()
{
	using namespace RK3588Display;
	int fd = open("/dev/graphics/rk3588_display/0", O_RDONLY);
	if (fd < 0) {
		perror("open display");
		return 1;
	}
	DisplaySnapshot snapshot = {};
	if (ioctl(fd, kGetSnapshot, &snapshot, sizeof(snapshot)) != 0) {
		perror("snapshot");
		close(fd);
		return 1;
	}
	printf("snapshot version=%u flags=%#x gate1=%08" PRIx32
		" gate2=%08" PRIx32 " status=%08" PRIx32 " repair=%08" PRIx32 "\n",
		snapshot.version, snapshot.flags, snapshot.pmu[6], snapshot.pmu[7],
		snapshot.pmu[8], snapshot.pmu[10]);
	// Linux 6.18.52 drivers/pmdomain/rockchip/pm-domains.c RK3588 masks.
	const unsigned bits[] = {7, 8, 9, 11};
	const char* names[] = {"rkvdec0", "rkvdec1", "vdpu", "av1"};
	for (unsigned i = 0; i < 4; i++) {
		printf("%s repair_bit=%u on=%u\n", names[i], bits[i],
			(snapshot.pmu[10] >> bits[i]) & 1);
	}
	close(fd);
	return 0;
}
