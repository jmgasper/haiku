/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "../../src/add-ons/kernel/drivers/video/rk3588_vpu/VpuInterface.h"
#include "../../src/add-ons/kernel/drivers/video/rk3588_vpu/VpuPower.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace RK3588Vpu;

int
main(int argc, char** argv)
{
	bool cycle = argc == 2 && strcmp(argv[1], "--cycle") == 0;
	bool identity = argc == 2 && strcmp(argv[1], "--id") == 0;
	bool dma = argc == 2 && strcmp(argv[1], "--dma") == 0;
	bool buffer = argc == 2 && strcmp(argv[1], "--buffer") == 0;
	bool mpp = argc == 2 && strcmp(argv[1], "--mpp") == 0;
	bool rkvCycle = argc == 2 && strcmp(argv[1], "--rkvdec0-cycle") == 0;
	bool rkvWithParent = argc == 2
		&& strcmp(argv[1], "--rkvdec0-with-vdpu") == 0;
	bool rkvRegisters = argc == 2
		&& strcmp(argv[1], "--rkvdec0-registers") == 0;
	bool av1WithParent = argc == 2
		&& strcmp(argv[1], "--av1-with-vdpu") == 0;
	bool av1Registers = argc == 2
		&& strcmp(argv[1], "--av1-registers") == 0;
	if (argc > 2 || (argc == 2 && !cycle && !identity && !dma && !buffer && !mpp
			&& !rkvCycle && !rkvWithParent && !rkvRegisters
			&& !av1WithParent && !av1Registers)) {
		fprintf(stderr, "usage: %s [--cycle|--id|--dma|--buffer|--mpp|--rkvdec0-cycle"
			"|--rkvdec0-with-vdpu|--rkvdec0-registers|--av1-with-vdpu"
			"|--av1-registers]\n",
			argv[0]);
		return 2;
	}
	int fd = open("/dev/video/rk3588_vpu/0",
		cycle || identity || dma || buffer || mpp || rkvCycle || rkvWithParent
			|| rkvRegisters || av1WithParent || av1Registers
			? O_RDWR : O_RDONLY);
	if (fd < 0) {
		perror("open VPU");
		return 1;
	}
	ResourceInfo resources = {};
	if (ioctl(fd, kGetResources, &resources, sizeof(resources)) != 0) {
		perror("VPU resources");
		close(fd);
		return 1;
	}
	printf("ROCK5_VPU_RESOURCES version=%u flags=%u decoder=%#" PRIx64
		"/%#" PRIx64 " iommu=%#" PRIx64 "/%#" PRIx64
		" cru=%#" PRIx64 "/%#" PRIx64 " pmu=%#" PRIx64 "/%#" PRIx64
		" irq=%u iommu_irq=%u clocks=%u,%u domain=%u\n",
		resources.version, resources.flags, resources.decoderBase,
		resources.decoderSize, resources.iommuBase, resources.iommuSize,
		resources.clockBase, resources.clockSize, resources.powerBase,
		resources.powerSize, resources.decoderInterrupt, resources.iommuInterrupt,
		resources.clockIds[0], resources.clockIds[1], resources.powerDomain);
	if (!ResourcesMatch(resources)) {
		fprintf(stderr, "VPU FDT resource mismatch\n");
		close(fd);
		return 1;
	}
	if (mpp) {
		MppValidationStats stats = {};
		if (ioctl(fd, kGetMppValidation, &stats, sizeof(stats)) != 0) {
			perror("MPP validation statistics");
			close(fd);
			return 1;
		}
		printf("ROCK5_MPP_VALIDATION version=%u jobs=%u writes=%u reads=%u"
			" offsets=%u rcb=%u handles=%u entries=%u status=%" PRId32 "\n",
			stats.version,
			stats.jobs, stats.writeRequests, stats.readRequests,
			stats.offsetRequests, stats.rcbRequests, stats.addressHandles,
			stats.offsetEntries, stats.lastStatus);
		close(fd);
		return stats.version == 2 && stats.jobs > 0
			&& stats.writeRequests > 0 && stats.readRequests == stats.jobs
			&& stats.addressHandles > 0
			&& stats.lastStatus == B_NOT_SUPPORTED ? 0 : 1;
	}
	if (buffer) {
		BufferAllocation allocation = {};
		allocation.version = kBufferVersion;
		allocation.bytes = 1024 * 1024;
		if (ioctl(fd, kAllocBuffer, &allocation, sizeof(allocation)) != 0) {
			perror("VPU buffer allocation");
			close(fd);
			return 1;
		}
		bool valid = allocation.version == kBufferVersion
			&& allocation.handle != 0 && allocation.area >= 0
			&& allocation.address != 0 && allocation.bytes == 1024 * 1024;
		if (valid) {
			volatile uint8_t* address = (volatile uint8_t*)(uintptr_t)allocation.address;
			for (size_t offset = 0; offset < allocation.bytes; offset += 4096)
				address[offset] = (uint8_t)(offset / 4096);
			for (size_t offset = 0; offset < allocation.bytes; offset += 4096) {
				if (address[offset] != (uint8_t)(offset / 4096))
					valid = false;
			}
		}
		BufferRelease release = {kBufferVersion, allocation.handle};
		if (ioctl(fd, kFreeBuffer, &release, sizeof(release)) != 0) {
			perror("VPU buffer release");
			valid = false;
		}
		printf("ROCK5_VPU_BUFFER handle=%u area=%d bytes=%" PRIu64
			" mapped=%u\n", allocation.handle, allocation.area,
			allocation.bytes, valid ? 1 : 0);
		close(fd);
		return valid ? 0 : 1;
	}
	if (dma) {
		DmaProbe probe = {};
		if (ioctl(fd, kProbeDma, &probe, sizeof(probe)) != 0) {
			perror("VDPU DMA allocation probe");
			close(fd);
			return 1;
		}
		printf("ROCK5_VPU_DMA version=%u status=%" PRId32
			" physical=%#" PRIx64 " bytes=%" PRIu64 "\n",
			probe.version, probe.status, probe.physical, probe.bytes);
		close(fd);
		return probe.version == kDmaProbeVersion && probe.status == 0
			&& probe.bytes == 4096 && probe.physical < (UINT64_C(1) << 32)
			? 0 : 1;
	}
	Snapshot snapshot = {};
	if (ioctl(fd, kGetSnapshot, &snapshot, sizeof(snapshot)) != 0) {
		perror("VPU snapshot");
		close(fd);
		return 1;
	}
	printf("ROCK5_VPU_POWER version=%u flags=%u start=%" PRId64
		" end=%" PRId64 " idle_req=%08" PRIx32 " idle_ack=%08" PRIx32
		" idle_status=%08" PRIx32 " gate=%08" PRIx32
		" status=%08" PRIx32 " repair=%08" PRIx32
		" chain=%08" PRIx32 " memory=%08" PRIx32 "\n",
		snapshot.version, snapshot.flags, snapshot.startedMicros,
		snapshot.finishedMicros, snapshot.idleRequest, snapshot.idleAck,
		snapshot.idleStatus, snapshot.powerGate, snapshot.powerStatus,
		snapshot.repairStatus, snapshot.chainStatus, snapshot.memoryStatus);
	printf("ROCK5_VPU_CLOCK select98=%08" PRIx32 " gate40=%08" PRIx32
		" gate41=%08" PRIx32 " gate44=%08" PRIx32 " gate45=%08" PRIx32
		" gate68=%08" PRIx32 " select89=%08" PRIx32
		" select90=%08" PRIx32 " select91=%08" PRIx32
		" select163=%08" PRIx32 " reset68=%08" PRIx32 "\n",
		snapshot.clockSelect98,
		snapshot.clockGate40, snapshot.clockGate41, snapshot.clockGate44,
		snapshot.clockGate45, snapshot.clockGate68, snapshot.clockSelect89,
		snapshot.clockSelect90, snapshot.clockSelect91,
		snapshot.clockSelect163, snapshot.softReset68);
	printf("ROCK5_VPU_DOMAINS rkvdec0=%u rkvdec1=%u vdpu=%u av1=%u\n",
		(snapshot.repairStatus >> 7) & 1, (snapshot.repairStatus >> 8) & 1,
		(snapshot.repairStatus >> 9) & 1, (snapshot.repairStatus >> 11) & 1);
	if (rkvRegisters) {
		Rkvdec0RegisterProbe probe = {};
		if (ioctl(fd, kProbeRkvdec0Registers, &probe, sizeof(probe)) != 0) {
			perror("RKVDEC0 register observation");
			close(fd);
			return 1;
		}
		printf("ROCK5_RKVDEC0_REGISTERS version=%u status=%" PRId32
			" link=%08" PRIx32 ",%08" PRIx32
			" function=%08" PRIx32 ",%08" PRIx32
			" interrupt=%08" PRIx32
			" iommu=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
			" parent=%u/%u/%#x child=%u/%u/%#x\n",
			probe.version, probe.readStatus, probe.link[0], probe.link[1],
			probe.function[0], probe.function[1], probe.interruptStatus,
			probe.iommuDte, probe.iommuStatus, probe.iommuFault,
			probe.cycle.parent.result, probe.cycle.parent.restoreResult,
			probe.cycle.parent.flags, probe.cycle.child.result,
			probe.cycle.child.restoreResult, probe.cycle.child.flags);
		bool ok = probe.version == 1 && probe.readStatus == 0
			&& probe.cycle.version == 1
			&& probe.cycle.parent.result == kPowerOK
			&& probe.cycle.parent.restoreResult == kPowerOK
			&& probe.cycle.parent.flags == 7
			&& probe.cycle.child.result == kPowerOK
			&& probe.cycle.child.restoreResult == kPowerOK
			&& probe.cycle.child.flags == 7
			&& RestoredStateMatches(probe.cycle.parent.before,
				probe.cycle.parent.after)
			&& RestoredRkvdec0StateMatches(probe.cycle.child.before,
				probe.cycle.child.after);
		close(fd);
		return ok ? 0 : 1;
	}
	if (av1Registers) {
		Av1RegisterProbe probe = {};
		if (ioctl(fd, kProbeAv1Registers, &probe, sizeof(probe)) != 0) {
			perror("AV1 register observation");
			close(fd);
			return 1;
		}
		printf("ROCK5_AV1_REGISTERS version=%u status=%" PRId32
			" registers=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
			" build=%08" PRIx32 " synthesis=%08" PRIx32
			" parent=%u/%u/%#x child=%u/%u/%#x\n",
			probe.version, probe.readStatus, probe.registers[0],
			probe.registers[1], probe.registers[2], probe.buildId,
			probe.synthesisId, probe.cycle.parent.result,
			probe.cycle.parent.restoreResult, probe.cycle.parent.flags,
			probe.cycle.child.result, probe.cycle.child.restoreResult,
			probe.cycle.child.flags);
		bool ok = probe.version == 1 && probe.readStatus == 0
			&& probe.cycle.version == 1
			&& probe.cycle.parent.result == kPowerOK
			&& probe.cycle.parent.restoreResult == kPowerOK
			&& probe.cycle.parent.flags == 7
			&& probe.cycle.child.result == kPowerOK
			&& probe.cycle.child.restoreResult == kPowerOK
			&& probe.cycle.child.flags == 7
			&& RestoredStateMatches(probe.cycle.parent.before,
				probe.cycle.parent.after)
			&& RestoredAv1StateMatches(probe.cycle.child.before,
				probe.cycle.child.after);
		close(fd);
		return ok ? 0 : 1;
	}
	if (av1WithParent) {
		ParentChildCycle pair = {};
		if (ioctl(fd, kCycleAv1WithVdpu, &pair, sizeof(pair)) != 0) {
			perror("AV1 with VDPU parent cycle");
			close(fd);
			return 1;
		}
		printf("ROCK5_AV1_WITH_VDPU version=%u parent=%u/%u/%#x"
			" child=%u/%u/%#x parent_before=%08" PRIx32 "/%08" PRIx32
			" child_powered=%08" PRIx32 "/%08" PRIx32
			" parent_after=%08" PRIx32 "/%08" PRIx32 "\n",
			pair.version, pair.parent.result, pair.parent.restoreResult,
			pair.parent.flags, pair.child.result, pair.child.restoreResult,
			pair.child.flags, pair.parent.before.powerGate,
			pair.parent.before.repairStatus, pair.child.powered.powerGate,
			pair.child.powered.repairStatus, pair.parent.after.powerGate,
			pair.parent.after.repairStatus);
		bool ok = pair.version == 1 && pair.parent.result == kPowerOK
			&& pair.parent.restoreResult == kPowerOK && pair.parent.flags == 7
			&& pair.child.result == kPowerOK
			&& pair.child.restoreResult == kPowerOK && pair.child.flags == 7
			&& RestoredStateMatches(pair.parent.before, pair.parent.after)
			&& RestoredAv1StateMatches(pair.child.before, pair.child.after);
		close(fd);
		return ok ? 0 : 1;
	}
	if (rkvWithParent) {
		ParentChildCycle pair = {};
		if (ioctl(fd, kCycleRkvdec0WithVdpu, &pair, sizeof(pair)) != 0) {
			perror("RKVDEC0 with VDPU parent cycle");
			close(fd);
			return 1;
		}
		printf("ROCK5_RKVDEC0_WITH_VDPU version=%u parent=%u/%u/%#x"
			" child=%u/%u/%#x parent_before=%08" PRIx32 "/%08" PRIx32
			" parent_powered=%08" PRIx32 "/%08" PRIx32
			" child_powered=%08" PRIx32 "/%08" PRIx32
			" parent_after=%08" PRIx32 "/%08" PRIx32 "\n",
			pair.version, pair.parent.result, pair.parent.restoreResult,
			pair.parent.flags, pair.child.result, pair.child.restoreResult,
			pair.child.flags, pair.parent.before.powerGate,
			pair.parent.before.repairStatus, pair.parent.powered.powerGate,
			pair.parent.powered.repairStatus, pair.child.powered.powerGate,
			pair.child.powered.repairStatus, pair.parent.after.powerGate,
			pair.parent.after.repairStatus);
		bool ok = pair.version == 1
			&& pair.parent.version == kPowerCycleVersion
			&& pair.parent.result == kPowerOK
			&& pair.parent.restoreResult == kPowerOK
			&& pair.parent.flags == 7
			&& RestoredStateMatches(pair.parent.before, pair.parent.after)
			&& pair.child.version == kPowerCycleVersion
			&& pair.child.result == kPowerOK
			&& pair.child.restoreResult == kPowerOK
			&& pair.child.flags == 7
			&& RestoredRkvdec0StateMatches(pair.child.before,
				pair.child.after);
		close(fd);
		return ok ? 0 : 1;
	}
	if (cycle || identity || rkvCycle) {
		PowerCycle result = {};
		if (identity) {
			DecoderProbe probe = {};
			if (ioctl(fd, kProbeDecoder, &probe, sizeof(probe)) != 0) {
				perror("VDPU decoder probe");
				close(fd);
				return 1;
			}
			printf("ROCK5_VPU_ID version=%u status=%" PRId32
				" id=%08" PRIx32 " iommu_dte=%08" PRIx32
				" iommu_status=%08" PRIx32 " iommu_raw=%08" PRIx32
				" iommu_mask=%08" PRIx32 "\n", probe.version,
				probe.readStatus, probe.decoderId, probe.iommuDte,
				probe.iommuStatus, probe.iommuInterruptRaw,
				probe.iommuInterruptMask);
			if (probe.version != kDecoderProbeVersion
				|| probe.readStatus != 0) {
				close(fd);
				return 1;
			}
			result = probe.cycle;
		} else if (ioctl(fd,
				rkvCycle ? kCycleRkvdec0Power : kCycleVdpuPower,
				&result, sizeof(result)) != 0) {
			perror(rkvCycle ? "RKVDEC0 power cycle" : "VDPU power cycle");
			close(fd);
			return 1;
		}
		printf("%s version=%u result=%u restore=%u flags=%#x"
			" before=%08" PRIx32 "/%08" PRIx32
			" powered=%08" PRIx32 "/%08" PRIx32
			" after=%08" PRIx32 "/%08" PRIx32 "\n",
			rkvCycle ? "ROCK5_RKVDEC0_CYCLE" : "ROCK5_VPU_CYCLE",
			result.version, result.result, result.restoreResult, result.flags,
			result.before.powerGate, result.before.repairStatus,
			result.powered.powerGate, result.powered.repairStatus,
			result.after.powerGate, result.after.repairStatus);
		const Snapshot* stages[] = {&result.before, &result.powered, &result.after};
		const char* names[] = {"before", "powered", "after"};
		for (unsigned i = 0; i < 3; i++) {
			const Snapshot& s = *stages[i];
			printf("ROCK5_VPU_STAGE %s req=%08" PRIx32 " ack=%08" PRIx32
				" idle=%08" PRIx32 " gate=%08" PRIx32
				" status=%08" PRIx32 " repair=%08" PRIx32
				" select=%08" PRIx32 " gates=%08" PRIx32 ",%08" PRIx32
				"\n", names[i], s.idleRequest, s.idleAck, s.idleStatus,
				s.powerGate, s.powerStatus, s.repairStatus, s.clockSelect98,
				s.clockGate44, s.clockGate45);
		}
		if (result.version != kPowerCycleVersion || result.result != kPowerOK
			|| result.restoreResult != kPowerOK || result.flags != 7
			|| !(rkvCycle
				? RestoredRkvdec0StateMatches(result.before, result.after)
				: RestoredStateMatches(result.before, result.after))) {
			close(fd);
			return 1;
		}
	}
	close(fd);
	return snapshot.version == kSnapshotVersion && snapshot.flags == 1
		&& snapshot.finishedMicros >= snapshot.startedMicros ? 0 : 1;
}
