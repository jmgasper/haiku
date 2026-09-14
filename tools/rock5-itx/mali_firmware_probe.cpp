/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "CsfFirmware.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s mali_csffw.bin\n", argv[0]);
		return 2;
	}
	FILE* file = fopen(argv[1], "rb");
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
	free(data);
	puts("ROCK5_MALI_FIRMWARE_CONTAINER_PASS gpu_started=0");
	return 0;
}
