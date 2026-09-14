/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "CsfPlatform.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>


int
main(int argc, char** argv)
{
	bool platform = argc > 1 && strcmp(argv[1], "--platform") == 0;
	int argument = platform ? 2 : 1;
	if (argc > argument + 1) {
		fprintf(stderr, "usage: %s [--platform] [device]\n", argv[0]);
		return 2;
	}
	const char* path = argc > argument ? argv[argument] : "/dev/graphics/mali_csf/0";
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
	if (platform) {
		MaliCSF::PlatformSnapshot snapshot = {};
		if (ioctl(fd, MaliCSF::kGetPlatformSnapshot, &snapshot, sizeof(snapshot) - 1) == 0
			|| errno != EINVAL) {
			fprintf(stderr, "Malformed platform request was not rejected\n");
			close(fd);
			return 1;
		}
		for (int sample = 0; sample < 3; sample++) {
			if (ioctl(fd, MaliCSF::kGetPlatformSnapshot, &snapshot, sizeof(snapshot)) != 0) {
				perror("Mali platform observation");
				close(fd);
				return 1;
			}
			if (snapshot.version != MaliCSF::kPlatformVersion
				|| snapshot.flags != MaliCSF::kPlatformReadOnly
				|| snapshot.startedMicros < 0
				|| snapshot.finishedMicros < snapshot.startedMicros) {
				fprintf(stderr, "Invalid platform observation\n");
				close(fd);
				return 1;
			}
			printf("ROCK5_MALI_PLATFORM sample=%d start_us=%" PRId64 " end_us=%" PRId64
				" select=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
				" gate=%08" PRIx32 ",%08" PRIx32
				" idle_req=%08" PRIx32 " idle_ack=%08" PRIx32 " idle=%08" PRIx32
				" power_req=%08" PRIx32 " repair=%08" PRIx32 "\n",
				sample, snapshot.startedMicros, snapshot.finishedMicros,
				snapshot.clockSelect[0], snapshot.clockSelect[1], snapshot.clockSelect[2],
				snapshot.clockGate[0], snapshot.clockGate[1], snapshot.idleRequest,
				snapshot.idleAck, snapshot.idleStatus, snapshot.powerRequest, snapshot.powerRepair);
		}
		puts("ROCK5_MALI_PLATFORM_OBSERVATION_PASS register_writes=0 gpu_accessed=0");
	}
	close(fd);
	return 0;
}
