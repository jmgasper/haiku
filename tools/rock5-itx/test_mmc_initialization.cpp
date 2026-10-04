/* Production MMC initialization with a card retaining an illegal SD command. */
#include "test_mmc_stubs.h"

static bigtime_t now;
static bigtime_t system_time() { return now; }
static void snooze(bigtime_t delay) { now += delay; }

struct MMCBus {
    std::string fault;
    bool illegal = false;
    int resets = 0, conditions = 0;
    status_t ExecuteCommand(uint16 address, uint8 command, uint32 argument, uint32* response) {
        assert(address == 0);
        if (command == SD_SEND_IF_COND) {
            assert(argument == 0x1aa);
            illegal = true;
            return B_TIMED_OUT;
        }
        if (command == GO_IDLE_STATE) {
            assert(!argument && response == nullptr);
            resets++;
            if (fault == "reset") return B_IO_ERROR;
            illegal = false;
            return B_OK;
        }
        if (command == MMC_SEND_OP_COND) {
            assert(argument == 0x40ff8000 && now >= 30000 && resets == 1);
            conditions++;
            if (fault == "condition") return B_TIMED_OUT;
            *response = fault == "all-ones" ? UINT32_MAX
                : fault == "busy" || conditions < 3 ? 0x40ff8080 : 0xc0ff8080;
            return B_OK;
        }
        assert(command == MMC_SET_RELATIVE_ADDR);
        *response = 0x500 | (illegal ? 1u << 22 : 0);
        illegal = false;
        return B_OK;
    }
};

#include "initialization.inc"

int main()
{
    for (const char* fault : {"", "reset", "condition", "all-ones", "busy"}) {
        MMCBus bus; bus.fault = fault; now = 0;
        uint32 reply = 0;
        assert(bus.ExecuteCommand(0, SD_SEND_IF_COND, 0x1aa, &reply) == B_TIMED_OUT);
        assert(bus.ExecuteCommand(0, MMC_SET_RELATIVE_ADDR, 1 << 16, &reply) == B_OK);
        assert(reply == 0x00400500); // Native +130 observation without the reset.
        assert(bus.ExecuteCommand(0, SD_SEND_IF_COND, 0x1aa, &reply) == B_TIMED_OUT);
        uint32 ocr = 0;
        status_t status = initialize_mmc(&bus, ocr);
        assert(now <= 2030000 && bus.resets == 1);
        if (bus.fault.empty()) {
            assert(status == B_OK && ocr == 0xc0ff8080 && bus.conditions == 3);
            assert(bus.ExecuteCommand(0, MMC_SET_RELATIVE_ADDR, 1 << 16, &reply) == B_OK);
            assert(reply == 0x500 && !(reply & kMmcR1ErrorMask));
        } else {
            assert(status != B_OK);
            if (bus.fault == "reset") assert(!bus.conditions && !now);
            if (bus.fault == "all-ones") assert(status == B_BAD_DATA && bus.conditions == 1);
            if (bus.fault == "busy") assert(status == B_TIMED_OUT && bus.conditions == 20);
        }
    }
    puts("MMC reset after SD probe and OCR failures passed");
}
