#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "cache_line_size.h"


int
main()
{
	const struct {
		uint64_t ctr;
		uint64_t instructionBytes;
		uint64_t dataBytes;
	} cases[] = {
		{UINT64_C(0x8444c004), 64, 64},
		{UINT64_C(0x8444c003), 32, 64},
		{UINT64_C(0x8445c004), 64, 128},
		{UINT64_C(0xf444c004), 64, 64},
		{UINT64_C(0xffffffffffffffff), 131072, 131072},
		{UINT64_C(0xfffffff0fffffff0), 4, 131072},
		{UINT64_C(0), 4, 4}
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		assert(arm64_instruction_cache_line_size(cases[i].ctr)
			== cases[i].instructionBytes);
		assert(arm64_data_cache_line_size(cases[i].ctr) == cases[i].dataBytes);
	}
	puts("ARM64 cache line decoding checks passed");
	return 0;
}
