/*
 * Check instruction replacement and cache synchronization on each ARM64 CPU.
 * Distributed under the terms of the MIT License.
 */
#include <OS.h>
#include <image.h>
#include <syscalls.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


#if defined(__aarch64__)
static status_t
PinCPU(uint32 cpu)
{
	uint32 mask[32] = {};
	mask[cpu / 32] = 1U << (cpu % 32);
	status_t status = _kern_set_thread_affinity(0, mask, sizeof(mask));
	if (status != B_OK)
		return status;
	bigtime_t deadline = system_time() + 1000000;
	while (_kern_get_cpu() != (int)cpu) {
		if (system_time() >= deadline)
			return B_TIMED_OUT;
		snooze(50);
	}
	return B_OK;
}


static bool
CheckAliases(uint32 cpus, unsigned rounds)
{
	void* writer = NULL;
	area_id source = create_area("instruction writer", &writer, B_ANY_ADDRESS,
		B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (source < B_OK) {
		fprintf(stderr, "Cannot create instruction writer: %s\n", strerror(source));
		return false;
	}
	const unsigned aliasCount = 8, slots = 63;
	// Reserve consecutive virtual pages so both values of the first cache
	// index bit above the page offset are exercised on every run. Independent
	// B_ANY_ADDRESS clones can all receive the same index by chance.
	addr_t aliasBase = 0;
	const size_t aliasBytes = aliasCount * B_PAGE_SIZE;
	status_t reserved = _kern_reserve_address_range(&aliasBase,
		B_ANY_ADDRESS, aliasBytes);
	if (reserved != B_OK) {
		fprintf(stderr, "Cannot reserve instruction aliases: %s\n", strerror(reserved));
		delete_area(source);
		return false;
	}
	printf("ROCK5_CACHE_ALIAS_RESERVED base=%p bytes=%zu\n", (void*)aliasBase, aliasBytes);
	void* aliases[aliasCount] = {};
	area_id areas[aliasCount];
	unsigned created = 0;
	bool passed = true;
	bool differentIndex = false;
	uint64 checked = 0, mismatches = 0;
	for (; created < aliasCount; ++created) {
		aliases[created] = (void*)(aliasBase + created * B_PAGE_SIZE);
		areas[created] = clone_area("instruction alias", &aliases[created], B_EXACT_ADDRESS,
			B_READ_AREA | B_EXECUTE_AREA, source);
		if (areas[created] < B_OK) {
			fprintf(stderr, "Cannot clone instruction alias: %s\n", strerror(areas[created]));
			passed = false;
			break;
		}
		differentIndex |= (((addr_t)aliases[created] ^ (addr_t)writer) & B_PAGE_SIZE) != 0;
	}
	passed &= differentIndex;
	printf("ROCK5_CACHE_ALIAS_BEGIN cpus=%" B_PRIu32 " rounds=%u aliases=%u slots=%u"
		" different_index=%u writer=%p\n", cpus, rounds, created, slots, differentIndex, writer);
	for (unsigned i = 0; i < created; ++i)
		printf("ROCK5_CACHE_ALIAS_MAPPING index=%u address=%p\n", i, aliases[i]);
	fflush(stdout);
	bigtime_t start = system_time();
	for (unsigned round = 0; round < rounds && passed; ++round) {
		// Alternate writer cores so a PIPT core must also invalidate the VIPT
		// cores' aliases. No executable mapping is recreated between rounds.
		if (PinCPU(round % cpus) != B_OK) {
			passed = false;
			break;
		}
		for (unsigned slot = 0; slot < slots; ++slot) {
			uint32 value = (round * 257 + slot + 1) & 0xffff;
			uint32 instructions[] = {0x52800000 | (value << 5), 0xd65f03c0};
			memcpy((uint8*)writer + 60 + slot * 64, instructions, sizeof(instructions));
		}
		// Flush only the writable alias, just as the translation map uses its
		// physical alias while publishing an executable mapping elsewhere.
		clear_caches((uint8*)writer + 60, (slots - 1) * 64 + 8, B_INVALIDATE_ICACHE);
		for (uint32 cpu = 0; cpu < cpus && passed; ++cpu) {
			if (PinCPU(cpu) != B_OK) {
				passed = false;
				break;
			}
			asm volatile("isb" : : : "memory");
			for (unsigned alias = 0; alias < created && passed; ++alias) {
				for (unsigned slot = 0; slot < slots; ++slot) {
					typedef uint32 (*Function)();
					Function function = (Function)((uint8*)aliases[alias] + 60 + slot * 64);
					uint32 actual = function();
					uint32 expected = (round * 257 + slot + 1) & 0xffff;
					checked++;
					if (actual != expected) {
						fprintf(stderr, "CACHE_ALIAS_MISMATCH cpu=%" B_PRIu32
							" round=%u alias=%u slot=%u actual=%" B_PRIu32 " expected=%" B_PRIu32 "\n",
							cpu, round, alias, slot, actual, expected);
						mismatches++;
						passed = false;
						break;
					}
				}
			}
		}
	}
	for (unsigned i = 0; i < created; ++i)
		if (delete_area(areas[i]) != B_OK) passed = false;
	if (_kern_unreserve_address_range(aliasBase, aliasBytes) != B_OK) passed = false;
	if (delete_area(source) != B_OK) passed = false;
	passed &= checked == uint64(cpus) * rounds * aliasCount * slots;
	printf("ROCK5_CACHE_ALIAS_%s checked=%" B_PRIu64 " mismatches=%" B_PRIu64
		" elapsed_us=%" B_PRIdBIGTIME "\n", passed ? "PASS" : "FAIL", checked,
		mismatches, system_time() - start);
	return passed;
}
#endif


int
main(int argc, char** argv)
{
#if !defined(__aarch64__)
	fprintf(stderr, "This probe requires ARM64.\n");
	return 77;
#else
	unsigned rounds = argc == 2 ? atoi(argv[1]) : 32;
	if (argc > 2 || rounds < 1 || rounds > 256) {
		fprintf(stderr, "Usage: %s [rounds (1..256)]\n", argv[0]);
		return 2;
	}
	system_info info;
	if (get_system_info(&info) != B_OK || info.cpu_count < 1 || info.cpu_count > 64)
		return 1;
	alarm(60);
	const size_t mappingSize = 2 * B_PAGE_SIZE;
	uint8* mapping = (uint8*)mmap(NULL, mappingSize, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap executable memory");
		return 1;
	}

	// Each eight-byte function crosses a 64-byte line boundary. The last
	// function also crosses a page boundary; the synchronized range is unaligned.
	const unsigned slots = B_PAGE_SIZE / 64;
	uint8* code = mapping + 60;
	const size_t codeLength = (slots - 1) * 64 + 8;
	uint64 checked = 0;
	uint64 mismatches = 0;
	bool passed = true;
	printf("ROCK5_CACHE_BEGIN cpus=%" B_PRIu32 " rounds=%u slots=%u\n",
		info.cpu_count, rounds, slots);
	fflush(stdout);
	bigtime_t start = system_time();
	for (unsigned round = 0; round < rounds && passed; round++) {
		if (PinCPU(0) != B_OK) {
			fprintf(stderr, "Cannot pin the code-writing CPU\n");
			passed = false;
			break;
		}
		for (unsigned slot = 0; slot < slots; slot++) {
			uint32 value = (round * 257 + slot + 1) & 0xffff;
			uint32 instructions[] = {0x52800000 | (value << 5), 0xd65f03c0};
				// MOVZ W0, value; RET
			memcpy(code + slot * 64, instructions, sizeof(instructions));
		}
		clear_caches(code, codeLength, B_INVALIDATE_ICACHE);
		for (uint32 cpu = 0; cpu < info.cpu_count && passed; cpu++) {
			if (PinCPU(cpu) != B_OK) {
				fprintf(stderr, "Cannot pin executing CPU %" B_PRIu32 "\n", cpu);
				passed = false;
				break;
			}
			// The cache operations are broadcast, but each executing CPU must
			// synchronize its own instruction pipeline before entering changed code.
			asm volatile("isb" : : : "memory");
			for (unsigned slot = 0; slot < slots; slot++) {
				typedef uint32 (*Function)();
				Function function = (Function)(code + slot * 64);
				uint32 actual = function();
				uint32 expected = (round * 257 + slot + 1) & 0xffff;
				checked++;
				if (actual != expected) {
					fprintf(stderr, "CACHE_MISMATCH cpu=%" B_PRIu32 " round=%u slot=%u"
						" actual=%" B_PRIu32 " expected=%" B_PRIu32 "\n",
						cpu, round, slot, actual, expected);
					mismatches++;
					passed = false;
					break;
				}
			}
		}
	}
	munmap(mapping, mappingSize);
	printf("ROCK5_CACHE_%s checked=%" B_PRIu64 " mismatches=%" B_PRIu64
		" elapsed_us=%" B_PRIdBIGTIME "\n", passed ? "PASS" : "FAIL", checked,
		mismatches, system_time() - start);
	return passed && CheckAliases(info.cpu_count, rounds) ? 0 : 1;
#endif
}
