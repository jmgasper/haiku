/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "CsfReset.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>


static void
PrintIdentityPlatform(const char* prefix, const char* phase, const MaliCSF::PlatformSnapshot& snapshot)
{
	printf("ROCK5_MALI_%s_PLATFORM phase=%s start_us=%" PRId64 " end_us=%" PRId64
		" select=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
		" gate=%08" PRIx32 ",%08" PRIx32
		" idle_req=%08" PRIx32 " idle_ack=%08" PRIx32 " idle=%08" PRIx32
		" power_req=%08" PRIx32 " repair=%08" PRIx32 "\n",
		prefix, phase, snapshot.startedMicros, snapshot.finishedMicros,
		snapshot.clockSelect[0], snapshot.clockSelect[1], snapshot.clockSelect[2],
		snapshot.clockGate[0], snapshot.clockGate[1], snapshot.idleRequest,
		snapshot.idleAck, snapshot.idleStatus, snapshot.powerRequest, snapshot.powerRepair);
}


static bool
ReportIdentity(const MaliCSF::IdentityInfo& info, const char* prefix)
{
	printf("ROCK5_MALI_%s version=%" PRIu32 " result=%" PRIu32
		" restore=%" PRIu32 " flags=%08" PRIx32 " clock_hz=%" PRIu32
		" gpu=%08" PRIx32 " csf=%08" PRIx32 " mmu=%08" PRIx32 " as=%08" PRIx32
		" shader=%08" PRIx32 ",%08" PRIx32 " revision=%08" PRIx32
		" start_us=%" PRId64 " end_us=%" PRId64 "\n",
		prefix, info.version, info.result, info.restoreResult, info.flags, info.clockHertz,
		info.gpuID, info.csfID, info.mmuFeatures, info.addressSpaces,
		info.shaderPresentLow, info.shaderPresentHigh, info.gpuRevision,
		info.startedMicros, info.finishedMicros);
	PrintIdentityPlatform(prefix, "before", info.before);
	PrintIdentityPlatform(prefix, "powered", info.powered);
	PrintIdentityPlatform(prefix, "after", info.after);
	if (info.version != MaliCSF::kIdentityVersion
		|| info.result != MaliCSF::kIdentityOK || info.restoreResult != MaliCSF::kIdentityOK
		|| info.flags != (MaliCSF::kIdentityRead | MaliCSF::kIdentityRestored | MaliCSF::kIdentityChangedRegisters)
		|| info.clockHertz != 175500000 || info.gpuID != 0xa8670005 || info.csfID != 0x040a0412
		|| info.mmuFeatures != 0x2830 || info.addressSpaces != 0xff
		|| info.shaderPresentLow != 0x50005 || info.shaderPresentHigh != 0 || info.gpuRevision != 0
		|| info.startedMicros < 0 || info.finishedMicros < info.startedMicros
		|| !MaliCSF::IdentityInitialStateMatches(info.before)
		|| !MaliCSF::IdentityPoweredStateMatches(info.powered)
		|| !MaliCSF::IdentityInitialStateMatches(info.after)
		|| memcmp((const uint8_t*)&info.before + offsetof(MaliCSF::PlatformSnapshot, clockSelect),
			(const uint8_t*)&info.after + offsetof(MaliCSF::PlatformSnapshot, clockSelect),
			sizeof(MaliCSF::PlatformSnapshot) - offsetof(MaliCSF::PlatformSnapshot, clockSelect)) != 0) {
		fprintf(stderr, "Mali power/identity cycle or restoration failed\n");
		return false;
	}
	return true;
}


static bool
CheckIdentity(int fd)
{
	MaliCSF::IdentityInfo info = {};
	if (ioctl(fd, MaliCSF::kCycleIdentity, &info, sizeof(info) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed identity request was not rejected\n");
		return false;
	}
	if (ioctl(fd, MaliCSF::kCycleIdentity, NULL, sizeof(info)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null identity output was not rejected\n");
		return false;
	}
	if (ioctl(fd, MaliCSF::kCycleIdentity, &info, sizeof(info)) != 0) {
		perror("Mali identity cycle");
		return false;
	}
	if (!ReportIdentity(info, "IDENTITY"))
		return false;
	puts("ROCK5_MALI_IDENTITY_CYCLE_PASS gpu_accessed=1 gpu_commands=0 restored=1");
	return true;
}


static void
PrintResetSnapshot(const char* phase, const MaliCSF::ResetSnapshot& value)
{
	printf("ROCK5_MALI_RESET_REGISTERS phase=%s mask=%08" PRIx32
		" raw=%08" PRIx32 " irq_status=%08" PRIx32 " status=%08" PRIx32
		" mcu=%08" PRIx32 " job_mask=%08" PRIx32 " mmu_mask=%08" PRIx32
		" shader=%08" PRIx32 ",%08" PRIx32 " tiler=%08" PRIx32 ",%08" PRIx32
		" l2=%08" PRIx32 ",%08" PRIx32 " transition=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
		phase, value.mask, value.raw, value.interruptStatus, value.status, value.mcu,
		value.jobMask, value.mmuMask, value.shaderReady[0], value.shaderReady[1],
		value.tilerReady[0], value.tilerReady[1], value.l2Ready[0], value.l2Ready[1],
		value.shaderTransition, value.tilerTransition, value.l2Transition);
}


static bool
CheckReset(int fd)
{
	MaliCSF::ResetInfo info = {};
	if (ioctl(fd, MaliCSF::kCycleReset, &info, sizeof(info) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed reset request was not rejected\n");
		return false;
	}
	if (ioctl(fd, MaliCSF::kCycleReset, NULL, sizeof(info)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null reset output was not rejected\n");
		return false;
	}
	if (ioctl(fd, MaliCSF::kCycleReset, &info, sizeof(info)) != 0) {
		perror("Mali reset cycle");
		return false;
	}
	printf("ROCK5_MALI_RESET version=%" PRIu32 " result=%" PRIu32
		" cleanup=%" PRIu32 " flags=%08" PRIx32 " irq=%" PRIu32
		" completion_raw=%08" PRIx32 " start_us=%" PRId64 " end_us=%" PRId64 "\n",
		info.version, info.result, info.cleanupResult, info.flags, info.interrupt,
		info.completionRaw, info.startedMicros, info.finishedMicros);
	printf("ROCK5_MALI_RESET_INTERRUPT count=%" PRIu32 " status=%08" PRIx32
		" raw=%08" PRIx32 " gpu_status=%08" PRIx32 " cpu=%" PRId32 " time_us=%" PRId64 "\n",
		info.capture.count, info.capture.status, info.capture.raw,
		info.capture.gpuStatus, info.capture.cpu, info.capture.whenMicros);
	PrintResetSnapshot("before", info.before);
	PrintResetSnapshot("after", info.after);
	bool power = ReportIdentity(info.power, "RESET_POWER");
	if (!power || info.version != MaliCSF::kResetVersion
		|| info.result != MaliCSF::kResetOK || info.cleanupResult != MaliCSF::kResetOK
		|| info.flags != (MaliCSF::kResetCommandIssued | MaliCSF::kResetInterruptObserved | MaliCSF::kResetCleaned)
		|| info.interrupt != 126 || info.reserved[0] != 0 || info.reserved[1] != 0
		|| !MaliCSF::ResetCaptureMatches(info.capture, info.startedMicros)
		|| info.finishedMicros < info.capture.whenMicros
		|| info.startedMicros < info.power.powered.finishedMicros
		|| info.finishedMicros > info.power.after.startedMicros
		|| !MaliCSF::ResetIdleMatches(info.before) || !MaliCSF::ResetIdleMatches(info.after)
		|| (info.after.raw & MaliCSF::kResetCompleted) != 0) {
		fprintf(stderr, "Mali reset, interrupt delivery or cleanup failed\n");
		return false;
	}
	puts("ROCK5_MALI_RESET_CYCLE_PASS gpu_commands=1 interrupt_observed=1 firmware_started=0 restored=1");
	return true;
}


int
main(int argc, char** argv)
{
	bool reset = argc > 1 && strcmp(argv[1], "--reset") == 0;
	bool identity = argc > 1 && strcmp(argv[1], "--identity") == 0;
	bool platform = identity || (argc > 1 && strcmp(argv[1], "--platform") == 0);
	int argument = reset || platform ? 2 : 1;
	if (argc > argument + 1) {
		fprintf(stderr, "usage: %s [--platform|--identity|--reset] [device]\n", argv[0]);
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
	bool success = reset ? CheckReset(fd) : (!identity || CheckIdentity(fd));
	close(fd);
	return success ? 0 : 1;
}
