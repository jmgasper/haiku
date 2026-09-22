#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

#include "firmware_profile.h"
#include "intx_profile.h"

using namespace RK3588Firmware;

int
main(int argc, char** argv)
{
	assert(argc == 9);
	const unsigned segments[] = {0, 1, 3, 4};
	const uint64_t roots[] = {0xa40000000ULL, 0xa40400000ULL,
		0xa40c00000ULL, 0xa41000000ULL};
	const uint64_t endpoints[] = {0x900100000ULL, 0x940100000ULL,
		0x9c0100000ULL, 0xa00100000ULL};
	const uint64_t windows[] = {0xf0000000, 0xf1000000, 0xf3000000, 0xf4000000};
	for (unsigned i = 0; i < 4; i++) {
		const PortProfile* port = FindPort(roots[i]);
		assert(port && port->segment == segments[i]);
		assert(port->endpointConfig == endpoints[i] && port->memoryBase == windows[i]);
		assert(ProfileAllowsPort("rock5-itx-edk2-v1.1-dt-onboard", *port));
		assert(ProfileAllowsPort("rock5-itx-edk2-v1.1-dt-samsung950", *port) == (i == 0));
		assert(ProfileAllowsPort("rock5-itx-edk2-v1.1-dt-nvme", *port) == (i == 0));
		assert(!ProfileAllowsPort("", *port) && !ProfileAllowsPort("unknown", *port));
		uint32_t root[64], endpoint[64];
		for (unsigned function = 0; function < 2; function++) {
			FILE* file = fopen(argv[1 + 2 * i + function], "rb");
			assert(file);
			assert(fread(function ? endpoint : root, 1, 256, file) == 256);
			assert(fgetc(file) == EOF);
			fclose(file);
		}
		uint64_t base, size;
		assert(RootMatches(root, base, size, *port));
		assert(base == windows[i] && size == 0x100000);
		assert(EndpointMatches(endpoint, base, size, *port));
		assert(!EndpointMatches(endpoint, base + 0x100000, size, *port));
		uint32_t copy[64];
		for (unsigned index : {0u, 2u, 3u}) {
			memcpy(copy, endpoint, sizeof(copy));
			copy[index] = 0xffffffff;
			assert(!EndpointMatches(copy, base, size, *port));
		}
		memcpy(copy, endpoint, sizeof(copy));
		copy[1] &= ~2u;
		assert(!EndpointMatches(copy, base, size, *port));
		if (i != 0) {
			memcpy(copy, endpoint, sizeof(copy));
			copy[2] ^= 1;
			assert(!EndpointMatches(copy, base, size, *port));
		}
		const unsigned first = i < 2 ? 0 : 2;
		const unsigned second = i == 1 ? 5 : 4;
		for (unsigned bar : {first, second}) {
			if (i == 0 && bar == second)
				continue;
			memcpy(copy, endpoint, sizeof(copy));
			copy[4 + bar] = uint32_t(base + size) | (copy[4 + bar] & 0xf);
			assert(!EndpointMatches(copy, base, size, *port));
			memcpy(copy, endpoint, sizeof(copy));
			copy[4 + bar] |= 1;
			assert(!EndpointMatches(copy, base, size, *port));
			if (i != 1) {
				memcpy(copy, endpoint, sizeof(copy));
				copy[5 + bar] = 1;
				assert(!EndpointMatches(copy, base, size, *port));
			}
		}
		if (i != 0) {
			memcpy(copy, endpoint, sizeof(copy));
			copy[4 + second] = copy[4 + first];
			assert(!EndpointMatches(copy, base, size, *port));
		}
		memcpy(copy, root, sizeof(copy));
		copy[8] = uint32_t(base + 0x1000000) | uint32_t((base + 0x1000000) >> 16);
		assert(!RootMatches(copy, base, size, *port));
		assert(!MemoryBarMatches(endpoint, 6, 0, 4096, base, size));
		assert(!MemoryBarMatches(endpoint, 5, 4, 4096, base, size));
		assert(!MemoryBarMatches(endpoint, first, 0, 0, base, size));
		assert(!MemoryBarMatches(endpoint, first, 0, 3, base, size));
	}
	assert(!FindPort(0xa40800000ULL) && !FindPort(0xa40001000ULL));
	for (unsigned segment = 0; segment < 8; segment++) {
		IntxProfile profile;
		bool supported = FindIntxProfile(segment, profile);
		assert(supported == (segment == 1 || segment == 3 || segment == 4));
		if (!supported)
			continue;
		uint32_t specifier[] = {0, segment == 1 ? 255u : segment == 3 ? 245u : 250u, 4, 0};
		uint64_t apb = segment == 1 ? 0xfe160000 : segment == 3 ? 0xfe180000 : 0xfe190000;
		uint32_t irq = segment == 1 ? 287 : segment == 3 ? 277 : 282;
		assert(irq > UINT8_MAX);
		assert(IntxResourcesMatch(profile, apb, 0x10000, specifier, 4, irq, 0xfe600000));
		assert(!IntxResourcesMatch(profile, apb, 0x10000, nullptr, 4, irq, 0xfe600000));
		assert(!IntxResourcesMatch(profile, apb, 0x10000, specifier, 3, irq, 0xfe600000));
		assert(!IntxResourcesMatch(profile, apb + 0x10000, 0x10000, specifier, 4,
			irq, 0xfe600000));
		assert(!IntxResourcesMatch(profile, apb, 0x10000, specifier, 4,
			(uint8_t)irq, 0xfe600000));
		assert(!IntxResourcesMatch(profile, apb, 0x10000, specifier, 4, irq, 0xfe640000));
		assert(!IntxResourcesMatch(profile, apb, 0x1000, specifier, 4, irq, 0xfe600000));
		for (unsigned cell = 0; cell < 4; cell++) {
			specifier[cell] ^= 1;
			assert(!IntxResourcesMatch(profile, apb, 0x10000, specifier, 4, irq, 0xfe600000));
			specifier[cell] ^= 1;
		}
	}
	for (unsigned bus = 0; bus < 4; bus++) {
		for (unsigned device = 0; device < 32; device++) {
			for (unsigned function = 0; function < 8; function++) {
				for (unsigned pin = 0; pin <= 5; pin++) {
					assert(ValidIntxEndpoint(bus, device, function, pin)
						== (bus == 1 && device == 0 && function == 0 && pin == 1));
				}
			}
		}
	}
	puts("ROCK5_PCIE_ONBOARD_PROFILE_PASS functions=8 layouts=pass rejection=pass");
}
