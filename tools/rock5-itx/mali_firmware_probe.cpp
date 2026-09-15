/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "CsfFirmware.h"
#include "CsfRun.h"
#include "CsfCommands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>


static bool
RunFirmware(const void* data, size_t bytes)
{
	using namespace MaliCSF;
	int fd = open("/dev/graphics/mali_csf/0", O_RDONLY);
	if (fd < 0) {
		perror("open Mali firmware interface");
		return false;
	}
	size_t requestBytes = sizeof(FirmwareRunInfo) + bytes;
	FirmwareRunInfo* request = (FirmwareRunInfo*)calloc(1, requestBytes);
	if (request == NULL) { close(fd); return false; }
	request->version = kFirmwareRunVersion;
	request->firmwareBytes = bytes;
	memcpy((uint8_t*)request + sizeof(*request), data, bytes);
	if (ioctl(fd, kCycleFirmware, request, sizeof(*request) - 1) == 0 || errno != EINVAL
		|| ioctl(fd, kCycleFirmware, NULL, requestBytes) == 0 || errno != EFAULT) {
		fprintf(stderr, "Malformed firmware request was not rejected\n");
		free(request); close(fd); return false;
	}
	if (ioctl(fd, kCycleFirmware, request, requestBytes) != 0) {
		perror("Mali firmware cycle");
		free(request); close(fd); return false;
	}
	close(fd);
	const FirmwareRunInfo& info = *request;
	printf("ROCK5_MALI_FW_RUN version=%" PRIu32 " result=%" PRIu32
		" cleanup=%" PRIu32 " flags=%08" PRIx32 " tables=%" PRIu32
		" bytes=%" PRIu32 " root=%016" PRIx64 " mcu=%" PRIu32
		" start_us=%" PRId64 " end_us=%" PRId64 "\n", info.version, info.result,
		info.cleanupResult, info.flags, info.tablePages, info.allocationBytes,
		info.rootPhysical, info.mcuBootStatus, info.startedMicros, info.finishedMicros);
	printf("ROCK5_MALI_FW_INTERFACE version=%08" PRIx32 " features=%08" PRIx32
		" groups=%" PRIu32 " streams=%" PRIu32 " registers=%" PRIu32
		" scoreboards=%" PRIu32 " boot_irq=%" PRIu32 " ping_irq=%" PRIu32
		" request=%08" PRIx32 " ack=%08" PRIx32 "\n", info.interface.version,
		info.interface.features, info.interface.groupCount, info.interface.streamCount,
		info.interface.workRegisters, info.interface.scoreboards, info.boot.count,
		info.ping.count, info.pingRequest, info.pingAck);
	// Preserve every raw register, event and platform phase for an independent
	// controller decoder. The fixed ABI is entirely initialized by the kernel.
	printf("ROCK5_MALI_FW_ABI bytes=%zu hex=", sizeof(info));
	for (size_t i = 0; i < sizeof(info); i++) printf("%02x", ((const uint8_t*)&info)[i]);
	putchar('\n');
	bool ok = info.version == kFirmwareRunVersion && info.firmwareBytes == bytes
		&& info.result == kFirmwareRunOK && info.cleanupResult == kFirmwareRunOK
		&& info.flags == 255 && info.tablePages == 9 && info.allocationBytes == 954368
		&& info.translationConfig == UINT64_C(0x420001c6)
		&& info.memoryAttributes == UINT64_C(0xc0c0c0c0c0c08f4c)
		&& info.interface.version == 0x01050000 && info.interface.groupCount == 8
		&& info.interface.streamCount == 8 && info.interface.workRegisters == 96
		&& info.interface.scoreboards == 8 && info.mcuBootStatus == 1
		&& FirmwareEventMatches(info.boot, info.startedMicros, info.finishedMicros)
		&& FirmwareEventMatches(info.ping, info.boot.whenMicros, info.finishedMicros)
		&& ((info.pingRequest ^ info.pingAck) & kFirmwarePing) == 0
		&& info.gpuFault.count == 0 && info.mmuFault.count == 0
		&& info.power.result == kIdentityOK && info.power.restoreResult == kIdentityOK
		&& info.power.flags == 7 && ResetIdleMatches(info.after)
		&& info.jobRawAfter == 0 && (info.mmuRawAfter & ~kMmuAs0Completed) == 0
		&& info.asStatusAfter == 0 && info.asConfigAfter == 1;
	free(request);
	if (ok)
		puts("ROCK5_MALI_FIRMWARE_CYCLE_PASS firmware_started=1 ping_acknowledged=1 restored=1 rendered=0");
	else
		fprintf(stderr, "Mali firmware cycle failed; retain evidence and recover the board\n");
	return ok;
}

static bool
RunCommands(const void* data, size_t bytes)
{
	using namespace MaliCSF;
	int fd = open("/dev/graphics/mali_csf/0", O_RDONLY);
	if (fd < 0) { perror("open Mali command interface"); return false; }
	size_t requestBytes = sizeof(CommandRunInfo) + bytes;
	CommandRunInfo* request = (CommandRunInfo*)calloc(1, requestBytes);
	if (request == NULL) { close(fd); return false; }
	request->version = kCommandRunVersion;
	request->firmwareBytes = bytes;
	memcpy((uint8_t*)request + sizeof(*request), data, bytes);
	if (ioctl(fd, kCycleCommands, request, sizeof(*request) - 1) == 0 || errno != EINVAL
		|| ioctl(fd, kCycleCommands, NULL, requestBytes) == 0 || errno != EFAULT) {
		fprintf(stderr, "Malformed command request was not rejected\n");
		free(request); close(fd); return false;
	}
	if (ioctl(fd, kCycleCommands, request, requestBytes) != 0) {
		perror("Mali command cycle"); free(request); close(fd); return false;
	}
	close(fd);
	const CommandRunInfo& info = *request;
	printf("ROCK5_MALI_COMMAND_RUN version=%" PRIu32 " result=%" PRIu32
		" cleanup=%" PRIu32 " flags=%08" PRIx32 " rounds=%" PRIu32
		" bytes=%" PRIu32 " root=%016" PRIx64 " fw_result=%" PRIu32
		" fw_cleanup=%" PRIu32 " fw_flags=%08" PRIx32 "\n", info.version,
		info.result, info.cleanupResult, info.flags, info.roundsCompleted,
		info.arenaBytes, info.rootPhysical, info.firmware.result,
		info.firmware.cleanupResult, info.firmware.flags);
	// Full fixed ABI and every buffer, in bounded lines for reliable capture.
	// The host decoder must reject missing, duplicate or reordered chunks.
	for (size_t offset = 0; offset < sizeof(info); offset += 256) {
		size_t size = sizeof(info) - offset;
		if (size > 256) size = 256;
		printf("ROCK5_MALI_COMMAND_ABI total=%zu offset=%zu hex=", sizeof(info), offset);
		for (size_t n = 0; n < size; n++) printf("%02x", ((const uint8_t*)&info)[offset + n]);
		putchar('\n');
	}
	bool ok = info.version == kCommandRunVersion && info.firmwareBytes == bytes
		&& info.result == kCommandOK && info.cleanupResult == kCommandOK
		&& info.flags == 511 && info.roundsCompleted == 2
		&& info.firmware.result == kFirmwareRunOK && info.firmware.cleanupResult == kFirmwareRunOK
		&& info.firmware.flags == 255 && info.asStatusAfter == 0 && info.asConfigAfter == 1
		&& info.haltStatus == 2 && info.streamFault == 0 && info.streamFatal == 0;
	for (unsigned r = 0; r < 2; r++) {
		const CommandRoundInfo& round = info.rounds[r];
		ok = ok && round.sequenceBefore == 0 && round.sequenceAfter == 1
			&& round.status == 0 && round.irqAfter > round.irqBefore
			&& round.syncAfter > round.syncBefore && round.mismatches == 0
			&& round.extract == (r + 1) * 128 && round.insert == round.extract
			&& round.finishedMicros >= round.startedMicros
			&& round.finishedMicros - round.startedMicros <= 5000000;
	}
	free(request);
	if (ok)
		puts("ROCK5_MALI_COMMAND_CYCLE_PASS submissions=2 checked_words=4096 restored=1 rendered=0");
	else
		fprintf(stderr, "Mali command cycle failed; retain evidence and recover the board\n");
	return ok;
}


int
main(int argc, char** argv)
{
	bool start = argc == 3 && strcmp(argv[1], "--start") == 0;
	bool commands = argc == 3 && strcmp(argv[1], "--commands") == 0;
	if (argc != 2 && !start && !commands) {
		fprintf(stderr, "usage: %s [--start|--commands] mali_csffw.bin\n", argv[0]);
		return 2;
	}
	FILE* file = fopen(argv[argc - 1], "rb");
	if (file == NULL) {
		fprintf(stderr, "open: %s\n", strerror(errno));
		return 1;
	}
	struct stat stat;
	if (fstat(fileno(file), &stat) != 0 || !S_ISREG(stat.st_mode)
		|| stat.st_size < 20
		|| uint64_t(stat.st_size) > MaliCSF::kMaxFirmwareBytes) {
		fprintf(stderr, "firmware must be a regular file of 20..%zu bytes\n",
			MaliCSF::kMaxFirmwareBytes);
		fclose(file);
		return 1;
	}
	size_t size = size_t(stat.st_size);
	void* data = malloc(size);
	if (data == NULL) {
		fclose(file);
		return 1;
	}
	bool readOK = fread(data, 1, size, file) == size;
	int extra = fgetc(file);
	readOK = readOK && extra == EOF && !ferror(file);
	if (fclose(file) != 0)
		readOK = false;
	if (!readOK) {
		fprintf(stderr, "firmware read failed or file size changed\n");
		free(data);
		return 1;
	}

	MaliCSF::FirmwareImage firmware;
	MaliCSF::FirmwareStatus status = firmware.Init(data, size);
	if (status != MaliCSF::FIRMWARE_OK) {
		fprintf(stderr, "firmware: %s\n", MaliCSF::FirmwareStatusName(status));
		free(data);
		return 1;
	}
	const MaliCSF::FirmwareInfo& info = firmware.Info();
	printf("ROCK5_MALI_FIRMWARE version=%u.%u hash=%08" PRIx32
		" file_bytes=%zu table_bytes=%" PRIu32 " sections=%" PRIu32
		" protected=%" PRIu32 " ignored=%" PRIu32 " mapped_bytes=%" PRIu64 "\n",
		info.major, info.minor, info.versionHash, size, info.tableSize,
		info.sectionCount, info.protectedSectionCount, info.ignoredEntryCount,
		info.mappedBytes);
	for (uint32_t i = 0; i < info.sectionCount; i++) {
		MaliCSF::FirmwareSection section;
		if (firmware.GetSection(i, section) != MaliCSF::FIRMWARE_OK) {
			free(data);
			return 1;
		}
		void* copy = malloc(section.memorySize);
		if (copy == NULL || firmware.CopySection(i, copy, section.memorySize)
				!= MaliCSF::FIRMWARE_OK) {
			free(copy);
			free(data);
			return 1;
		}
		printf("ROCK5_MALI_SECTION index=%" PRIu32 " va=%08" PRIx32
			" bytes=%" PRIu32 " flags=%08" PRIx32 " data_offset=%" PRIu32
			" data_bytes=%" PRIu32 "\n", i, section.virtualAddress,
			section.memorySize, section.flags, section.dataOffset, section.dataSize);
		free(copy);
	}
	puts("ROCK5_MALI_FIRMWARE_CONTAINER_PASS gpu_started=0");
	bool ok = commands ? RunCommands(data, size) : !start || RunFirmware(data, size);
	free(data);
	return ok ? 0 : 1;
}
