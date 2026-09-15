/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <inttypes.h>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include "CsfQueueEngine.h"
#define main MaliMemoryFixtureMain
#include "test_mali_memory.cpp"
#undef main

using int32 = int32_t;
using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using status_t = int32_t;
using area_id = int32_t;
using thread_id = int32_t;
using phys_addr_t = uint64_t;
using bigtime_t = int64_t;
static const int B_OK = 0, B_NOT_SUPPORTED = -1, B_BAD_VALUE = -2, B_BAD_DATA = -3,
	B_BAD_ADDRESS = -4, B_NO_MEMORY = -5, B_NOT_ALLOWED = -6, B_ENTRY_NOT_FOUND = -7,
	B_BUSY = -8, B_DEV_INVALID_IOCTL = -9, B_CANCELED = -10, B_IO_ERROR = -11,
	B_TIMED_OUT = -12, B_INTERRUPTED = -13, B_NORMAL_PRIORITY = 10;
static const uint32 B_SYSTEM_TEAM = 1, B_CONTIGUOUS = 3,
	B_KERNEL_READ_AREA = 4, B_KERNEL_WRITE_AREA = 8,
	B_RELATIVE_TIMEOUT = 1, B_ABSOLUTE_TIMEOUT = 2, B_CAN_INTERRUPT = 4;
#define B_PRId32 PRId32
#define B_PRIx64 PRIx64
#include "CsfClient.h"
#include "CsfDevice.h"

static bigtime_t system_time()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void memory_full_barrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }
static void KernelPrint(const char*, ...) {}
#define dprintf KernelPrint
struct mutex { std::mutex value; };
#define MUTEX_INITIALIZER(name) {}
struct MutexLocker {
	mutex& lock;
	explicit MutexLocker(mutex& value) : lock(value) { lock.value.lock(); }
	~MutexLocker() { lock.value.unlock(); }
};
static std::atomic<bool> sInterruptWait{false};
struct ConditionVariable {
	std::condition_variable changed;
	void Init(const void*, const char*) {}
	void NotifyAll() { changed.notify_all(); }
	status_t Wait(mutex* mutex, uint32 flags = 0, bigtime_t timeout = 0)
	{
		if ((flags & B_CAN_INTERRUPT) && sInterruptWait.exchange(false)) return B_INTERRUPTED;
		std::unique_lock<std::mutex> lock(mutex->value, std::adopt_lock);
		status_t status = B_OK;
		if (flags & (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT)) {
			bigtime_t remaining = (flags & B_ABSOLUTE_TIMEOUT) ? timeout - system_time() : timeout;
			if (changed.wait_for(lock, std::chrono::microseconds(remaining)) == std::cv_status::timeout)
				status = B_TIMED_OUT;
		} else changed.wait(lock);
		lock.release(); return status;
	}
};

struct Area { std::unique_ptr<Guarded> memory; uint64 physical; std::string name; };
static std::mutex sAreaLock;
static std::map<area_id, Area> sAreas;
static area_id sNextArea = 1;
static uint64 sNextPhysical = UINT64_C(0x280000000);
static std::atomic<unsigned> sAllocationCalls{0}, sFailAllocation{0}, sCopies{0}, sFailCopy{0};
struct virtual_address_restrictions { uint64 unused; };
struct physical_address_restrictions { uint64 low_address, high_address; };
struct physical_entry { uint64 address; size_t size; };
static area_id create_area_etc(int team, const char* name, size_t bytes, unsigned lock,
	uint32 protection, int flags, int spec, virtual_address_restrictions* virt,
	physical_address_restrictions* phys, void** address)
{
	assert(team == B_SYSTEM_TEAM && lock == B_CONTIGUOUS && bytes > 0 && !(bytes & 4095));
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA) && flags == 0 && spec == 0);
	assert(virt->unused == 0 && phys->low_address == 0 && phys->high_address == (UINT64_C(1) << 40));
	if (++sAllocationCalls == sFailAllocation) return B_NO_MEMORY;
	std::lock_guard<std::mutex> guard(sAreaLock);
	auto memory = std::make_unique<Guarded>(bytes);
	*address = memory->data;
	area_id id = sNextArea++;
	sAreas.emplace(id, Area{std::move(memory), sNextPhysical, name});
	sNextPhysical += bytes; return id;
}
static status_t get_memory_map(void* address, size_t bytes, physical_entry* entry, unsigned count)
{
	assert(count == 1);
	std::lock_guard<std::mutex> guard(sAreaLock);
	for (auto& pair : sAreas) if (pair.second.memory->data == address) {
		assert(pair.second.memory->size == bytes);
		entry->address = pair.second.physical; entry->size = bytes; return B_OK;
	}
	assert(false); return B_BAD_ADDRESS;
}
static status_t MakeFirmwareRamNoncacheable(area_id id, void* address, phys_addr_t physical, size_t bytes)
{
	std::lock_guard<std::mutex> guard(sAreaLock);
	Area& area = sAreas.at(id);
	assert(area.memory->data == address && area.memory->size == bytes && area.physical == physical);
	return B_OK;
}
static status_t delete_area(area_id id)
{
	std::lock_guard<std::mutex> guard(sAreaLock);
	assert(sAreas.erase(id) == 1); return B_OK;
}
bool FirmwareMemoryRetained()
{
	std::lock_guard<std::mutex> guard(sAreaLock);
	for (auto& pair : sAreas) if (pair.second.name == "Mali CSF firmware DMA") return true;
	return false;
}
static status_t user_memcpy(void* output, const void* input, size_t bytes)
{
	if (++sCopies == sFailCopy || output == NULL || input == NULL) return B_BAD_ADDRESS;
	memcpy(output, input, bytes); return B_OK;
}

struct Thread {
	int32 (*entry)(void*);
	void* data;
	std::thread thread;
	status_t result = B_OK;
};
static std::mutex sThreadLock;
static std::map<thread_id, std::shared_ptr<Thread>> sThreads;
static thread_id sNextThread = 1;
static std::atomic<bool> sFailSpawn{false}, sFailResume{false};
static thread_id spawn_kernel_thread(int32 (*entry)(void*), const char*, int, void* data)
{
	if (sFailSpawn) return B_NO_MEMORY;
	std::lock_guard<std::mutex> guard(sThreadLock);
	thread_id id = sNextThread++;
	auto thread = std::make_shared<Thread>(); thread->entry = entry; thread->data = data;
	sThreads.emplace(id, thread); return id;
}
static status_t resume_thread(thread_id id)
{
	if (sFailResume) return B_NO_MEMORY;
	std::lock_guard<std::mutex> guard(sThreadLock);
	auto thread = sThreads.at(id);
	thread->thread = std::thread([thread]() { thread->result = thread->entry(thread->data); });
	return B_OK;
}
static status_t kill_thread(thread_id id)
{
	std::lock_guard<std::mutex> guard(sThreadLock);
	assert(!sThreads.at(id)->thread.joinable()); return B_OK;
}
static status_t wait_for_thread(thread_id id, status_t* result)
{
	std::shared_ptr<Thread> thread;
	{
		std::lock_guard<std::mutex> guard(sThreadLock);
		thread = sThreads.at(id); sThreads.erase(id);
	}
	if (thread->thread.joinable()) thread->thread.join();
	*result = thread->result; return B_OK;
}

// A tiny independent client/VM oracle. User ownership can disappear while the
// runtime's leases must keep this root alive. Full mapping/BO semantics have
// their own production CsfClient fixture.
struct Generation {
	Guarded root{4096};
	unsigned references = 1;
	uint64 number;
	explicit Generation(uint64 value) : number(value) { memset(root.data, 0, root.size); }
};
struct Client { Generation* generation; bool closed = false; };
static std::mutex sLeaseLock;
static std::set<Generation*> sGenerations;
static Generation* NewGeneration(uint64 number)
{
	std::lock_guard<std::mutex> guard(sLeaseLock);
	Generation* value = new Generation(number); sGenerations.insert(value); return value;
}
static void ReleaseGeneration(Generation* value)
{
	if (value != NULL && --value->references == 0) {
		assert(sGenerations.erase(value) == 1); delete value;
	}
}
static void CloseVm(Client& client)
{
	std::lock_guard<std::mutex> guard(sLeaseLock);
	ReleaseGeneration(client.generation); client.generation = NULL;
}
static void AdvanceVm(Client& client)
{
	Generation* next = NewGeneration(client.generation->number + 1);
	std::lock_guard<std::mutex> guard(sLeaseLock);
	ReleaseGeneration(client.generation); client.generation = next;
}
status_t MaliCSF::AcquireClientVm(void* cookie, uint32 handle, uint64 generation, ClientVmLease& lease)
{
	std::lock_guard<std::mutex> guard(sLeaseLock);
	Client& client = *(Client*)cookie;
	if (client.closed) return B_NOT_ALLOWED;
	if (handle != 7 || client.generation == NULL) return B_ENTRY_NOT_FOUND;
	if (generation != 0 && generation != client.generation->number) return B_BUSY;
	client.generation->references++;
	lease.state = client.generation; lease.generation = client.generation->number;
	return B_OK;
}
void MaliCSF::ReleaseClientVm(ClientVmLease& lease)
{
	std::lock_guard<std::mutex> guard(sLeaseLock);
	ReleaseGeneration((Generation*)lease.state); lease = {};
}
const uint64_t* MaliCSF::ClientVmRoot(const ClientVmLease& lease)
{
	assert(lease.state != NULL); return (uint64_t*)((Generation*)lease.state)->root.data;
}
bool MaliCSF::ClientVmRange(const ClientVmLease& lease, uint64 address, uint64 bytes)
{
	assert(lease.state != NULL);
	return address >= 0x100000000 && address < 0x100001000 && bytes <= 0x100001000 - address;
}
static std::atomic<bool> sFailHardware{false};
struct FirmwareHardware {
	explicit FirmwareHardware(bool commands) { assert(commands); }
	status_t Init(const ResourceInfo&) { return sFailHardware ? B_IO_ERROR : B_OK; }
};
class Runtime;
static void DriveRuntime(Runtime*, FirmwareRunInfo&);
#include "runtime.inc"

static std::atomic<bool> sHoldJob{false}, sFailJob{false}, sFailBoot{false};
static std::atomic<unsigned> sEnteredJob{0}, sHardwareCycles{0};
static void DriveRuntime(Runtime* runtime, FirmwareRunInfo& info)
{
	sHardwareCycles++;
	if (sFailBoot) { info.flags |= kFirmwareNeedsRecovery; return; }
	runtime->Ready({});
	QueueProgress progress[8]{};
	for (;;) {
		QueueWork work{}; runtime->Next(work);
		if (work.type == kQueueWorkStop) break;
		if (work.type == kQueueWorkIdle) continue;
		if (work.type == kQueueWorkDestroy) { runtime->Destroyed(work.slot); continue; }
		assert(work.type == kQueueWorkSubmit && work.root != NULL);
		sEnteredJob++;
		while (sHoldJob) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (sFailJob) { info.flags |= kFirmwareNeedsRecovery; return; }
		assert(work.memory->SetUserRoot(work.root));
		if (work.generation != progress[work.slot].generation) runtime->Activated(work);
		auto& value = progress[work.slot];
		value.generation = work.generation; value.completed = work.sequence;
		value.insert += 128; value.extract = value.insert; value.interrupts++; value.syncEvents++;
		runtime->Complete(work, value);
	}
	info.result = info.cleanupResult = kFirmwareRunOK;
	info.flags |= kFirmwarePowerRestored | kFirmwareCleaned;
}

template<typename T> static status_t Call(Client& client, uint32 op, T& request, bool& recovery)
{
	return ControlQueues({}, &client, op, &request, sizeof(request), recovery);
}
static status_t Create(Client& client, uint32& handle, bool firmware, bool& recovery)
{
	auto file = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	std::vector<uint8_t> request(sizeof(QueueCreate) + (firmware ? file.size() : 0), 0);
	QueueCreate* create = (QueueCreate*)request.data();
	create->version = 1; create->vm = 7; create->firmwareBytes = firmware ? file.size() : 0;
	if (firmware) memcpy(request.data() + sizeof(*create), file.data(), file.size());
	status_t status = ControlQueues({}, &client, kCreateQueue, request.data(), request.size(), recovery);
	if (status == B_OK) {
		handle = create->handle;
		assert(handle != 0 && create->generation == client.generation->number);
	}
	return status;
}
static QueueInfo Info(Client& client, uint32 handle)
{
	bool recovery = false; QueueInfo info{}; info.version = 1; info.handle = handle;
	assert(Call(client, kGetQueueInfo, info, recovery) == B_OK && !recovery); return info;
}
static QueueSubmit Submit(Client& client, uint32 handle)
{
	QueueSubmit request{}; request.version = 1; request.handle = handle;
	request.streamAddress = 0x100000000; request.streamBytes = 48;
	bool recovery = false;
	assert(Call(client, kSubmitQueue, request, recovery) == B_OK && !recovery); return request;
}
static QueueWait Wait(Client& client, uint32 handle, uint64 sequence)
{
	QueueWait request{}; request.version = 1; request.handle = handle;
	request.sequence = sequence; request.timeoutMicros = 1000000;
	bool recovery = false;
	assert(Call(client, kWaitQueue, request, recovery) == B_OK && !recovery); return request;
}
static void Destroy(Client& client, uint32 handle)
{
	QueueHandle request{1, handle, 0}; bool recovery = false;
	assert(Call(client, kDestroyQueue, request, recovery) == B_OK && !recovery);
}
static void AwaitJob(unsigned before)
{
	bigtime_t deadline = system_time() + 1000000;
	while (sEnteredJob == before && system_time() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	assert(sEnteredJob > before);
}
static void Clean(unsigned generations)
{
	assert(sRuntime == NULL && sThreads.empty() && sAreas.empty());
	assert(sGenerations.size() == generations);
}
static void DropSimulatedRetainedRuntime()
{
	// Fixture teardown only: the simulated GPU owns no RAM. Production has no
	// such recovery shortcut; uncertain hardware allocations survive to reboot.
	Runtime* runtime = sRuntime;
	assert(runtime != NULL && runtime->retained && runtime->finished && runtime->thread == -1);
	assert(runtime->references == 1); sRuntime = NULL; PutRuntime(runtime);
}

int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	Client client{NewGeneration(1)}, other{NewGeneration(1)};
	bool recovery = false; uint32 handle = 0;
	assert(Create(client, handle, false, recovery) == B_ENTRY_NOT_FOUND); Clean(2);
	for (unsigned fail = 1; fail <= 2; fail++) {
		sAllocationCalls = 0; sFailAllocation = fail;
		assert(Create(client, handle, true, recovery) == B_NO_MEMORY && !recovery);
		Clean(2); assert(client.generation->references == 1);
	}
	sFailAllocation = 0;
	sFailSpawn = true;
	assert(Create(client, handle, true, recovery) == B_NO_MEMORY); Clean(2); sFailSpawn = false;
	sFailResume = true;
	assert(Create(client, handle, true, recovery) == B_NO_MEMORY); Clean(2); sFailResume = false;
	sFailHardware = true;
	assert(Create(client, handle, true, recovery) == B_IO_ERROR && !recovery); Clean(2); sFailHardware = false;
	for (unsigned fail = 1; fail <= 3; fail++) {
		sCopies = 0; sFailCopy = fail;
		assert(Create(client, handle, true, recovery) == B_BAD_ADDRESS && !recovery);
		Clean(2); assert(client.generation->references == 1);
	}
	sFailCopy = 0;
	assert(Create(client, handle, true, recovery) == B_OK);
	unsigned cycles = sHardwareCycles;
	uint32 handles[8]; handles[0] = handle;
	for (unsigned i = 1; i < 8; i++) assert(Create(client, handles[i], false, recovery) == B_OK);
	uint32 excess = 0;
	assert(Create(client, excess, false, recovery) == B_NO_MEMORY && excess == 0);
	assert(Create(client, excess, true, recovery) == B_BAD_VALUE && sHardwareCycles == cycles);
	QueueInfo invalid{}; invalid.version = 1; invalid.handle = handle;
	assert(Call(other, kGetQueueInfo, invalid, recovery) == B_ENTRY_NOT_FOUND);
	assert(Info(client, handle).state == kQueueReady);
	QueueSubmit bad{}; bad.version = 1; bad.handle = handle;
	bad.streamAddress = 0x100000000; bad.streamBytes = 8;
	sCopies = 0; sFailCopy = 2;
	assert(Call(client, kSubmitQueue, bad, recovery) == B_BAD_ADDRESS);
	sFailCopy = 0;
	assert(Info(client, handle).submitted == 0 && client.generation->references == 9);
	bad.streamAddress += 4096;
	assert(Call(client, kSubmitQueue, bad, recovery) == B_BAD_VALUE);
	for (unsigned i = 1; i < 8; i++) Destroy(client, handles[i]);
	assert(client.generation->references == 2);

	sHoldJob = true; unsigned before = sEnteredJob;
	auto job = Submit(client, handle); AwaitJob(before);
	assert(job.sequence == 1 && job.generation == 1 && client.generation->references == 3);
	QueueWait wait{}; wait.version = 1; wait.handle = handle; wait.sequence = 1;
	assert(Call(client, kWaitQueue, wait, recovery) == B_TIMED_OUT);
	wait.timeoutMicros = 1000;
	assert(Call(client, kWaitQueue, wait, recovery) == B_TIMED_OUT);
	wait.timeoutMicros = -1; sInterruptWait = true;
	assert(Call(client, kWaitQueue, wait, recovery) == B_INTERRUPTED);
	Generation* old = client.generation; AdvanceVm(client);
	assert(old->references == 2 && sGenerations.size() == 3);
	auto newJob = Submit(client, handle);
	assert(newJob.generation == 2 && newJob.sequence == 2);
	bad.streamAddress = 0x100000000; bad.generation = 1;
	assert(Call(client, kSubmitQueue, bad, recovery) == B_BUSY);
	sHoldJob = false;
	assert(Wait(client, handle, 2).result == B_OK);
	assert(sGenerations.size() == 2 && client.generation->references == 2);
	assert(Info(client, handle).activeGeneration == 2);

	// Queued jobs retain the original snapshot, and timed-out waits leave it
	// untouched. Closing the VM prevents new submissions, but not old work.
	sHoldJob = true; before = sEnteredJob; Submit(client, handle); AwaitJob(before);
	for (unsigned i = 1; i < 64; i++) Submit(client, handle);
	bad.generation = 0;
	assert(Call(client, kSubmitQueue, bad, recovery) == B_BUSY);
	assert(Info(client, handle).pending == 64);
	CloseVm(client);
	assert(Call(client, kSubmitQueue, bad, recovery) == B_ENTRY_NOT_FOUND);
	std::atomic<bool> closed{false};
	std::thread closer([&]() { CloseQueues(&client, recovery); closed = true; });
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	assert(!closed);
	sHoldJob = false; closer.join();
	assert(closed && !recovery); Clean(1);

	// Two API users may wait and close concurrently; no worker outlives the
	// final close, and a stale handle never becomes a subsequently reused slot.
	client.generation = NewGeneration(1);
	assert(Create(client, handle, true, recovery) == B_OK);
	uint32 stale = handle;
	sHoldJob = true; before = sEnteredJob; job = Submit(client, handle); AwaitJob(before);
	status_t waitStatus = B_IO_ERROR;
	std::thread waiter([&]() {
		QueueWait pending{}; pending.version = 1; pending.handle = handle;
		pending.sequence = job.sequence; pending.timeoutMicros = -1; bool ignored = false;
		waitStatus = Call(client, kWaitQueue, pending, ignored);
		if (waitStatus == B_OK) assert(pending.result == B_OK || pending.result == B_CANCELED);
	});
	std::thread closeAgain([&]() { CloseQueues(&client, recovery); });
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	sHoldJob = false; waiter.join(); closeAgain.join();
	assert(waitStatus == B_OK || waitStatus == B_ENTRY_NOT_FOUND); Clean(2);
	assert(Create(client, handle, true, recovery) == B_OK && handle != stale);
	invalid.handle = stale;
	assert(Call(client, kGetQueueInfo, invalid, recovery) == B_ENTRY_NOT_FOUND);
	Destroy(client, handle); Clean(2);

	// A failed running job leaves both its queued lease and the previously
	// active root alive after user closure, until the fixture simulates reboot.
	assert(Create(client, handle, true, recovery) == B_OK);
	sFailJob = true; job = Submit(client, handle);
	assert(Wait(client, handle, job.sequence).result == B_IO_ERROR);
	assert(Info(client, handle).state == kQueueFailed);
	CloseVm(client); CloseQueues(&client, recovery);
	assert(recovery && !sAreas.empty() && sThreads.empty() && sGenerations.size() == 2);
	assert(Create(other, excess, true, recovery) == B_BAD_VALUE);
	DropSimulatedRetainedRuntime(); sFailJob = false; recovery = false; Clean(1);

	sFailBoot = true;
	assert(Create(other, handle, true, recovery) == B_IO_ERROR && recovery);
	assert(!sAreas.empty() && sThreads.empty() && other.generation->references == 1);
	DropSimulatedRetainedRuntime(); sFailBoot = false; recovery = false; Clean(1);
	CloseVm(other); Clean(0);
	puts("MALI_CSF_RUNTIME_TEST_PASS");
}
