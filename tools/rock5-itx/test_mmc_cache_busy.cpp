/* Production command dispatch and module locking against a delayed card. */
#include "test_mmc_stubs.h"

static bigtime_t now;
static bool locked;
static bigtime_t system_time() { return now; }
static void snooze(bigtime_t delay) { assert(locked && delay > 0); now += delay; }

struct MMCBus {
    mmc_bus_interface* fController;
    void* fCookie;
    card_type fCardType = CARD_TYPE_MMC_EXTENDED_CAPACITY;
    status_t activationStatus = B_OK;
    unsigned acquisitions = 0, releases = 0, activations = 0;
    void AcquireBus() { assert(!locked); locked = true; acquisitions++; }
    void ReleaseBus() { assert(locked); locked = false; releases++; }
    status_t _ActivateDevice(uint16_t) { assert(locked); activations++; return activationStatus; }
    status_t ExecuteCommand(uint16_t, uint8_t, uint32_t, uint32_t*);
};

#include "cache_busy.inc"

struct Fixture {
    mmc_bus_interface controller{};
    MMCBus bus;
    std::string fault;
    unsigned commands = 0, polls = 0;
    bigtime_t readyAt = 5000000;
    bigtime_t commandTime = 0;
    uint8_t command;
    uint32_t argument;
    Fixture() {
        assert(!locked);
        now = 0;
        controller.execute_command = execute;
        bus.fController = &controller;
        bus.fCookie = this;
    }
    static status_t execute(void* cookie, uint8_t command, uint32_t argument,
        uint32_t* reply)
    {
        Fixture& f = *static_cast<Fixture*>(cookie);
        // This applies to both the original command and every status poll.
        assert(locked && f.bus.acquisitions == 1 && f.bus.releases == 0);
        assert(reply);
        if (f.commands++ == 0) {
            assert(command == f.command && argument == f.argument);
            now += f.commandTime;
            *reply = 0xe00; // PRG, not ready.
            if (f.fault == "command") return B_TIMED_OUT;
            if (f.fault == "switch-error") *reply |= 1 << 7;
            return B_OK;
        }
        assert(command == SEND_STATUS && argument == 1u << 16);
        f.polls++;
        *reply = now >= f.readyAt ? 0x900 : 0xe00;
        if (f.fault == "poll-command") return B_IO_ERROR;
        if (f.fault == "poll-switch-error") *reply |= 1 << 7;
        if (f.fault == "poll-illegal") *reply |= 1 << 22;
        if (f.fault == "stuck") *reply = 0xe00;
        if (f.fault == "ready-wrong-state") *reply = 0x700; // Ready, STBY.
        return B_OK;
    }
    status_t run(uint8_t cmd, uint32_t arg, uint32_t* reply, uint16_t rca = 1) {
        command = cmd; argument = arg;
        status_t result = mmc_bus_execute_command(nullptr, &bus, rca, cmd, arg, reply);
        assert(!locked && bus.acquisitions == 1 && bus.releases == 1);
        return result;
    }
};

int main()
{
    for (uint32_t argument : {0x03200100u, 0x03210100u}) {
        for (bigtime_t delay : {bigtime_t(0), bigtime_t(5000000), bigtime_t(29000000)}) {
            Fixture f; f.readyAt = delay; uint32_t reply = 0;
            assert(f.run(MMC_SWITCH, argument, &reply) == B_OK);
            // Returning the initial command response is not completion.
            assert(f.polls > 0 && now >= delay && reply == 0x900);
            assert(now < 30001000);
        }
        for (const char* failure : {"command", "switch-error", "poll-command",
                "poll-switch-error", "poll-illegal", "stuck", "ready-wrong-state"}) {
            Fixture f; f.fault = failure; uint32_t reply = 0;
            bool timeout = f.fault == "command" || f.fault == "stuck"
                || f.fault == "ready-wrong-state";
            assert(f.run(MMC_SWITCH, argument, &reply) == (timeout ? B_TIMED_OUT : B_IO_ERROR));
            if (f.fault == "command" || f.fault == "switch-error") assert(f.polls == 0);
            if (f.fault == "stuck" || f.fault == "ready-wrong-state")
                assert(now >= 30000000 && now < 30001000);
        }
        { Fixture f; f.bus.activationStatus = B_BUSY; uint32_t reply = 0;
          assert(f.run(MMC_SWITCH, argument, &reply) == B_BUSY && f.commands == 0);
        }
        { Fixture f; uint32_t reply = 0;
          f.commandTime = 5000000; f.readyAt = 30000001;
          assert(f.run(MMC_SWITCH, argument, &reply) == B_TIMED_OUT);
          assert(now == 30000000); // The command itself consumes the same budget.
        }
        { Fixture f;
          assert(f.run(MMC_SWITCH, argument, nullptr) == B_BAD_VALUE);
          assert(!f.commands && !f.bus.activations);
        }
        { Fixture f; uint32_t reply = 0;
          assert(f.run(MMC_SWITCH, argument, &reply, 0) == B_BAD_VALUE);
          assert(!f.commands && !f.bus.activations);
        }
    }
    // Other MMC operations, and SD CMD6, keep their original completion policy.
    for (uint32_t argument : {0x03b70200u, 0x03200000u, 0x03210101u, 2u}) {
        Fixture f; uint32_t reply = 0;
        assert(f.run(MMC_SWITCH, argument, &reply) == B_OK);
        assert(f.commands == 1 && !f.polls && now == 0);
    }
    { Fixture f; f.bus.fCardType = CARD_TYPE_SD; uint32_t reply = 0;
      assert(f.run(SD_SET_BUS_WIDTH, 2, &reply) == B_OK);
      assert(f.commands == 1 && !f.polls && now == 0);
    }
    puts("MMC cache busy completion and bus serialization passed");
}
