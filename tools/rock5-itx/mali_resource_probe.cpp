/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "CsfResources.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>


int
main(int argc, char** argv)
{
	if (argc > 2) {
		fprintf(stderr, "usage: %s [device]\n", argv[0]);
		return 2;
	}
	const char* path = argc == 2 ? argv[1] : "/dev/graphics/mali_csf/0";
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open Mali resource interface");
		return 1;
	}
	MaliCSF::ResourceInfo info = {};
	if (ioctl(fd, MaliCSF::kGetResources, &info, sizeof(info)) != 0) {
		perror("Mali resources");
		close(fd);
		return 1;
	}
	if (!MaliCSF::ResourcesMatch(info)) {
		fprintf(stderr, "Unexpected Mali firmware resource description\n");
		close(fd);
		return 1;
	}
	if (ioctl(fd, MaliCSF::kGetResources, &info, sizeof(info) - 1) == 0
		|| errno != EINVAL) {
		fprintf(stderr, "Malformed resource request was not rejected\n");
		close(fd);
		return 1;
	}
	close(fd);
	printf("ROCK5_MALI_RESOURCES version=%" PRIu32 " gpu=%08" PRIx64
		" size=%" PRIu64 " cru=%08" PRIx64 " pmu=%08" PRIx64
		" gic=%08" PRIx64 "\n", info.version, info.gpuBase, info.gpuSize,
		info.clockBase, info.powerBase, info.interruptBase);
	printf("ROCK5_MALI_RESOURCES irq=%" PRIu32 ",%" PRIu32 ",%" PRIu32
		" clocks=%" PRIu32 ",%" PRIu32 ",%" PRIu32 " domain=%" PRIu32
		" supply=%s min_uv=%" PRIu32 " max_uv=%" PRIu32 "\n",
		info.interrupts[0], info.interrupts[1], info.interrupts[2], info.clockIds[0],
		info.clockIds[1], info.clockIds[2], info.powerDomain, info.supplyName,
		info.supplyMinMicrovolt, info.supplyMaxMicrovolt);
	puts("ROCK5_MALI_RESOURCE_DESCRIPTION_PASS gpu_accessed=0");
	return 0;
}
