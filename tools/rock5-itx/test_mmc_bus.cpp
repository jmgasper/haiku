#include "test_mmc_stubs.h"

static std::string fault;
static int references, scanSem, lockSem, workers, semCalls, registered;
static status_t create_sem(int, const char*)
{
    int call = ++semCalls;
    if ((call == 1 && fault == "scan") || (call == 2 && fault == "lock")) return B_NO_MEMORY;
    if (call == 1) { scanSem++; return 5; }
    lockSem++; return 6;
}
static void delete_sem(sem_id id)
{
    assert(!registered);
    if (id == 5) { assert(scanSem == 1); scanSem--; }
    else { assert(id == 6 && lockSem == 1 && !workers); lockSem--; }
}
static void acquire_sem(sem_id) {}
static void release_sem(sem_id) {}
static thread_id spawn_kernel_thread(status_t (*)(void*), const char*, int, void*)
{
    assert(scanSem == 1 && lockSem == 1);
    if (fault == "spawn") return B_NO_MEMORY;
    workers++; return 7;
}
static status_t resume_thread(thread_id id) { assert(id == 7); return fault == "resume" ? B_ERROR : B_OK; }
static void kill_thread(thread_id id) { assert(id == 7 && workers == 1); workers--; }
static void wait_for_thread(thread_id id, status_t* result)
{
    assert(id == 7 && workers == 1 && lockSem == 1 && !scanSem && !registered);
    workers--; *result = B_OK;
}
static void setScan(void*, sem_id id)
{
    if (id == -1) registered = 0;
    else { assert(id == 5 && workers == 1); registered = 1; }
}
static mmc_bus_interface controller = {{}, nullptr, nullptr, nullptr, setScan,
    nullptr, nullptr, nullptr, nullptr};
struct device_manager_info {
    device_node node;
    device_node* get_parent_node(device_node*) { references++; return &node; }
    status_t get_driver(device_node*, driver_module_info** out, void** cookie) {
        if (fault == "driver") return B_ERROR;
        *out = &controller.info; *cookie = nullptr; return B_OK;
    }
    void put_node(device_node*) { assert(references == 1); references--; }
};
static device_manager_info manager;
device_manager_info* gDeviceManager = &manager;
#define MMC_BUS_H
#include "bus.h"
status_t MMCBus::_WorkerThread(void*) { return B_OK; }
#include "bus.inc"

int main()
{
    for (const char* failure : {"driver", "scan", "lock", "spawn", "resume", ""}) {
        fault = failure; semCalls = 0;
        {
            MMCBus bus(nullptr);
            assert((bus.InitCheck() == B_OK) == fault.empty());
            assert(!references);
        }
        assert(!scanSem && !lockSem && !workers && !registered);
    }
    puts("MMC bus partial initialization cleanup passed");
}
