#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

#include "firmware_profile.h"

using namespace RK3588Firmware;

int
main(int argc, char** argv)
{
	assert(argc == 3);
	uint32_t root[64], endpoint[64];
	for (int i = 1; i <= 2; i++) {
		FILE* file = fopen(argv[i], "rb");
		assert(file != NULL);
		assert(fread(i == 1 ? root : endpoint, 1, 256, file) == 256);
		assert(fgetc(file) == EOF);
		fclose(file);
	}
	uint64_t base, size;
	assert(RootMatches(root, base, size));
	assert((base == 0xf0000000 || base == 0xf0200000) && size == 0x100000);
	assert(EndpointMatches(endpoint, base, size));
	for (unsigned bus = 0; bus < 256; bus++) {
		for (unsigned device = 0; device < 32; device++) {
			for (unsigned function = 0; function < 8; function++) {
				assert(ValidAccess(bus, device, function, 0, 4)
					== (bus <= 1 && device == 0 && function == 0));
			}
		}
	}
	assert(ValidAccess(1, 0, 0, 4095, 1));
	assert(ValidAccess(1, 0, 0, 4094, 2));
	assert(ValidAccess(1, 0, 0, 4092, 4));
	const unsigned badOffsets[] = {1, 2, 3, 4094, 4095, 4096, 65535, UINT32_MAX};
	for (unsigned offset : badOffsets)
		assert(!ValidAccess(1, 0, 0, offset, 4));
	for (unsigned width : {0u, 3u, 8u, UINT32_MAX})
		assert(!ValidAccess(1, 0, 0, 0, width));
	// Reject wrong identities, classes, multifunction headers and bus layouts.
	const unsigned rootFields[] = {0, 2, 3, 6, 8, 0x70 / 4, 0x80 / 4};
	for (unsigned field : rootFields) {
		uint32_t copy[64];
		memcpy(copy, root, sizeof(copy));
		copy[field] = 0xffffffff;
		assert(!RootMatches(copy, base, size));
	}
	for (uint32_t link : {0x10230000u, 0x38230000u, 0x30200000u, 0x30030000u}) {
		uint32_t copy[64];
		memcpy(copy, root, sizeof(copy));
		copy[0x80 / 4] = link;
		assert(!RootMatches(copy, base, size));
	}
	assert(RootMatches(root, base, size));
	root[1] &= ~2u;
	assert(!RootMatches(root, base, size));
	const unsigned endpointFields[] = {0, 2, 3, 4, 5};
	for (unsigned field : endpointFields) {
		uint32_t copy[64];
		memcpy(copy, endpoint, sizeof(copy));
		copy[field] = 0xffffffff;
		assert(!EndpointMatches(copy, base, size));
	}
	assert(!EndpointMatches(endpoint, base + 0x1000, size));
	assert(!EndpointMatches(endpoint, base, 0x3fff));
	endpoint[1] &= ~2u;
	assert(!EndpointMatches(endpoint, base, size));
	puts("ROCK5_PCIE_PROFILE_PASS captured-firmware=2 config-boundaries=pass");
	return 0;
}
