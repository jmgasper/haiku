/* Kernel boundary for the production synchronization and descriptor fixtures. */
#include <fcntl.h>
#include <stddef.h>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include "CsfSynchronization.h"

struct vnode;
struct fs_mount;
struct file_descriptor;
struct io_context;
struct selectsync;
struct dirent;
struct module_info { const char* name; };
#include "fd_structures.inc"

static void mutex_lock(mutex* lock) { lock->value.lock(); }
static void mutex_unlock(mutex* lock) { lock->value.unlock(); }
using WriteLocker = MutexLocker;
#define O_CLOFORK 0x20000000
#define ASSERT_READ_LOCKED_RW_LOCK(lock) ((void)0)
#define ASSERT_WRITE_LOCKED_RW_LOCK(lock) ((void)0)
#define CHECK_BIT(byte, bit) ((byte) & (1 << (bit)))
#define SET_BIT(byte, bit) ((byte) |= (1 << (bit)))
#define CLEAR_BIT(byte, bit) ((byte) &= ~(1 << (bit)))
#define TFD(...) ((void)0)
#define TRACE(...) ((void)0)
static int32 atomic_add(int32* value, int32 delta) { return __atomic_fetch_add(value, delta, __ATOMIC_SEQ_CST); }
static thread_local team_id sSyncTestTeam = 1;
static team_id team_get_current_team_id() { return sSyncTestTeam; }
struct io_context {
	mutable mutex lock;
	uint32 table_size = 128, num_used_fds = 0;
	file_descriptor* fds[128] = {};
	uint8 fds_close_on_exec[16] = {}, fds_close_on_fork[16] = {};
};
static std::mutex sFdContextLock;
static std::map<team_id, std::unique_ptr<io_context>> sFdContexts;
static io_context* get_current_io_context(bool kernel)
{
	assert(!kernel);
	std::lock_guard<std::mutex> guard(sFdContextLock);
	auto& context = sFdContexts[sSyncTestTeam];
	if (!context) context = std::make_unique<io_context>();
	return context.get();
}

static std::atomic<unsigned> sDescriptorCount{0}, sModuleReferences{0}, sModulePuts{0};
static bool sFailDescriptor, sFailModule;
struct object_cache {};
static object_cache* sFileDescriptorCache;
static void* object_cache_alloc(object_cache*, int)
{
	if (sFailDescriptor) return NULL;
	void* value = std::malloc(sizeof(file_descriptor));
	assert(value != NULL); sDescriptorCount++; return value;
}
static void object_cache_free(object_cache*, void* value, int)
{
	assert(sDescriptorCount-- != 0); std::free(value);
}
static module_info sSyncTestModule{"drivers/graphics/mali_csf/device_v1"};
static status_t get_module(const char* name, module_info** module)
{
	assert(strcmp(name, sSyncTestModule.name) == 0);
	if (sFailModule) return B_ENTRY_NOT_FOUND;
	sModuleReferences++; *module = &sSyncTestModule; return B_OK;
}
static status_t put_module(const char* name)
{
	assert(strcmp(name, sSyncTestModule.name) == 0 && sModuleReferences-- != 0);
	sModulePuts++; return B_OK;
}
#include "fd_core.inc"

static file_descriptor* get_fd(const io_context* context, int fd)
{
	WriteLocker guard(context->lock);
	if (fd < 0 || unsigned(fd) >= context->table_size || context->fds[fd] == NULL) return NULL;
	file_descriptor* descriptor = context->fds[fd];
	if ((descriptor->open_mode & O_DISCONNECTED) != 0) return NULL;
	atomic_add(&descriptor->ref_count, 1); return descriptor;
}
static inline void SyncCloseFd(int fd)
{
	io_context* context = get_current_io_context(false);
	file_descriptor* descriptor;
	{
		WriteLocker guard(context->lock);
		assert(fd >= 0 && unsigned(fd) < context->table_size && context->fds[fd] != NULL);
		descriptor = context->fds[fd]; context->fds[fd] = NULL; context->num_used_fds--;
		fd_set_close_on_exec(context, fd, false); fd_set_close_on_fork(context, fd, false);
	}
	if (atomic_add(&descriptor->open_count, -1) == 1 && descriptor->ops != NULL
		&& descriptor->ops->fd_close != NULL) descriptor->ops->fd_close(descriptor);
	put_fd(descriptor);
}
static inline int SyncDupFd(int fd)
{
	io_context* context = get_current_io_context(false);
	file_descriptor* descriptor = get_fd(context, fd); assert(descriptor != NULL);
	int other = new_fd(context, descriptor); assert(other >= 0); return other;
}
static std::mutex sSyncAllocationLock;
static std::set<void*> sSyncAllocations;
static std::atomic<unsigned> sSyncAllocationCalls{0}, sSyncFailAllocation{0};
static void* SyncAllocate(size_t count, size_t size)
{
	if (++sSyncAllocationCalls == sSyncFailAllocation) return NULL;
	void* value = std::calloc(count, size);
	assert(value != NULL);
	std::lock_guard<std::mutex> guard(sSyncAllocationLock);
	assert(sSyncAllocations.insert(value).second); return value;
}
static void SyncFree(void* value)
{
	if (value == NULL) return;
	std::lock_guard<std::mutex> guard(sSyncAllocationLock);
	assert(sSyncAllocations.erase(value) == 1); std::free(value);
}
#define calloc SyncAllocate
#define free SyncFree
#include "sync.inc"
#undef calloc
#undef free
