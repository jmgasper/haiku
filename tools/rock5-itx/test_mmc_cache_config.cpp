/* Production cache setup against unsupported cards, ignored commands and corruption. */
#include "test_mmc_stubs.h"

struct MMCBus {
    std::array<uint8_t, 512> data;
    std::string fault;
    unsigned commands = 0, reads = 0;
    int corruptField = -1;
    status_t ExecuteCommand(uint16_t rca, uint8_t command, uint32_t argument,
        uint32_t* response)
    {
        assert(rca == 1 && command == MMC_SWITCH && argument == 0x03210100);
        assert(commands++ == 0 && !reads && !(data[33] & 1));
        *response = 0x900;
        if (fault == "command") return B_TIMED_OUT;
        if (fault == "switch-error") { *response |= 1 << 7; return B_OK; }
        if (fault == "illegal") { *response |= 1 << 22; return B_OK; }
        if (fault != "ignored") data[33] |= 1;
        return B_OK;
    }
    status_t ReadExtendedCsd(uint16_t rca, uint8_t out[512]) {
        assert(rca == 1 && reads++ == 0);
        if (fault == "read") return B_IO_ERROR;
        std::copy(data.begin(), data.end(), out);
        if (corruptField >= 0) out[corruptField] ^= 1;
        return B_OK;
    }
    void SetBusWidth(int) { assert(false); }
};

#include "cache_config.inc"

static std::array<uint8_t, 512> reference()
{
    std::array<uint8_t, 512> data;
    for (unsigned i = 0; i < data.size(); i++) data[i] = i * 37 + 5;
    data[33] = 0; data[61] = 0; data[162] = 0; data[179] = 0; data[192] = 8;
    uint32_t sectors = 15269888;
    for (unsigned i = 0; i < 4; i++) data[212 + i] = sectors >> (8 * i);
    data[249] = 0; data[250] = 0; data[251] = 1; data[252] = 0;
    assert(mmc_ext_csd_cache_size(data.data()) == 65536);
    return data;
}

int main()
{
    for (bool alreadyEnabled : {false, true}) {
        auto ref = reference(); ref[33] = alreadyEnabled;
        MMCBus bus; bus.data = ref;
        assert(configure_mmc_cache(&bus, 1, ref.data()) == B_OK);
        assert(bus.commands == unsigned(!alreadyEnabled) && bus.reads == 1);
        assert(bus.data[162] == ref[162] && bus.data[179] == ref[179]);
    }
    for (bool zeroCache : {false, true}) {
        auto ref = reference();
        if (zeroCache) std::fill(ref.begin() + 249, ref.begin() + 253, 0);
        else ref[192] = 5;
        MMCBus bus; bus.data = ref;
        assert(configure_mmc_cache(&bus, 1, ref.data()) == B_NOT_SUPPORTED);
        assert(!bus.commands && !bus.reads);
    }
    for (int field : {61, 179, 212}) {
        auto ref = reference();
        if (field == 212) std::fill(ref.begin() + 212, ref.begin() + 216, 0);
        else ref[field] = 1;
        MMCBus bus; bus.data = ref;
        assert(configure_mmc_cache(&bus, 1, ref.data()) == B_BAD_DATA);
        assert(!bus.commands && !bus.reads);
    }
    for (const char* failure : {"command", "switch-error", "illegal", "read", "ignored"}) {
        auto ref = reference(); MMCBus bus; bus.data = ref; bus.fault = failure;
        status_t expected = bus.fault == "command" ? B_TIMED_OUT
            : bus.fault == "ignored" ? B_BAD_DATA : B_IO_ERROR;
        assert(configure_mmc_cache(&bus, 1, ref.data()) == expected);
        assert(bus.commands == 1);
        assert(bus.reads == unsigned(bus.fault == "read" || bus.fault == "ignored"));
    }
    for (int field : {33, 61, 160, 162, 166, 168, 179, 181, 192, 194, 196, 197,
            198, 199, 200, 201, 202, 203, 212, 213, 214, 215, 217, 221, 222,
            223, 224, 226, 229, 230, 231, 232, 236, 237, 238, 239, 249, 250,
            251, 252, 253}) {
        auto ref = reference(); MMCBus bus; bus.data = ref; bus.corruptField = field;
        assert(configure_mmc_cache(&bus, 1, ref.data()) == B_BAD_DATA);
        assert(bus.commands == 1 && bus.reads == 1);
    }
    puts("MMC cache capability and state verification passed");
}
