#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "interrupt_specifier.h"

int
main()
{
	// ROCK 5 ITX EDK2: host1 EHCI is SPI 218, level high, no PPI partition.
	const uint8_t ehci[] = {0,0,0,0, 0,0,0,218, 0,0,0,4, 0,0,0,0};
	uint32_t irq = 9999;
	assert(fdt_decode_interrupt(ehci, sizeof(ehci), 4, 0, irq) && irq == 250);
	assert(!fdt_decode_interrupt(ehci, sizeof(ehci), 4, 1, irq));
	assert(!fdt_decode_interrupt(ehci, sizeof(ehci), 4, UINT32_MAX, irq));
	assert(!fdt_decode_interrupt(ehci, sizeof(ehci) - 1, 4, 0, irq));
	assert(!fdt_decode_interrupt(ehci, sizeof(ehci), 0, 0, irq));
	assert(!fdt_decode_interrupt(ehci, sizeof(ehci), UINT32_MAX, 0, irq));
	assert(!fdt_decode_interrupt(NULL, 16, 4, 0, irq));
	uint8_t unaligned[17];
	memcpy(unaligned + 1, ehci, 16);
	assert(fdt_decode_interrupt(unaligned + 1, 16, 4, 0, irq) && irq == 250);
	uint8_t affinity[16];
	memcpy(affinity, ehci, 16);
	affinity[15] = 1;
	assert(!fdt_decode_interrupt(affinity, 16, 4, 0, irq));
	// Two three-cell GIC timer PPIs; selecting the second must use its own ID.
	const uint8_t timers[] = {
		0,0,0,1, 0,0,0,13, 0,0,0,4,
		0,0,0,1, 0,0,0,10, 0,0,0,4
	};
	assert(fdt_decode_interrupt(timers, sizeof(timers), 3, 0, irq) && irq == 29);
	assert(fdt_decode_interrupt(timers, sizeof(timers), 3, 1, irq) && irq == 26);
	const uint8_t invalidPPI[] = {0,0,0,1, 0,0,0,16, 0,0,0,4};
	assert(!fdt_decode_interrupt(invalidPPI, 12, 3, 0, irq));
	const uint8_t extendedSPI[] = {0,0,0,2, 0,0,0,0, 0,0,0,4};
	assert(!fdt_decode_interrupt(extendedSPI, 12, 3, 0, irq));
	const uint8_t simple[] = {0,0,0,9, 0,0,0,11};
	assert(fdt_decode_interrupt(simple, 8, 1, 1, irq) && irq == 11);
	assert(fdt_decode_interrupt(simple, 8, 2, 0, irq) && irq == 9);
	return 0;
}
