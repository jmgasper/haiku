// SPDX-License-Identifier: MIT
#ifndef ROCK5_PCI_CONFIG_PROBE_CHECKS_H
#define ROCK5_PCI_CONFIG_PROBE_CHECKS_H

#include <stdint.h>

// EDK2 v1.1's observed single-endpoint root layout. Do not issue a
// downstream configuration transaction while the root link is inactive.
inline bool
Rock5RootLinkReady(const uint32_t* words)
{
	if (words[0] != 0x35881d87 || words[2] >> 8 != 0x060400
		|| ((words[3] >> 16) & 0xff) != 1
		|| (words[6] & 0x00ffffff) != 0x00010100
		|| (words[1] & 2) == 0 || (words[0x70 / 4] & 0xff) != 0x10) {
		return false;
	}
	uint32_t link = words[0x80 / 4] >> 16;
	return (link & 0x2000) != 0 && (link & 0x0800) == 0
		&& (link & 0x000f) != 0 && (link & 0x03f0) != 0;
}

// Read-only MSI inspection is limited to this 16 KiB Samsung BAR layout.
inline bool
Rock5SamsungMsixLayoutMatches(const uint32_t* words)
{
	return words[0] == 0xa802144d && words[2] == 0x01080201
		&& words[0x10 / 4] == 0xf0000004 && words[0x14 / 4] == 0
		&& (words[1] & 2) != 0
		&& (words[0xb0 / 4] & 0x07ffffff) == 0x00080011
		&& words[0xb4 / 4] == 0x3000 && words[0xb8 / 4] == 0x2000;
}

#endif
