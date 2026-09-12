#include <arch/arm64/gicv3_its_commands.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace Gicv3Its;

static void Check(Command command, uint64_t a, uint64_t b, uint64_t c)
{
	const uint64_t expected[] = {a, b, c, 0};
	assert(memcmp(command.words, expected, sizeof(expected)) == 0);
}

int main()
{
	// Independent packets laid out from IHI 0069G's command bit diagrams.
	Check(MapDevice(0x100, 0x100001000, 5, true),
		0x0000010000000008, 4, 0x8000000100001000);
	Check(MapDevice(0x100, 0x100001000, 5, false),
		0x0000010000000008, 4, 0x0000000100001000);
	Check(MapCollection(0x1234, 0x123456780000, true),
		9, 0, 0x8000123456781234);
	Check(MapInterrupt(0x100, 31, 8223, 0x321),
		0x000001000000000a, 0x0000201f0000001f, 0x321);
	Check(EventCommand(0x0c, 0x100, 31), 0x000001000000000c, 31, 0);
	Check(EventCommand(0x04, 0x100, 31), 0x0000010000000004, 31, 0);
	Check(EventCommand(0x0f, 0x100, 31), 0x000001000000000f, 31, 0);
	Check(InvalidateAll(3), 0x0d, 0, 3);
	Check(Sync(0x123456780000), 5, 0, 0x123456780000);
	assert(!ValidCount(0) && !ValidCount(33) && !ValidCount(UINT32_MAX));
	for (unsigned i = 1; i <= 32; i++) assert(ValidCount(i));
	assert(ValidTableRange(0x100000000, 0x80000, 0x10000));
	assert(ValidTableRange(0x7ffff0000, 0x10000, 0x10000));
	assert(!ValidTableRange(0x7ffff0000, 0x10001, 0x10000));
	assert(!ValidTableRange(0x800000000, 1, 1));
	assert(!ValidTableRange(UINT64_MAX, 1, 1));
	assert(!ValidTableRange(0, UINT64_MAX, 1));
	assert(!ValidTableRange(1, 4096, 4096));
	assert(!ValidTableRange(0, 0, 4096));
	assert(!ValidTableRange(0, 4096, 0));
	assert(!ValidTableRange(0, 4096, 3));
	unsigned visits[2048]{};
	unsigned offset = 0;
	for (unsigned i = 0; i < 6144; i++) {
		assert(offset < 65536 && offset % 32 == 0);
		visits[offset / 32]++;
		offset = NextCommand(offset);
	}
	assert(offset == 0);
	for (unsigned n : visits) assert(n == 3);
	assert(kFirstLpi == 8192 && kFirstLpi + kEventCount - 1 == 8223);
	puts("ROCK5_ITS_COMMANDS_PASS");
}
