#include "test_mmc_stubs.h"
#include <cerrno>
#include <climits>
#include <sys/types.h>
using addr_t = uintptr_t;
constexpr int B_DISK = 0;
static const uint32 kBlockSize = 512;
struct device_geometry {
    uint32 bytes_per_sector, sectors_per_track, cylinder_count, head_count, device_type;
    bool removable, read_only, write_once;
};
struct mutex { bool locked = false; };
struct MutexLocker {
    mutex* lock;
    explicit MutexLocker(mutex* m) : lock(m) { assert(!m->locked); m->locked = true; }
    ~MutexLocker() { assert(lock->locked); lock->locked = false; }
};
struct DMAResource {};
struct IORequest {
    off_t offset; size_t length; bool write;
    status_t Init(off_t o, addr_t, size_t n, bool w, int) { offset = o; length = n; write = w; return B_OK; }
    off_t Offset() const { return offset; }
    size_t Length() const { return length; }
    bool IsWrite() const { return write; }
    status_t Wait(int, int) { return B_OK; }
    size_t TransferredBytes() { return length; }
};
using io_request = IORequest;
static int scheduled;
struct IOScheduler { status_t ScheduleRequest(IORequest*) { scheduled++; return B_OK; } };
struct fs_trim_data {
    uint64 trimmed_size;
    uint32 range_count;
    struct { uint64 offset, size; } ranges[1];
};
#define ASSERT assert
#define STATIC_ASSERT static_assert
#define ROUNDUP(x, y) (((x) + (y) - 1) / (y) * (y))
#define min_c(a, b) std::min((a), (b))
static bigtime_t now;
static bigtime_t system_time() { return now; }
static void snooze(bigtime_t n) { now += n; }
static uint32 sectors;
static std::string fault;
struct device_manager_info {
    status_t get_attr_uint8(device_node*, const char* name, uint8* out, bool) {
        if (strcmp(name, kMmcBusWidthAttribute) == 0) {
            if (fault == "width-attribute") return B_ERROR;
            *out = fault == "invalid-width" ? 2 : 8;
            return B_OK;
        }
        *out = fault == "readonly-profile" && (strcmp(name, kMmcReadOnlyAttribute) == 0
            || strcmp(name, kMmcNonRemovableAttribute) == 0);
        return B_OK;
    }
    status_t get_attr_uint32(device_node*, const char* name, uint32* out, bool) {
        assert(strcmp(name, kMmcSectorCountAttribute) == 0);
        if (fault == "attribute") return B_ERROR;
        *out = sectors; return B_OK;
    }
};
static device_manager_info manager;
static device_manager_info* sDeviceManager = &manager;
#include "mmc_disk.h"
#include "disk.inc"

struct CommandRecord { uint16 rca; uint8 command; uint32 argument; };
static std::vector<CommandRecord> commands;
static std::array<uint32, 4> csd;
static int busWidth, statusPolls;
static status_t execute(device_node*, void*, uint16 rca, uint8 command, uint32 argument, uint32* reply)
{
    commands.push_back({rca, command, argument});
    *reply = 0x900;
    if (command == SEND_CSD) {
        assert(rca == 0 && argument == (1 << 16));
        if (fault == "csd") return B_TIMED_OUT;
        std::copy(csd.begin(), csd.end(), reply);
    } else if (command == MMC_SWITCH) {
        if (fault == "switch") *reply |= 1 << 7;
        if (fault == "switch-transport") return B_IO_ERROR;
    } else if (command == SD_APP_CMD) {
        if (fault != "no-app") *reply |= 1 << 5;
    } else if (command == SEND_STATUS) {
        statusPolls++;
        if (fault == "status") return B_IO_ERROR;
        if (fault == "status-error") *reply |= 1 << 23;
        if (fault == "busy" || (fault == "delayed" && statusPolls < 3)) *reply = 0xe00;
        if (fault == "wrong-state") *reply = 0xb00;
    }
    return B_OK;
}
static void setWidth(device_node*, void*, int width) { assert(width == 4); busWidth = width; }
static mmc_device_interface interface = {{}, execute, nullptr, setWidth};

struct Fixture {
    mmc_disk_driver_info info{};
    IOScheduler scheduler;
    explicit Fixture(card_type type = CARD_TYPE_MMC_EXTENDED_CAPACITY) {
        fault.clear(); commands.clear(); busWidth = scheduled = statusPolls = 0; now = 0;
        info.mmc = &interface; info.rca = 1; info.cardType = type;
        info.flags = type == CARD_TYPE_MMC || type == CARD_TYPE_SD ? 0 : kIoCommandOffsetAsSectors;
        info.scheduler = &scheduler;
        sectors = 18874368; // Distinct 9 GiB eMMC fixture, above byte-addressing limit.
        csd = {0xef8e4040, 0xfff6dbff, 0x320f5903, 0x00d02701};
    }
    mmc_disk_handle* open(status_t expected = B_OK) {
        void* cookie = nullptr;
        assert(mmc_block_open(&info, "fixture", 0, &cookie) == expected);
        assert(!info.geometryLock.locked);
        if (expected != B_OK) assert(cookie == nullptr && info.geometry.bytes_per_sector == 0);
        return static_cast<mmc_disk_handle*>(cookie);
    }
};

int main()
{
    for (card_type type : {CARD_TYPE_MMC, CARD_TYPE_MMC_EXTENDED_CAPACITY}) {
        Fixture f(type); auto* handle = f.open();
        assert(f.info.DeviceSize() == (int64_t(9) << 30));
        assert(f.info.geometry.bytes_per_sector == 512 && busWidth == 0);
        // Opening the disk must preserve the width already verified by the bus.
        assert(commands.size() == (type == CARD_TYPE_MMC ? 2 : 1));
        if (type == CARD_TYPE_MMC) assert(commands[1].command == 16 && commands[1].argument == 512);
        size_t n = commands.size(); auto* second = f.open();
        assert(commands.size() == n);
        mmc_block_free(second); mmc_block_free(handle);
    }
    for (const char* failure : {"csd", "attribute", "width-attribute", "invalid-width"}) {
        Fixture f; fault = failure;
        status_t expected = fault == "csd" ? B_TIMED_OUT : B_BAD_DATA;
        f.open(expected); assert(busWidth == 0);
        fault.clear(); auto* handle = f.open();
        assert(f.info.DeviceSize() == (int64_t(9) << 30)); mmc_block_free(handle);
    }
    { Fixture f; sectors = 0; f.open(B_BAD_DATA); assert(busWidth == 0); }
    { Fixture f; sectors = UINT32_MAX; auto* handle = f.open();
      assert(f.info.DeviceSize() == 2199023255040LL); mmc_block_free(handle); }
    { Fixture f(CARD_TYPE_SDHC);
      // QEMU's independent SDHC v2 CSD: 8 GiB, 512-byte sectors.
      csd = {0x000a4000, 0x003fff7f, 0x325b5900, 0x00400e00};
      auto* handle = f.open();
      assert(f.info.DeviceSize() == (int64_t(8) << 30));
      assert(commands[1].command == 55 && commands[2].command == 6 && commands[2].argument == 2);
      size_t before = commands.size();
      assert(mmc_flush_cache(&f.info) == B_NOT_SUPPORTED && commands.size() == before);
      mmc_block_free(handle);
    }
    for (bool enabled : {false, true}) {
        Fixture f; f.info.cacheEnabled = enabled;
        fault = "delayed";
        assert(mmc_flush_cache(&f.info) == B_OK && statusPolls == 3);
        assert(commands.size() == (enabled ? 4 : 3));
        if (enabled) assert(commands[0].command == 6 && commands[0].argument == 0x03200100);
        assert(commands.back().command == 13 && now == 2000);
    }
    for (const char* failure : {"switch", "switch-transport", "status", "status-error", "busy", "wrong-state"}) {
        Fixture f; f.info.cacheEnabled = true; fault = failure;
        assert(mmc_flush_cache(&f.info) == ((fault == "busy" || fault == "wrong-state") ? B_TIMED_OUT : B_IO_ERROR));
        assert(now <= 1000000);
        if (fault == "switch" || fault == "switch-transport") assert(commands.size() == 1);
    }
    { Fixture f; csd[0] |= 1 << 4; auto* handle = f.open();
      assert(f.info.geometry.read_only);
      char buffer[512]{}; size_t length = sizeof(buffer);
      assert(mmc_block_write(handle, 0, buffer, &length) == B_READ_ONLY_DEVICE);
      IORequest request{0, 512, true};
      assert(mmc_block_io(handle, &request) == B_READ_ONLY_DEVICE && scheduled == 0);
      fs_trim_data trim{}; size_t before = commands.size();
      assert(mmc_block_trim(&f.info, &trim) == B_NOT_SUPPORTED && commands.size() == before);
      mmc_block_free(handle);
    }
    { Fixture f; fault = "readonly-profile"; auto* handle = f.open();
      assert(f.info.geometry.read_only && !f.info.geometry.removable);
      mmc_block_free(handle);
    }
    { Fixture f; auto* handle = f.open(); char buffer[512]{}; size_t length = 512;
      assert(mmc_block_write(handle, -1, buffer, &length) == B_BAD_VALUE);
      assert(mmc_block_read(handle, -1, buffer, &length) == B_BAD_VALUE);
      IORequest request{INT64_MAX, SIZE_MAX, true};
      assert(mmc_block_io(handle, &request) == ERANGE && scheduled == 0);
      length = 512;
      assert(mmc_block_read(handle, f.info.DeviceSize() - 1, buffer, &length) == B_OK);
      assert(length == 1 && scheduled == 1);
      mmc_block_free(handle);
    }
    puts("MMC geometry, flush and media restrictions passed");
}
