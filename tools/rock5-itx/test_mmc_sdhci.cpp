/* Hardware side effects are faked; command, PIO and DMA submission are real. */
#include "test_mmc_stubs.h"

static void postCommand(uint16);
static uint32 readFifo();
struct Doorbell16 {
    uint16 bits;
    operator uint16() const { return bits; }
    void operator=(uint16 value) { bits = value; postCommand(value); }
};
struct W1C32 {
    uint32 bits;
    operator uint32() const { return bits; }
    void operator=(uint32 value) { bits &= ~value; }
    void operator|=(uint32 value) { *this = bits | value; }
};
struct Fifo32 { uint32 unused; operator uint32() const { return readFifo(); } };
static bigtime_t now;
static std::function<void()> onAdd, onWait;
struct ConditionVariableEntry {
    status_t Wait(int flags, bigtime_t deadline) {
        assert(flags == B_ABSOLUTE_TIMEOUT);
        if (onWait) { auto callback = onWait; onWait = {}; callback(); return B_OK; }
        now = deadline;
        return B_TIMED_OUT;
    }
};
struct ConditionVariable {
    void Init(void*, const char*) {}
    void Add(ConditionVariableEntry*) {
        if (onAdd) { auto callback = onAdd; onAdd = {}; callback(); }
    }
    void NotifyAll() {}
};
static bigtime_t system_time() { return now; }
static void snooze(bigtime_t);
static void memory_full_barrier();
static status_t vm_memcpy_from_physical(void*, phys_addr_t, size_t, bool);
static status_t vm_memcpy_to_physical(phys_addr_t, const void*, size_t, bool);
static int32 atomic_get(int32* p) { return *p; }
static void atomic_set(int32* p, int32 n) { *p = n; }
static void atomic_or(int32* p, int32 n) { *p |= n; }
static status_t install_io_interrupt_handler(uint32, int32 (*)(void*), void*, int);
static void remove_io_interrupt_handler(uint32, int32 (*)(void*), void*);
static thread_id spawn_kernel_thread(status_t (*)(void*), const char*, int, void*);
static status_t resume_thread(thread_id);
static void kill_thread(thread_id);
static void wait_for_thread(thread_id, status_t*);
static area_id area_for(void*) { return 42; }
static void delete_area(area_id);
static status_t release_sem(sem_id) { return B_OK; }
static status_t release_sem_etc(sem_id, int, int) { return B_OK; }
struct IOOperation {
    int64_t offset = 0;
    generic_size_t length = 512;
    bool write = false;
    std::vector<generic_io_vec> vecs = {{0x100000, 512}};
    int64_t Offset() const { return offset; }
    generic_size_t Length() const { return length; }
    bool IsWrite() const { return write; }
    const generic_io_vec* Vecs() const { return vecs.data(); }
    size_t VecCount() const { return vecs.size(); }
};
#define private public
#include "sdhci.h"
#undef private
#include "sdhci.inc"

struct Submitted { uint16 command; uint32 argument, address; uint16 mode, blocks; };
static registers* regs;
static SdhciBus* bus;
static std::string fault;
static std::vector<Submitted> submissions;
static int installed, workers, unmapped, pioWords, resets, freedDMA;
static bool pending;
static std::array<uint8, 512> cardData;
static std::function<void()> dmaTransfer;
static std::vector<uint8> cpuMemory;
static const phys_addr_t kCpuAddress = UINT64_C(0x100001000);
static int copiedIn, copiedOut;

static status_t vm_memcpy_from_physical(void* out, phys_addr_t address, size_t size, bool user)
{
    assert(!user && address >= kCpuAddress && address + size <= kCpuAddress + cpuMemory.size());
    copiedIn++;
    if (fault == "copy-in") return B_IO_ERROR;
    memcpy(out, cpuMemory.data() + address - kCpuAddress, size);
    return B_OK;
}
static status_t vm_memcpy_to_physical(phys_addr_t address, const void* in, size_t size, bool user)
{
    assert(!user && address >= kCpuAddress && address + size <= kCpuAddress + cpuMemory.size());
    copiedOut++;
    if (fault == "copy-out") return B_IO_ERROR;
    memcpy(cpuMemory.data() + address - kCpuAddress, in, size);
    return B_OK;
}

static void service()
{
    if (regs->software_reset.fBits && fault != "reset"
            && !(fault == "reset-after-error" && !submissions.empty())) {
        regs->software_reset.fBits = 0;
        regs->present_state.fBits &= ~3u;
        pending = false;
        resets++;
    }
    if ((regs->clock_control.fBits & 1) && fault != "clock")
        regs->clock_control.fBits |= 2;
    if (!pending) return;
    pending = false;
    if (fault == "no-command" || fault == "reset-after-error") return;
    uint16 command = submissions.back().command;
    regs->present_state.fBits &= ~1u;
    regs->response[0] = 0x900;
    regs->response[1] = 0x12345678;
    regs->response[2] = 0x89abcdef;
    regs->response[3] = 0x55aa55aa;
    uint32 completion = SDHCI_INT_CMD_CMP;
    if (fault == "command-crc") completion = SDHCI_INT_ERROR | SDHCI_INT_COMMAND_CRC;
    if (fault == "command-timeout") completion = SDHCI_INT_ERROR | SDHCI_INT_COMMAND_TIMEOUT;
    if (fault == "card-error") regs->response[0] |= 1u << 22;
    if ((command & 0x20) != 0 && fault != "command-crc") {
        if ((command >> 8) == 8) {
            pioWords = 0;
            regs->present_state.fBits |= 2;
            if (fault != "late-pio") regs->present_state.fBits |= 1 << 11;
            completion |= SDHCI_INT_BUF_READ_READY;
        } else if (fault != "no-transfer") {
            if (dmaTransfer) dmaTransfer();
            completion |= SDHCI_INT_TRANS_CMP;
            if (fault == "data-crc") completion |= SDHCI_INT_DATA_CRC | SDHCI_INT_ERROR;
        }
    }
    if ((command & 3) == 3 && (fault == "busy" || fault == "busy-stuck")) {
        regs->present_state.fBits |= 2;
        if (fault == "busy") onWait = [] {
            regs->present_state.fBits &= ~2u;
            regs->interrupt_status.bits |= SDHCI_INT_TRANS_CMP;
            bus->HandleInterrupt();
        };
    } else if ((command & 3) == 3 && fault != "no-busy-irq")
        completion |= SDHCI_INT_TRANS_CMP;
    regs->interrupt_status.bits |= completion;
    bus->HandleInterrupt();
}
static void memory_full_barrier() { service(); }
static void snooze(bigtime_t delay)
{
    now += delay;
    service();
    if (fault == "late-pio" && now >= 1000)
        regs->present_state.fBits |= 1 << 11;
}
static void postCommand(uint16 command)
{
    submissions.push_back({command, regs->argument, regs->system_address,
        regs->transfer_mode, regs->block_count});
    pending = true;
    regs->present_state.fBits |= 1;
}
static uint32 readFifo()
{
    assert(pioWords < 128 && (regs->present_state.fBits & (1 << 11)));
    uint32 value = 0;
    for (unsigned i = 0; i < 4; i++) value |= uint32(cardData[4 * pioWords + i]) << (8 * i);
    pioWords++;
    if (pioWords == 64 && (fault == "short-pio" || fault == "pio-crc")) {
        regs->present_state.fBits &= ~(1 << 11);
        if (fault == "pio-crc") {
            regs->interrupt_status.bits |= SDHCI_INT_ERROR | SDHCI_INT_DATA_CRC;
            bus->HandleInterrupt();
        }
    }
    if (pioWords == 128) {
        regs->present_state.fBits &= ~((1 << 11) | 2);
        regs->interrupt_status.bits |= SDHCI_INT_TRANS_CMP;
        bus->HandleInterrupt();
    }
    return value;
}
static status_t install_io_interrupt_handler(uint32 irq, int32 (*)(void*), void* cookie, int)
{
    assert(irq == 237 && !installed);
    assert(regs->interrupt_signal_enable == 0 && regs->interrupt_status_enable == 0);
    bus = static_cast<SdhciBus*>(cookie);
    if (fault == "install") return B_ERROR;
    installed++; return B_OK;
}
static void remove_io_interrupt_handler(uint32 irq, int32 (*)(void*), void*)
{
    assert(irq == 237 && installed == 1 && !workers && !regs->interrupt_signal_enable);
    installed--;
}
static thread_id spawn_kernel_thread(status_t (*)(void*), const char*, int, void*)
{
    if (fault == "spawn") return B_NO_MEMORY;
    workers++; return 33;
}
static status_t resume_thread(thread_id id) { assert(id == 33); return fault == "resume" ? B_ERROR : B_OK; }
static void kill_thread(thread_id id) { assert(id == 33 && workers == 1); workers--; }
static void wait_for_thread(thread_id id, status_t* result)
{
    assert(id == 33 && workers == 1 && !unmapped);
    workers--; *result = B_OK;
}
static void delete_area(area_id id) {
    assert(!workers && !installed);
    if (id == 71) { assert(!freedDMA); freedDMA++; }
    else { assert(id == 42); unmapped++; }
}
static status_t platformClock(void*, uint32 requested, uint32* baseClock)
{
    assert(!(regs->clock_control.Bits() & 4));
    if (fault == "platform-clock") return B_IO_ERROR;
    if (requested != 400 && requested != 25000) return B_NOT_SUPPORTED;
    *baseClock = requested == 400 ? 375 : 24000;
    return B_OK;
}

struct Fixture {
    registers hardware{};
    SdhciBus* controller;
    explicit Fixture(const char* injected = "", bool poll = false,
            const sdhci_platform_info* platform = nullptr) {
        fault = injected; regs = &hardware; now = 0;
        assert(!installed && !workers);
        unmapped = resets = pioWords = freedDMA = 0; pending = false;
        copiedIn = copiedOut = 0; dmaTransfer = {};
        onAdd = {}; onWait = {}; submissions.clear();
        regs->capabilities.fBits = (uint64(1) << 24) | (200 << 8) | 1;
        regs->host_controller_version.specVersion = 2;
        regs->present_state.fBits = 1 << 16;
        for (unsigned i = 0; i < cardData.size(); i++) cardData[i] = (i * 17) ^ (i >> 4);
        controller = new SdhciBus(regs, 237, poll, platform);
    }
    ~Fixture() { delete controller; assert(unmapped == 1 && !installed && !workers); }
};

int main()
{
    static_assert(sizeof(W1C32) == 4 && sizeof(Doorbell16) == 2 && sizeof(Fifo32) == 4);
    static_assert(offsetof(registers, interrupt_status) == 0x30);
    static_assert(offsetof(registers, clock_control) == 0x2c);
    { Fixture f;
      regs->host_control.SetDMAMode(0x18);
      for (int width : {8, 4, 1, 8}) {
          bus->SetBusWidth(width);
          assert(regs->host_control.Bits() == (width == 8 ? 0x38 : width == 4 ? 0x1a : 0x18));
      }
    }
    for (const char* failure : {"install", "reset", "clock", "spawn", "resume"}) {
        Fixture f(failure, true);
        assert(f.controller->InitCheck() != B_OK && now <= 200000);
    }
    { Fixture f("", true); assert(f.controller->InitCheck() == B_OK); }
    for (card_type type : {CARD_TYPE_MMC, CARD_TYPE_MMC_EXTENDED_CAPACITY}) {
        Fixture f;
        bus->SetCardType(type);
        uint32 reply = 0;
        assert(bus->ExecuteCommand(MMC_SET_RELATIVE_ADDR, 1 << 16, &reply) == B_OK);
        assert((submissions.back().command & 0xff) == 0x1a && reply == 0x900);
        assert(bus->ExecuteCommand(MMC_SWITCH, 0x03b70100, &reply) == B_OK);
        assert((submissions.back().command & 0xff) == 0x1b && reply == 0x900);
        std::array<uint8, 514> data; data.fill(0xa5);
        assert(bus->ReadExtendedCsd(data.data() + 1) == B_OK);
        assert(data.front() == 0xa5 && data.back() == 0xa5);
        assert(std::equal(cardData.begin(), cardData.end(), data.begin() + 1));
        assert(pioWords == 128 && submissions.back().mode == 0x10);
        assert((submissions.back().command & 0xff) == 0x3a && submissions.back().blocks == 1);
        assert(regs->interrupt_status.bits == 0);
    }
    for (const char* failure : {"short-pio", "pio-crc", "late-pio"}) {
        Fixture f(failure); bus->SetCardType(CARD_TYPE_MMC_EXTENDED_CAPACITY);
        std::array<uint8, 514> data; data.fill(0xa5);
        status_t result = bus->ReadExtendedCsd(data.data() + 1);
        assert(result == (fault == "late-pio" ? B_OK : fault == "pio-crc" ? B_IO_ERROR : B_TIMED_OUT));
        assert(data.front() == 0xa5 && data.back() == 0xa5 && now <= 1100000);
        if (fault == "short-pio") assert(data[1 + 256] == 0xa5 && pioWords == 64);
    }
    { Fixture f; uint32 response[4]{};
      assert(bus->ExecuteCommand(ALL_SEND_CID, 0, response) == B_OK);
      assert(response[0] == 0x900 && response[3] == 0x55aa55aa);
      assert(bus->ExecuteCommand(SELECT_DESELECT_CARD, 0, nullptr) == B_OK);
      assert((submissions.back().command & 0xff) == 0);
      assert(bus->ExecuteCommand(SEND_STATUS, 0, nullptr) == B_BAD_VALUE);
      size_t count = submissions.size();
      assert(bus->ExecuteCommand(63, 0, response) == B_BAD_DATA && submissions.size() == count);
      bus->SetCardType(CARD_TYPE_SDHC);
      assert(bus->ReadExtendedCsd(reinterpret_cast<uint8*>(response)) == B_BAD_VALUE);
      assert(bus->ExecuteCommand(SD_SEND_IF_COND, 0x1aa, response) == B_OK);
      assert((submissions.back().command & 0xff) == 0x1a);
    }
    for (const char* failure : {"no-command", "command-timeout", "command-crc", "busy", "busy-stuck", "no-busy-irq", "reset-after-error"}) {
        Fixture f(failure); uint32 response = 0;
        bigtime_t before = now;
        status_t result = bus->ExecuteCommand(SELECT_DESELECT_CARD, 1 << 16, &response);
        assert(result == (fault == "busy" ? B_OK : fault == "command-crc" ? B_IO_ERROR : B_TIMED_OUT));
        assert(now - before <= 1100000);
        if (fault == "reset-after-error") {
            assert(bus->InitCheck() == B_TIMED_OUT && !regs->interrupt_signal_enable);
            size_t count = submissions.size();
            uint16 clock = regs->clock_control.Bits();
            bus->SetClock(400, false);
            assert(bus->InitCheck() == B_TIMED_OUT && clock == regs->clock_control.Bits());
            assert(bus->ExecuteCommand(SEND_STATUS, 0, &response) == B_TIMED_OUT);
            assert(submissions.size() == count);
        }
    }
    { Fixture f;
      // A completion delivered while registering the waiter must not be lost.
      bigtime_t before = now;
      onAdd = [] { atomic_or(&bus->fCommandResult, SDHCI_INT_TRANS_CMP); };
      assert(bus->WaitForCompletion(SDHCI_INT_TRANS_CMP, 1000) == B_OK && now == before);
      atomic_set(&bus->fCommandResult, 0);
      regs->interrupt_status.bits = SDHCI_INT_CMD_CMP | SDHCI_INT_BUF_READ_READY;
      bus->HandleInterrupt();
      assert(regs->interrupt_status.bits == SDHCI_INT_BUF_READ_READY);
      assert(bus->HandleInterrupt() == B_UNHANDLED_INTERRUPT);
      regs->interrupt_status.bits |= SDHCI_INT_DMA;
      assert(bus->HandleInterrupt() == B_UNHANDLED_INTERRUPT);
      regs->interrupt_status.bits |= SDHCI_INT_TRANS_CMP;
      bus->HandleInterrupt();
      assert((atomic_get(&bus->fCommandResult) & 3) == 3);
      regs->interrupt_status.bits |= SDHCI_INT_DATA_CRC;
      bus->HandleInterrupt();
      assert(bus->WaitForCompletion(SDHCI_INT_TRANS_CMP, 1000) == B_IO_ERROR);
      regs->interrupt_signal_enable = 0;
      regs->interrupt_status.bits = SDHCI_INT_CMD_CMP;
      assert(bus->HandleInterrupt() == B_UNHANDLED_INTERRUPT);
    }
    { Fixture f; ClockControl clock{};
      for (uint16 divisor : {1, 2, 3, 256, 512, 1024, 2046}) {
          uint16 effective = clock.SetDivider(divisor);
          uint16 field = ((clock.Bits() >> 8) & 255) | ((clock.Bits() & 0xc0) << 2);
          assert((field == 0 ? 1 : 2 * field) == effective);
          assert(effective >= divisor && effective <= divisor + 1);
      }
      bus->SetClock(400, false);
      uint16 raw = regs->clock_control.Bits();
      assert((((raw >> 8) & 255) | ((raw & 0xc0) << 2)) == 250);
    }
    for (const char* failure : {"", "platform-clock"}) {
        sdhci_platform_info platform = {platformClock, nullptr, true, true, 375};
        Fixture f(failure, false, &platform);
        if (fault == "platform-clock") {
            assert(bus->InitCheck() == B_IO_ERROR && !(regs->clock_control.Bits() & 4));
            continue;
        }
        assert(bus->InitCheck() == B_OK && (regs->clock_control.Bits() & 4));
        uint16 raw = regs->clock_control.Bits();
        assert((((raw >> 8) & 255) | ((raw & 0xc0) << 2)) == 1); // minimum nonzero divider
        bus->SetClock(25000, false);
        raw = regs->clock_control.Bits();
        assert((raw & 4) && (((raw >> 8) & 255) | ((raw & 0xc0) << 2)) == 1); // /2
        bus->SetClock(50000, false);
        assert(bus->InitCheck() == B_NOT_SUPPORTED && !(regs->clock_control.Bits() & 4));
    }
    for (bool quarantine : {false, true}) {
        for (bool resetFails : {false, true}) {
            { Fixture f;
              bus->fDMAArea = 71;
              bus->fDMAQuarantined = quarantine;
              if (resetFails) fault = "reset";
            }
            assert(freedDMA == int(!quarantine || !resetFails));
        }
    }
    { Fixture f; IOOperation operation;
      operation.offset = int64_t(5) << 30; operation.length = 1024;
      operation.vecs = {{0x100000, 512}, {0x200000, 512}};
      assert(bus->DoIO(SD_READ_MULTIPLE_BLOCKS, &operation, true) == B_OK);
      assert(submissions.size() == 2 && submissions[0].argument == 10485760);
      assert(submissions[1].argument == 10485761 && submissions[1].address == 0x200000);
      assert((submissions[0].mode & 0x37) == 0x37);
    }
    for (int problem = 0; problem < 12; problem++) {
        Fixture f; IOOperation op; uint8 cmd = SD_READ_MULTIPLE_BLOCKS;
        bool sectors = true;
        switch (problem) {
            case 0: op.offset = -512; break;
            case 1: op.offset = 1; break;
            case 2: op.length = 513; break;
            case 3: op.vecs[0].base = 0x100000000; break;
            case 4: op.vecs[0].base = 0xfffffe00; op.length = op.vecs[0].length = 1024; break;
            case 5: op.vecs[0].base = 0x17fe00; op.length = op.vecs[0].length = 1024; break;
            case 6: op.length = 1024; op.vecs.push_back({0x100000001, 512}); break;
            case 7: op.vecs.clear(); break;
            case 8: op.offset = int64_t(1) << 41; break;
            case 9: op.offset = int64_t(1) << 32; sectors = false; break;
            case 10: cmd = SD_WRITE_MULTIPLE_BLOCKS; break;
            case 11: cmd = SD_READ_SINGLE_BLOCK; op.length = op.vecs[0].length = 1024; break;
        }
        assert(bus->DoIO(cmd, &op, sectors) == B_BAD_VALUE && submissions.empty());
    }
    for (const char* failure : {"data-crc", "no-transfer", "card-error"}) {
        Fixture f(failure); IOOperation operation;
        status_t result = bus->DoIO(SD_READ_MULTIPLE_BLOCKS, &operation, true);
        assert(result == (fault == "no-transfer" ? B_TIMED_OUT : B_IO_ERROR));
        assert(submissions.size() == 1 && resets == 2 && now <= 1100000);
    }
    for (bool write : {false, true}) {
        Fixture f;
        std::vector<uint8> payload(kSdhciDmaSize + 32, 0x7d);
        cpuMemory.assign(kSdhciDmaSize + 32, 0xa5);
        for (unsigned i = 0; i < 1024; i++) cpuMemory[512 + i] = (i * 19) ^ (i >> 3);
        std::vector<uint8> expected(cpuMemory);
        bus->fDMABuffer = payload.data(); bus->fDMAAddress = 0x800000;
        IOOperation op; op.offset = int64_t(5) << 30; op.length = 1024; op.write = write;
        op.vecs = {{kCpuAddress + 512, 1024}};
        dmaTransfer = [&] {
            assert(submissions.back().address == 0x800000);
            if (write) assert(memcmp(payload.data(), expected.data() + 512, 1024) == 0);
            else memcpy(payload.data(), expected.data() + 512, 1024);
        };
        if (!write) std::fill(cpuMemory.begin() + 512, cpuMemory.begin() + 1536, 0);
        assert(bus->DoIO(write ? SD_WRITE_MULTIPLE_BLOCKS : SD_READ_MULTIPLE_BLOCKS, &op, true) == B_OK);
        assert(cpuMemory == expected && payload[kSdhciDmaSize] == 0x7d);
        assert(copiedIn == int(write) && copiedOut == int(!write));
    }
    for (const char* failure : {"data-crc", "no-transfer", "copy-in", "copy-out", "reset-after-error"}) {
        Fixture f(failure);
        std::vector<uint8> payload(kSdhciDmaSize, 0);
        cpuMemory.assign(512, 0xa5);
        bus->fDMABuffer = payload.data(); bus->fDMAAddress = 0x800000;
        IOOperation op; op.vecs = {{kCpuAddress, 512}}; op.write = fault == "copy-in";
        dmaTransfer = [&] { memset(payload.data(), 0x66, 512); };
        assert(bus->DoIO(op.write ? SD_WRITE_MULTIPLE_BLOCKS : SD_READ_MULTIPLE_BLOCKS, &op, true) != B_OK);
        assert(std::all_of(cpuMemory.begin(), cpuMemory.end(), [](uint8 n) { return n == 0xa5; }));
        if (fault == "copy-in") assert(submissions.empty());
        if (fault == "reset-after-error") assert(bus->fDMAQuarantined);
    }
    { Fixture f; IOOperation op; op.write = true;
      bus->fPlatform.read_only = true;
      assert(bus->DoIO(SD_WRITE_MULTIPLE_BLOCKS, &op, true) == B_READ_ONLY_DEVICE && submissions.empty());
    }
    puts("SDHCI protocol, bounds and faults passed");
}
