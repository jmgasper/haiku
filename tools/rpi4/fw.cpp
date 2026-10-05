/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_fw <tag> <response bytes> [request words...]
// Sends one property tag to the Raspberry Pi's firmware through the lab
// device /dev/misc/rpi_property and prints the response words.
// "b:<n>" as a word stands for a single byte value, "h:<n>" for 16 bits;
// they are packed in order (little endian) the way a C struct would be.


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>
#include <SupportDefs.h>


#define RPI_PROPERTY_REQUEST	(B_DEVICE_OP_CODES_END + 1)

struct rpi_property_request {
	uint32	tag;
	uint32	size;
	uint32	data[256];
};


int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <tag> <bytes> [words...]\n", argv[0]);
		return 1;
	}

	rpi_property_request request = {};
	request.tag = strtoul(argv[1], NULL, 0);
	request.size = (strtoul(argv[2], NULL, 0) + 3) & ~3;

	uint8* out = (uint8*)request.data;
	size_t offset = 0;
	for (int i = 3; i < argc && offset + 4 <= sizeof(request.data); i++) {
		if (strncmp(argv[i], "b:", 2) == 0) {
			out[offset++] = strtol(argv[i] + 2, NULL, 0);
		} else if (strncmp(argv[i], "h:", 2) == 0) {
			uint16 value = strtol(argv[i] + 2, NULL, 0);
			memcpy(out + offset, &value, 2);
			offset += 2;
		} else {
			uint32 value = strtoul(argv[i], NULL, 0);
			memcpy(out + offset, &value, 4);
			offset += 4;
		}
	}
	if (offset > request.size)
		request.size = (offset + 3) & ~3;

	int device = open("/dev/misc/rpi_property", O_RDWR);
	if (device < 0) {
		fprintf(stderr, "/dev/misc/rpi_property: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(device, RPI_PROPERTY_REQUEST, &request, sizeof(request)) != 0) {
		fprintf(stderr, "tag %#x: %s\n", (unsigned)request.tag,
			strerror(errno));
		return 1;
	}

	for (uint32 i = 0; i < request.size / 4; i++)
		printf("%s%08x", i % 8 == 0 ? (i == 0 ? "" : "\n") : " ",
			(unsigned)request.data[i]);
	printf("\n");
	return 0;
}
