/*
 * Reading what MediaTek ships for this part.
 *
 * Two files, in two different layouts, for no reason either of them explains.
 *
 * The patch is what the bootloader takes first. It opens with a header whose
 * numbers are big-endian - alone among everything else here - and its sections
 * follow the header in a table.
 *
 * The RAM image is the code the part actually runs, and it is written back to
 * front: its table of contents sits at the very end of the file, little-endian,
 * with the regions it describes immediately before it and their contents from
 * the beginning. Its bytes are encrypted, so nothing in it can be checked by
 * looking, which is why the arithmetic is checked instead: the pieces have to
 * account for the whole file, and a layout read wrongly does not add up.
 *
 * Distributed under the terms of the MIT License.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <ByteOrder.h>
#include <FindDirectory.h>
#include <StorageDefs.h>

#include "mt7922.h"


#define TRACE(x...)	dprintf("mt7922: " x)
#define ERROR(x...)	dprintf("mt7922: " x)

#define FIRMWARE_DIRECTORY	"/firmware/mt7922/"
#define MAX_FIRMWARE_SIZE	(8 * 1024 * 1024)

/* The patch: a header, then a table of sections, then their contents. */
#define PATCH_HEADER_SIZE	96
#define PATCH_SECTION_SIZE	64
#define PATCH_N_REGION_OFFSET	0x2c
#define PATCH_HW_SW_VER_OFFSET	0x14

/* The RAM image: contents, then a table of regions, then a trailer, counting
 * backwards from the end of the file.
 */
#define RAM_TRAILER_SIZE	36
#define RAM_REGION_SIZE		40

/* What a region says about itself. */
#define FW_FEATURE_ENCRYPTED	(1 << 0)
#define FW_FEATURE_OVERRIDE	(1 << 5)
#define FW_FEATURE_NOT_DOWNLOADED (1 << 6)

#define MAX_REGIONS		16


static uint32
read_be32(const uint8* bytes)
{
	return ((uint32)bytes[0] << 24) | ((uint32)bytes[1] << 16)
		| ((uint32)bytes[2] << 8) | bytes[3];
}


static uint32
read_le32(const uint8* bytes)
{
	return bytes[0] | ((uint32)bytes[1] << 8) | ((uint32)bytes[2] << 16)
		| ((uint32)bytes[3] << 24);
}


static status_t
load_file(const char* name, uint8** _data, size_t* _size)
{
	char path[B_PATH_NAME_LENGTH];
	directory_which places[] = { B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY };
	int fd = -1;

	for (size_t i = 0; i < B_COUNT_OF(places); i++) {
		if (find_directory(places[i], -1, false, path, sizeof(path)) != B_OK)
			continue;

		strlcat(path, FIRMWARE_DIRECTORY, sizeof(path));
		strlcat(path, name, sizeof(path));

		fd = open(path, B_READ_ONLY);
		if (fd >= 0)
			break;
	}

	if (fd < 0) {
		ERROR("no %s to be found\n", name);
		return B_ENTRY_NOT_FOUND;
	}

	off_t size = lseek(fd, 0, SEEK_END);
	if (size <= 0 || size > MAX_FIRMWARE_SIZE) {
		ERROR("%s is %" B_PRIdOFF " bytes, which cannot be right\n", path,
			size);
		close(fd);
		return B_BAD_DATA;
	}
	lseek(fd, 0, SEEK_SET);

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}

	ssize_t read_bytes = read(fd, data, size);
	close(fd);

	if (read_bytes != (ssize_t)size) {
		ERROR("could only read %" B_PRIdSSIZE " of %" B_PRIdOFF " bytes of "
			"%s\n", read_bytes, size, path);
		free(data);
		return B_IO_ERROR;
	}

	*_data = data;
	*_size = (size_t)size;
	return B_OK;
}


/* The patch the bootloader takes before anything else. Its numbers are
 * big-endian, and the version in its header names the revision of the part it
 * was built for - which had better be the one in front of us.
 */
status_t
mt7922_firmware_read_patch(mt7922_dev* device, mt7922_firmware* firmware)
{
	status_t status = load_file("WIFI_MT7922_patch_mcu_1_1_hdr.bin",
		&firmware->data, &firmware->size);
	if (status != B_OK)
		return status;

	if (firmware->size < PATCH_HEADER_SIZE + PATCH_SECTION_SIZE) {
		ERROR("the patch is too small to be one\n");
		goto bad;
	}

	firmware->count = read_be32(firmware->data + PATCH_N_REGION_OFFSET);
	if (firmware->count == 0 || firmware->count > MAX_REGIONS
		|| PATCH_HEADER_SIZE + firmware->count * PATCH_SECTION_SIZE
			> firmware->size) {
		ERROR("the patch says it has %" B_PRIu32 " sections\n",
			firmware->count);
		goto bad;
	}

	{
		uint32 version = read_be32(firmware->data + PATCH_HW_SW_VER_OFFSET);
		TRACE("patch built for hardware %#" B_PRIx32 ", %" B_PRIu32
			" section%s\n", version >> 16, firmware->count,
			firmware->count == 1 ? "" : "s");

		if ((version >> 16) != (device->revision & 0xffff)) {
			ERROR("this patch is for revision %#" B_PRIx32 ", the part is %#"
				B_PRIx32 "\n", version >> 16, device->revision & 0xffff);
			goto bad;
		}
	}

	/* The pieces have to account for the whole file. A layout read wrongly
	 * will not add up, and this is the only check available on bytes that
	 * cannot be read.
	 */
	{
		uint64 accounted = PATCH_HEADER_SIZE
			+ (uint64)firmware->count * PATCH_SECTION_SIZE;

		for (uint32 i = 0; i < firmware->count; i++) {
			const uint8* section = firmware->data + PATCH_HEADER_SIZE
				+ i * PATCH_SECTION_SIZE;

			firmware->region[i].offset = read_be32(section + 4);
			firmware->region[i].size = read_be32(section + 8);
			firmware->region[i].address = read_be32(section + 12);
			firmware->region[i].length = read_be32(section + 16);
			firmware->region[i].keyIndex = read_be32(section + 20);
			firmware->region[i].features = 0;

			if ((uint64)firmware->region[i].offset
					+ firmware->region[i].length > firmware->size) {
				ERROR("patch section %" B_PRIu32 " runs past the end\n", i);
				goto bad;
			}

			accounted += firmware->region[i].length;
			TRACE("  section %" B_PRIu32 ": %" B_PRIu32 " bytes for %#"
				B_PRIx32 "\n", i, firmware->region[i].length,
				firmware->region[i].address);
		}

		if (accounted != firmware->size) {
			ERROR("the patch's pieces come to %" B_PRIu64 " of %" B_PRIuSIZE
				" bytes\n", accounted, firmware->size);
			goto bad;
		}
	}

	return B_OK;

bad:
	free(firmware->data);
	firmware->data = NULL;
	return B_BAD_DATA;
}


/* The code the part runs. Written back to front: the trailer is the last
 * thing in the file, the regions it counts are immediately before it, and what
 * they describe starts at the beginning.
 */
status_t
mt7922_firmware_read_ram(mt7922_dev* device, mt7922_firmware* firmware)
{
	status_t status = load_file("WIFI_RAM_CODE_MT7922_1.bin", &firmware->data,
		&firmware->size);
	if (status != B_OK)
		return status;

	if (firmware->size < RAM_TRAILER_SIZE + RAM_REGION_SIZE) {
		ERROR("the RAM image is too small to be one\n");
		goto bad;
	}

	{
		const uint8* trailer = firmware->data + firmware->size
			- RAM_TRAILER_SIZE;

		firmware->count = trailer[2];
		uint8 chip = trailer[0];

		if (firmware->count == 0 || firmware->count > MAX_REGIONS
			|| RAM_TRAILER_SIZE
				+ (uint64)firmware->count * RAM_REGION_SIZE > firmware->size) {
			ERROR("the RAM image says it has %" B_PRIu32 " regions\n",
				firmware->count);
			goto bad;
		}

		TRACE("RAM image for chip %#x, %" B_PRIu32 " regions, built %.14s\n",
			chip, firmware->count, (const char*)trailer + 17);
	}

	{
		uint64 offset = 0;

		for (uint32 i = 0; i < firmware->count; i++) {
			const uint8* region = firmware->data + firmware->size
				- RAM_TRAILER_SIZE
				- (uint64)(firmware->count - i) * RAM_REGION_SIZE;

			firmware->region[i].address = read_le32(region + 16);
			firmware->region[i].length = read_le32(region + 20);
			firmware->region[i].features = region[24];
			firmware->region[i].keyIndex = 0;
			firmware->region[i].offset = offset;
			firmware->region[i].size = firmware->region[i].length;

			/* A region marked as not downloaded still takes up its place in
			 * the file, so what follows it is where it would have been.
			 */
			offset += firmware->region[i].length;

			if (offset > firmware->size) {
				ERROR("RAM region %" B_PRIu32 " runs past the end\n", i);
				goto bad;
			}

			TRACE("  region %" B_PRIu32 ": %" B_PRIu32 " bytes for %#" B_PRIx32
				"%s%s\n", i, firmware->region[i].length,
				firmware->region[i].address,
				(firmware->region[i].features & FW_FEATURE_ENCRYPTED) != 0
					? ", encrypted" : "",
				(firmware->region[i].features & FW_FEATURE_NOT_DOWNLOADED) != 0
					? ", not sent" : "");
		}
	}

	return B_OK;

bad:
	free(firmware->data);
	firmware->data = NULL;
	return B_BAD_DATA;
}


void
mt7922_firmware_free(mt7922_firmware* firmware)
{
	free(firmware->data);
	firmware->data = NULL;
	firmware->size = 0;
	firmware->count = 0;
}
