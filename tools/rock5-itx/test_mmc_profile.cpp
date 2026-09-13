/* Production admission and register sequencing with a high-word-mask CRU. */
#include "test_mmc_stubs.h"
#include "rk3588_profile.h"

using namespace RK3588Mmc;

static void put32(uint8* p, uint32 n)
{
    for (unsigned i = 0; i < 4; i++) p[i] = n >> (24 - i * 8);
}

struct IO {
    std::array<uint8, 4096> registers{};
    uint32 clock = 0x0590;
    bool failClock = false;
    unsigned clockWrites = 0, writes = 0, barriers = 0;
    uint8 Read8(unsigned offset) { return registers.at(offset); }
    uint16 Read16(unsigned offset) {
        assert(offset % 2 == 0 && offset + 2 <= registers.size());
        uint16 n; memcpy(&n, registers.data() + offset, 2); return n;
    }
    uint32 Read32(unsigned offset) {
        assert(offset % 4 == 0 && offset + 4 <= registers.size());
        uint32 n; memcpy(&n, registers.data() + offset, 4); return n;
    }
    void Write8(unsigned offset, uint8 n) {
        assert(barriers == 1 && clockWrites == 1); writes++;
        registers.at(offset) = n;
    }
    void Write16(unsigned offset, uint16 n) {
        assert(offset % 2 == 0 && offset + 2 <= registers.size());
        assert(barriers == 1 && clockWrites == 1); writes++;
        memcpy(registers.data() + offset, &n, 2);
    }
    void Write32(unsigned offset, uint32 n) {
        assert(offset % 4 == 0 && offset + 4 <= registers.size());
        assert(barriers == 1 && clockWrites == 1); writes++;
        memcpy(registers.data() + offset, &n, 4);
    }
    uint32 ReadClock() { assert(barriers == 1 && !writes); return clock; }
    void WriteClock(uint32 n) {
        assert(!clockWrites && !writes && !barriers && !(Read16(0x2c) & 4));
        clockWrites++;
        assert((n >> 16) == 0xff00 && !(n & 0xff));
        if (!failClock) clock = (clock & ~(n >> 16)) | (n & (n >> 16));
    }
    void Barrier() { barriers++; }
};

int main()
{
    Resources good = {0xfe2e0000, 0x10000, 237, 0xfd7c0000, 0x5c000,
        8, 150000000, 0x21, true, true, true, true, true, true, true};
    assert(Allows(good));
    for (auto field : {&Resources::base, &Resources::size, &Resources::interrupt,
            &Resources::cruBase, &Resources::cruSize}) {
        Resources p = good; p.*field ^= 1; assert(!Allows(p));
    }
    for (auto field : {&Resources::width, &Resources::maxFrequency, &Resources::clockPhandle}) {
        Resources p = good; p.*field = 0; assert(!Allows(p));
    }
    for (auto field : {&Resources::board, &Resources::nonRemovable, &Resources::noSD,
            &Resources::noSDIO, &Resources::identity, &Resources::levelHigh, &Resources::cru}) {
        Resources p = good; p.*field = false; assert(!Allows(p));
    }
    const char compatible[] = "radxa,rock-5-itx\0rockchip,rk3588";
    assert(HasString(compatible, sizeof(compatible), "rockchip,rk3588"));
    assert(!HasString(compatible, sizeof(compatible) - 1, "rockchip,rk3588"));
    assert(!HasString(compatible, sizeof(compatible), "rk3588"));
    assert(!HasString(nullptr, 100, "rk3588") && !HasString(compatible, -1, "rk3588"));
    std::array<uint8, 17> irq{};
    put32(irq.data() + 5, 205); put32(irq.data() + 9, 4);
    for (int length : {12, 16}) assert(Interrupt(irq.data() + 1, length));
    for (int length : {-1, 0, 11, 13, 15, 17}) assert(!Interrupt(irq.data() + 1, length));
    for (unsigned i : {4, 8, 12, 16}) {
        irq[i] ^= 1; assert(!Interrupt(irq.data() + 1, 16)); irq[i] ^= 1;
    }
    const char names[] = "core\0bus\0axi\0block\0timer";
    const uint32 clockIDs[] = {0x12c, 0x12a, 0x12b, 0x12d, 0x12e};
    const uint32 resetIDs[] = {0x118, 0x116, 0x117, 0x119, 0x11a};
    std::array<uint8, 41> references{};
    for (bool reset : {false, true}) {
        for (unsigned i = 0; i < 5; i++) {
            put32(references.data() + 1 + i * 8, 0x21);
            put32(references.data() + 5 + i * 8, reset ? resetIDs[i] : clockIDs[i]);
        }
        uint32 phandle = 0x21;
        auto check = [&] { return ClockReferences(references.data() + 1, 40, names,
            sizeof(names), reset, phandle); };
        assert(check() && phandle == 0x21);
        for (unsigned i = 1; i < references.size(); i++) {
            references[i] ^= 1; assert(!check()); references[i] ^= 1;
        }
        for (int length : {0, 39, 41})
            assert(!ClockReferences(references.data() + 1, length, names, sizeof(names), reset, phandle));
        assert(!ClockReferences(references.data() + 1, 40, names, sizeof(names) - 1, reset, phandle));
        if (reset) { phandle = 0x22; assert(!check() && phandle == 0x22); }
    }
    assert(Controller(0x226dc881, 0x08000007, 5, 0x01800500));
    for (unsigned i = 0; i < 32; i++) {
        assert(!Controller(0x226dc881 ^ (1u << i), 0x08000007, 5, 0x01800500));
        assert(!Controller(0x226dc881, 0x08000007 ^ (1u << i), 5, 0x01800500));
    }
    assert(!Controller(0x226dc881, 0x08000007, 4, 0x01800500));
    assert(!Controller(0x226dc881, 0x08000007, 5, 0x01800600));
    assert(ClockWrite(0xffff0590) == 0xff000500);
    for (bool identify : {false, true}) {
      IO io; io.registers.fill(0xa5); io.registers[0x2c] &= ~uint8(4);
      auto before = io.registers;
      assert(ConfigureLegacy(io, identify) && io.clock == (identify ? 0xbf90u : 0x8090u)
          && io.barriers == 2);
      assert(io.Read8(0x508) == 0xa4 && io.Read16(0x52c) == 0xa4a5);
      assert(io.Read32(0x800) == 0x01000001 && io.Read32(0x804) == 0x80000000);
      assert(io.Read32(0x808) == 0 && io.Read32(0x80c) == 0 && io.Read32(0x810) == 0);
      assert(io.Read16(0x3e) == 0xa5ad && io.writes == 8);
      for (unsigned i = 0; i < before.size(); i++) {
          bool touched = i == 0x508 || i == 0x52c || i == 0x52d
              || (i >= 0x800 && i < 0x814) || i == 0x3e || i == 0x3f;
          if (!touched) assert(io.registers[i] == before[i]);
      }
    }
    { IO io; io.failClock = true;
      assert(!ConfigureLegacy(io, true) && io.clockWrites == 1 && !io.writes && io.barriers == 1);
    }
    { IO io; io.registers[0x2c] = 4;
      assert(!ConfigureLegacy(io, true) && !io.clockWrites && !io.writes && !io.barriers);
    }
    puts("RK3588 MMC admission and clock sequencing passed");
}
