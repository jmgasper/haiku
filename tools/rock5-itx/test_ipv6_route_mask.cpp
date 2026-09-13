#include <arpa/inet.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using uint8 = uint8_t;
using int32 = int32_t;

#include "ipv6_route_mask_body.inc"

int
main()
{
	assert(ipv6_first_mask_bit(nullptr) == 0); // An implicit host route.
	sockaddr_in6 mask = {};
	mask.sin6_family = AF_INET6;
	for (int prefix = 0; prefix <= 128; prefix++) {
		memset(&mask.sin6_addr, 0, sizeof(mask.sin6_addr));
		for (int bit = 0; bit < prefix; bit++)
			mask.sin6_addr.s6_addr[bit / 8] |= 0x80 >> (bit % 8);
		// The address-module contract counts zero host bits from the LSB.
		assert(ipv6_first_mask_bit(reinterpret_cast<sockaddr*>(&mask))
			== 128 - prefix);
	}
	for (int least = 0; least < 128; least++) {
		memset(&mask.sin6_addr, 0, sizeof(mask.sin6_addr));
		mask.sin6_addr.s6_addr[15 - least / 8] = 1 << (least % 8);
		assert(ipv6_first_mask_bit(reinterpret_cast<sockaddr*>(&mask)) == least);
		// Even for a noncontiguous mask, higher bits cannot change its LSB.
		mask.sin6_addr.s6_addr[0] |= 0x80;
		assert(ipv6_first_mask_bit(reinterpret_cast<sockaddr*>(&mask)) == least);
	}
	puts("IPv6 mask ranking: 129 prefixes and all 128 bit positions passed");
}
