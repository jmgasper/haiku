#include <arch/arm64/gicv3_mbi_vectors.h>

#include <assert.h>
#include <stdio.h>
#include <random>
#include <vector>

using namespace Gicv3Mbi;

int
main()
{
	VectorPool pool;
	assert(pool.Find(0) == -1 && pool.Find(17) == -1 && pool.Find(UINT32_MAX) == -1);
	assert(!pool.Claim(UINT32_MAX, 1) && !pool.Claim(0, UINT32_MAX));
	assert(!pool.Release(0, 1) && !pool.Release(UINT32_MAX, 1));
	for (unsigned i = 0; i < 16; i++) {
		assert(pool.Find(1) == int(i));
		assert(pool.Claim(i, 1));
		assert(!pool.Claim(i, 1));
	}
	assert(pool.Find(1) == -1);
	assert(!pool.Release(0, 2));
	for (unsigned i = 0; i < 16; i++)
		assert(pool.Release(i, 1));
	assert(pool.Claim(0, 16) && pool.Find(1) == -1);
	assert(!pool.Release(0, 15) && !pool.Release(1, 15));
	assert(pool.Release(0, 16));
	assert(pool.Claim(0, 3) && pool.Find(3) == 4);
	assert(!pool.Claim(3, 3) && pool.Claim(3, 1));
	assert(pool.Release(3, 1) && pool.Release(0, 3));

	// Model fragmentation and reuse independently as 16 individual slots.
	std::mt19937 random(0x3588);
	bool used[16]{};
	struct Allocation { unsigned offset; unsigned count; };
	std::vector<Allocation> allocations;
	for (unsigned round = 0; round < 10000; round++) {
		if (!allocations.empty() && random() % 3 == 0) {
			unsigned index = random() % allocations.size();
			Allocation entry = allocations[index];
			assert(!pool.Release(entry.offset, entry.count + 1));
			assert(pool.Release(entry.offset, entry.count));
			assert(!pool.Release(entry.offset, entry.count));
			for (unsigned i = entry.offset; i < entry.offset + entry.count; i++)
				used[i] = false;
			allocations.erase(allocations.begin() + index);
		} else {
			unsigned count = random() % 16 + 1;
			int expected = -1;
			for (unsigned i = 0; i + count <= 16; i++) {
				// Base divisibility by every power of two up to the next
				// one covering the request is the MSI data-alignment rule.
				bool valid = true;
				for (unsigned n = 1; n < count; n *= 2)
					valid &= (kFirstVector + i) % (n * 2) == 0;
				for (unsigned j = 0; j < count; j++)
					valid &= !used[i + j];
				if (valid) { expected = i; break; }
			}
			assert(pool.Find(count) == expected);
			if (expected >= 0) {
				unsigned offset = expected;
				assert(pool.Claim(offset, count) && pool.Contains(offset, count));
				allocations.push_back({offset, count});
				for (unsigned i = offset; i < offset + count; i++)
					used[i] = true;
			}
		}
	}
	for (Allocation entry : allocations)
		assert(pool.Release(entry.offset, entry.count));
	assert(pool.Find(16) == 0);
	puts("ROCK5_MBI_VECTOR_POOL_PASS");
	return 0;
}
