/*
 * Memory the MT7922 can reach by itself.
 *
 * The card fetches its own descriptors and moves its own payloads, so the
 * memory it works from has to satisfy it rather than us: contiguous, because a
 * ring is one array as far as the hardware is concerned, and below four
 * gigabytes, because this part addresses no higher. Neither is the default for
 * an ordinary allocation, and neither failure announces itself - a ring placed
 * where the card cannot reach simply never advances.
 *
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

#include <util/AutoLock.h>
#include <vm/vm.h>

#include "mt7922.h"


#define TRACE(x...)	dprintf("mt7922: " x)
#define ERROR(x...)	dprintf("mt7922: " x)


/* Take a block of memory the card can fetch from directly, and say where it
 * is from the card's point of view as well as ours.
 */
status_t
mt7922_dma_alloc(const char* name, size_t size, mt7922_dma_mem* memory)
{
	size = ROUNDUP(size, B_PAGE_SIZE);

	memory->size = size;
	memory->area = create_area(name, &memory->address, B_ANY_KERNEL_ADDRESS,
		size, B_32_BIT_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);

	if (memory->area < B_OK) {
		ERROR("cannot find %" B_PRIuSIZE " bytes the card can reach: %s\n",
			size, strerror(memory->area));
		return memory->area;
	}

	physical_entry entry;
	status_t status = get_memory_map(memory->address, size, &entry, 1);
	if (status != B_OK) {
		ERROR("cannot find where %s ended up: %s\n", name, strerror(status));
		delete_area(memory->area);
		memory->area = -1;
		return status;
	}

	/* Contiguous was asked for, so one entry should cover all of it. If it
	 * does not, the card would walk off the end of the first piece.
	 */
	if (entry.size < size) {
		ERROR("%s came back in pieces: %" B_PRIuSIZE " of %" B_PRIuSIZE "\n",
			name, entry.size, size);
		delete_area(memory->area);
		memory->area = -1;
		return B_NO_MEMORY;
	}

	memory->physical = entry.address;

	/* The card reaches no higher than four gigabytes, and an address above
	 * that would be quietly truncated into someone else's memory.
	 */
	if ((memory->physical + size) > 0x100000000ULL) {
		ERROR("%s is at %#" B_PRIxPHYSADDR ", which the card cannot reach\n",
			name, memory->physical);
		delete_area(memory->area);
		memory->area = -1;
		return B_BAD_VALUE;
	}

	memset(memory->address, 0, size);
	return B_OK;
}


void
mt7922_dma_free(mt7922_dma_mem* memory)
{
	if (memory->area >= B_OK)
		delete_area(memory->area);

	memory->area = -1;
	memory->address = NULL;
	memory->physical = 0;
	memory->size = 0;
}
