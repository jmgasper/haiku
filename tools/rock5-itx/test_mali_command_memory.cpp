/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfCommandMemory.h"
#define main MaliMemoryFixtureMain
#include "test_mali_memory.cpp"
#undef main

static void workspaces()
{
	auto bytes = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	FirmwareMemory memory;
	FirmwareWorkspace extra[] = {{0x4010000, 8192, false}, {0x4200000, 8192, true}};
	assert(!memory.Plan(image, NULL, 1));
	assert(!memory.Plan(image, extra, 4));
	for (auto damaged : {FirmwareWorkspace{0x4000000, 4096, false},
		FirmwareWorkspace{0x4010001, 4096, false}, FirmwareWorkspace{0x4010000, 0, false},
		FirmwareWorkspace{0x4010000, 8191, false}, FirmwareWorkspace{0x3fff000, 8192, true},
		FirmwareWorkspace{0x7fff000, 8192, true}, FirmwareWorkspace{0xfffff000, 8192, true},
		FirmwareWorkspace{0x4010000, 0xfffff000, true}}) {
		assert(!memory.Plan(image, &damaged, 1));
		assert(memory.RequiredBytes() == 0 && memory.WorkspaceData(0) == NULL);
	}
	assert(memory.Plan(image, extra, 2));
	Guarded arena(memory.RequiredBytes());
	assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
	assert(memory.WorkspaceData(2) == NULL && memory.WorkspaceData(~0u) == NULL);
	std::map<uint64_t, uint64_t> mappings;
	std::set<uint64_t> tables;
	decode(arena.data, memory.RootPhysical(), memory.TablePages() * 4096,
		memory.RootPhysical(), 0, 0, tables, mappings);
	assert(mappings.size() == 21 && tables.size() == memory.TablePages());
	for (unsigned r = 0; r < 2; r++) {
		for (unsigned off = 0; off < 8192; off += 4096) {
			uint64_t d = mappings.at(extra[r].address + off);
			assert(((d >> 6) & 3) == 1 && ((d >> 53) & 3) == 3);
			assert(((d >> 2) & 7) == r && ((d >> 8) & 3) == (r ? 3 : 2));
			assert((d & UINT64_C(0xfffffff000)) == memory.RootPhysical()
				+ ((uint8_t*)memory.WorkspaceData(r) - arena.data) + off);
		}
		for (unsigned n = 0; n < 8192; n++)
			assert(((uint8_t*)memory.WorkspaceData(r))[n] == 0);
	}
	extra[1] = {0x4011000, 4096, true};
	assert(!memory.Plan(image, extra, 2));
	assert(memory.RequiredBytes() == 0 && memory.SharedData() == NULL);
}

// Entire Mesa 25.3.6 packer output, from the independently decoded and native
// Linux-qualified reference. Keeping this fixture literal catches encoder
// mutations and accidental changes to addresses, wait masks or cache modes.
static const uint64_t golden[2][32] = {
	{0x0300000000ff0000,0x1700000000000002,0x0100000100002000,0x020200006d610000,
	0x1502000000010004,0x0300000000010000,0x0202000069620107,0x150200000001007c,
	0x0300000000010000,0x020200006567020e,0x15020000000101fc,0x0300000000010000,
	0x0202000061680315,0x15020000000107fc,0x0300000000010000,0x020200007d6d041c,
	0x1502000000010ffc,0x0300000000010000,0x02020000796e0523,0x1502000000011000,
	0x0300000000010000,0x020200007573062a,0x15020000000117fc,0x0300000000010000,
	0x0202000071740731,0x1502000000011ff8,0x0300000000010000,0x0204000000000000,
	0x2400040000000211,0x0300000000010000,0,0},
	{0x0300000000ff0000,0x1700000000000002,0x0100000100002000,0x020200007a403579,
	0x1502000000010004,0x0300000000010000,0x020200007e43347e,0x150200000001007c,
	0x0300000000010000,0x0202000072463777,0x15020000000101fc,0x0300000000010000,
	0x020200007649366c,0x15020000000107fc,0x0300000000010000,0x020200006a4c3165,
	0x1502000000010ffc,0x0300000000010000,0x020200006e4f305a,0x1502000000011000,
	0x0300000000010000,0x0202000062523353,0x15020000000117fc,0x0300000000010000,
	0x0202000066553248,0x1502000000011ff8,0x0300000000010000,0x0204000000000000,
	0x2400040000000211,0x0300000000010000,0,0}
};

static void commands()
{
	auto bytes = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	CommandMemory memory;
	assert(memory.Plan(image));
	Guarded arena(memory.RequiredBytes());
	for (uint64_t physical : {UINT64_C(0x1000), UINT64_C(0x182300000),
		(UINT64_C(1) << 40) - memory.RequiredBytes()}) {
		assert(memory.Build(arena.data, arena.size, physical));
		uint64_t base = memory.RootPhysical();
		const uint8_t* user = arena.data + base - physical;
		std::map<uint64_t, uint64_t> mappings;
		std::set<uint64_t> tables;
		decode(user, base, 6 * 4096, base, 0, 0, tables, mappings, UINT64_C(1) << 48);
		assert(tables.size() == 6 && mappings.size() == 20);
		const uint64_t addresses[] = {0x100000000, 0x100002000, 0x100400000, 0x100800000};
		const unsigned sizes[] = {4096, 8192, 65536, 4096};
		unsigned payload = 6 * 4096;
		for (unsigned r = 0; r < 4; r++) {
			for (unsigned p = 0; p < sizes[r]; p += 4096) {
				uint64_t d = mappings.at(addresses[r] + p);
				uint64_t attrs = (r == 0 ? 0 : UINT64_C(3) << 53) | 0x443
					| (r == 0 ? 0x80 : 0) | (r < 2 ? 0x304 : 0x200);
				assert(d == ((base + payload + p) | attrs));
				mappings.erase(addresses[r] + p);
			}
			payload += sizes[r];
		}
		assert(mappings.empty() && user + payload == arena.data + arena.size);
		assert(memcmp(memory.Code(), golden, sizeof(golden)) == 0);
		for (unsigned n = 512; n < 4096; n++) assert(((uint8_t*)memory.Code())[n] == 0);
		for (unsigned n = 256; n < 65536; n++) assert(((uint8_t*)memory.Ring())[n] == 0);
		for (unsigned n = 0; n < 4096; n++) assert(((uint8_t*)memory.Completion())[n] == 0);
		for (unsigned round = 0; round < 2; round++) {
			assert(memory.PrepareData(round));
			for (unsigned w = 0; w < 2048; w++) {
				uint32_t expected = uint32_t(UINT64_C(0xa5c3e271)
					^ (uint64_t(round) * 0x794a270d) ^ (uint64_t(w) * 0x9e3779b9));
				assert(((uint32_t*)memory.Data())[w] == expected);
			}
			const uint64_t* wrapper = (uint64_t*)memory.Ring() + 16 * round;
			unsigned opcodes[] = {2,36,1,2,3,32,1,1,3,51,47,0,0,0,0,0};
			for (unsigned i = 0; i < 16; i++) assert((wrapper[i] >> 56) == opcodes[i]);
			assert((wrapper[2] & UINT64_C(0xffffffffffff)) == UINT64_C(0x100000000) + round * 256);
			assert((wrapper[6] & UINT64_C(0xffffffffffff)) == UINT64_C(0x100800000) + round * 32);
			assert((wrapper[5] & UINT64_C(0xffffffffffffff)) == UINT64_C(0x005c5e00000000));
			assert((wrapper[9] & UINT64_C(0xffffffffffffff)) == UINT64_C(0x005c5e00000001));
		}
		assert(!memory.PrepareData(2));
	}
	for (uint64_t physical : {UINT64_C(1), UINT64_C(1) << 40, (UINT64_C(1) << 40) - 4096}) {
		memset(arena.data, 0x5a, arena.size);
		assert(!memory.Build(arena.data, arena.size, physical));
		assert(memory.Data() == NULL && memory.RootPhysical() == 0);
		assert(memory.Firmware().SharedData() == NULL && memory.Firmware().RootPhysical() == 0);
		for (size_t n = 0; n < arena.size; n++) assert(arena.data[n] == 0x5a);
	}
	assert(!memory.Build(arena.data, arena.size - 1, 4096));
	assert(!memory.Build(arena.data + 1, arena.size, 4096));
	assert(!memory.Build(NULL, arena.size, 4096));
	FirmwareImage empty;
	assert(!memory.Plan(empty) && !memory.PrepareData(0) && memory.RequiredBytes() == 0);
}

int main()
{
	workspaces();
	commands();
	auto shared = shared_fixture();
	InterfaceInfo info;
	QueueInterfaceInfo queue;
	for (unsigned g = 0; g < 8; g++) {
		shared[(0x1000 + g * 0xa0) / 4 + 3] = 100000;
		shared[(0x1000 + g * 0xa0) / 4 + 4] = 32768;
	}
	assert(InspectInterface(shared.data(), 65536, 0x4000000, info, &queue));
	assert(queue.suspendBytes == 100000 && queue.protectedBytes == 32768);
	assert(queue.groupInputOffset == shared[1025] - 0x4000000);
	assert(queue.groupOutputOffset == shared[1026] - 0x4000000);
	assert(queue.streamInputOffset == shared[1041] - 0x4000000);
	assert(queue.streamOutputOffset == shared[1042] - 0x4000000);
	shared[1025] = 0xffffffff;
	assert(!InspectInterface(shared.data(), 65536, 0x4000000, info, &queue));
	assert(queue.groupInputOffset == 0 && queue.suspendBytes == 0);
	puts("MALI_CSF_COMMAND_MEMORY_TEST_PASS");
}
