/* Production width selection against card/host sequencing and corrupted data. */
#include "test_mmc_stubs.h"

struct MMCBus {
    std::array<uint8, 512> data{};
    std::string fault;
    uint32 argument = 0;
    unsigned cardWidth = 1, hostWidth = 1, commands = 0, changes = 0, reads = 0;
    status_t ExecuteCommand(uint16 rca, uint8 command, uint32 value, uint32* reply) {
        assert(rca == 1 && command == MMC_SWITCH && commands++ == 0);
        assert(hostWidth == 1 && cardWidth == 1 && !reads && !changes);
        argument = value;
        *reply = 0x900;
        if (fault == "command") return B_TIMED_OUT;
        if (fault == "switch-error") { *reply |= 1 << 7; return B_OK; }
        if (fault == "illegal-command") { *reply |= 1 << 22; return B_OK; }
        assert(value == 0x03b70000 || value == 0x03b70100 || value == 0x03b70200);
        cardWidth = value == 0x03b70200 ? 8 : value == 0x03b70100 ? 4 : 1;
        return B_OK;
    }
    void SetBusWidth(int width) {
        assert(commands == 1 && !reads && changes++ == 0);
        hostWidth = width;
        assert(hostWidth == cardWidth);
    }
    status_t ReadExtendedCsd(uint16 rca, uint8 out[512]) {
        assert(rca == 1 && commands == 1 && changes == 1 && reads++ == 0);
        assert(cardWidth == hostWidth);
        if (fault == "data") return B_IO_ERROR;
        std::copy(data.begin(), data.end(), out);
        // The writable bus-width byte is not used to prove the data path.
        out[183] = 0;
        return B_OK;
    }
};

#include "width.inc"

int main()
{
    std::array<uint8, 512> reference;
    for (unsigned i = 0; i < reference.size(); i++) reference[i] = i * 37 + 5;
    reference[33] = 1;
    reference[61] = 0;
    reference[179] = 0;
    reference[183] = 2;
    for (unsigned width : {1, 4, 8}) {
        MMCBus bus; bus.data = reference;
        assert(configure_mmc_width(&bus, 1, width, reference.data()) == B_OK);
        assert(bus.argument == (width == 8 ? 0x03b70200u : width == 4 ? 0x03b70100u : 0x03b70000u));
        assert(bus.hostWidth == width && bus.reads == 1);
    }
    for (unsigned width : {0, 2, 3, 16, 255}) {
        MMCBus bus; bus.data = reference;
        assert(configure_mmc_width(&bus, 1, width, reference.data()) == B_BAD_VALUE);
        assert(!bus.commands && !bus.changes && !bus.reads);
    }
    for (const char* failure : {"command", "switch-error", "illegal-command", "data"}) {
        MMCBus bus; bus.data = reference; bus.fault = failure;
        assert(configure_mmc_width(&bus, 1, 8, reference.data())
            == (bus.fault == "command" ? B_TIMED_OUT : B_IO_ERROR));
        assert(bus.commands == 1);
        if (bus.fault != "data") assert(!bus.changes && !bus.reads);
    }
    // A successful command alone cannot admit corrupted capacity, revision,
    // power/erase capabilities or a changed partition/cache/sector-size state.
    for (unsigned field : {33, 61, 179, 192, 196, 200, 212, 213, 214, 215, 224, 249, 252}) {
        MMCBus bus; bus.data = reference; bus.data[field] ^= 1;
        assert(configure_mmc_width(&bus, 1, 8, reference.data()) == B_BAD_DATA);
        assert(bus.reads == 1);
    }
    puts("MMC width sequencing and data corruption rejection passed");
}
