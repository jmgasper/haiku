/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	pvrinfo: the powervr driver's bring-up state (its STAGE query).

		pvrinfo				the stage, the GPU, the firmware and its counters
		pvrinfo --health	send the firmware a HEALTH_CHECK first (root)
		pvrinfo --dump		have the driver log its registers, the firmware's
							state and trace to the syslog first (root)
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pvr_haiku.h>


static const char*
stage_name(uint32 stage)
{
	switch (stage) {
		case PVR_HAIKU_STAGE_OFF:
			return "off (driver settings)";
		case PVR_HAIKU_STAGE_POWERED:
			return "powered";
		case PVR_HAIKU_STAGE_IDENTIFIED:
			return "identified";
		case PVR_HAIKU_STAGE_FIRMWARE:
			return "firmware running";
		default:
			return "unknown";
	}
}


int
main(int argc, char** argv)
{
	uint32 command = PVR_HAIKU_STAGE_QUERY;
	if (argc > 1) {
		if (strcmp(argv[1], "--health") == 0)
			command = PVR_HAIKU_STAGE_HEALTH_CHECK;
		else if (strcmp(argv[1], "--dump") == 0)
			command = PVR_HAIKU_STAGE_DUMP;
		else {
			fprintf(stderr, "usage: %s [--health | --dump]\n", argv[0]);
			return 2;
		}
	}

	const char* path = getenv(PVR_HAIKU_DEVICE_ENV);
	if (path == NULL)
		path = PVR_HAIKU_DEVICE_PATH;
	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}

	pvr_haiku_stage stage = {};
	stage.version = PVR_HAIKU_ABI_VERSION;
	stage.command = command;
	if (ioctl(fd, PVR_HAIKU_OP(PVR_HAIKU_NR_STAGE), &stage, sizeof(stage))
			!= 0) {
		fprintf(stderr, "STAGE: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	printf("stage:              %s\n", stage_name(stage.stage));
	printf("BVNC:               %u.%u.%u.%u (%#llx)\n",
		(unsigned)(stage.bvnc >> 48), (unsigned)((stage.bvnc >> 32) & 0xffff),
		(unsigned)((stage.bvnc >> 16) & 0xffff), (unsigned)(stage.bvnc & 0xffff),
		(unsigned long long)stage.bvnc);
	printf("core ID:            %#x\n", (unsigned)stage.core_id);
	printf("core clock:         %u MHz\n",
		(unsigned)(stage.core_clock / 1000000));
	printf("firmware stage:     %s\n", strerror(stage.firmware_status));
	if (stage.fw_version_major == 0)
		return 0;
	printf("firmware:           %u.%u build %u, %s\n",
		(unsigned)stage.fw_version_major, (unsigned)stage.fw_version_minor,
		(unsigned)stage.fw_version_build,
		stage.firmware_running ? "running" : "not running");
	printf("boot:               %llu us\n",
		(unsigned long long)stage.fw_boot_time);
	printf("health checks:      %u executed, %u failed\n",
		(unsigned)stage.health_checks, (unsigned)stage.health_check_failures);
	printf("last KCCB return:   %#x\n", (unsigned)stage.last_kccb_return);
	printf("KCCB executed:      %u\n", (unsigned)stage.kccb_cmds_executed);
	printf("interrupts:         %u (%u spurious)\n",
		(unsigned)stage.irq_count, (unsigned)stage.irq_spurious);
	printf("MIPS exceptions:    %#x\n", (unsigned)stage.mips_exception_status);
	printf("firmware faults:    %u\n", (unsigned)stage.fw_faults);
	return 0;
}
