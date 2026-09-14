/* Exercise the actual sampler with protected kernel pages and fake user memory. */
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <setjmp.h>
#include <sys/mman.h>
#include <vector>

using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int32 = int32_t;
using addr_t = uintptr_t;
using status_t = int32;
static const status_t B_OK = 0, B_ERROR = -1;
static const size_t B_PAGE_SIZE = 4096, KERNEL_STACK_GUARD_PAGES = 1;
static const uint32 STACK_TRACE_KERNEL = 1, STACK_TRACE_USER = 2;
static const uint64 PSR_M_MASK = 15, PSR_M_32 = 16, PSR_M_EL0t = 0,
	PSR_M_EL1h = 5, PSR_M_EL2h = 9;
static const addr_t kKernelPC = 0xffff000000100000ULL;
static const addr_t kUserPC = 0x100000, USER_BASE = 0x1000, USER_TOP = 0xffffffff;
static uint8* sStack;
static bool sInterruptsEnabled;
static unsigned sUserReads, sUserFaults;

#include <arch/arm64/arch_thread_types.h>

struct Thread {
	arch_thread arch_info;
	addr_t kernel_stack_base, kernel_stack_top;
	void (*fault_handler)();
	jmp_buf fault_handler_state;
};
static Thread sThread;
static Thread* sCurrent;
static addr_t sFP;
static Thread* thread_get_current_thread() { return sCurrent; }
static addr_t arm64_get_fp() { return sFP; }
#define arch_cpu_enable_user_access()
#define arch_cpu_disable_user_access()
#include <arch/generic/user_memory.h>

static bool
IsKernelAddress(addr_t address)
{
	return (address >= (addr_t)sStack && address < (addr_t)sStack + 6 * B_PAGE_SIZE)
		|| (address >= kKernelPC && address < kKernelPC + 0x10000);
}
#define IS_KERNEL_ADDRESS(x) IsKernelAddress((addr_t)(x))
#define IS_USER_ADDRESS(x) ((addr_t)(x) >= USER_BASE && (addr_t)(x) <= USER_TOP)

static bool
is_user_address_range(const void* data, size_t size)
{
	addr_t address = (addr_t)data;
	return address + size >= address && IS_USER_ADDRESS(address)
		&& IS_USER_ADDRESS(address + size - 1);
}

class InterruptsLocker {
public:
	InterruptsLocker() : fPrevious(sInterruptsEnabled) { sInterruptsEnabled = false; }
	~InterruptsLocker() { sInterruptsEnabled = fPrevious; }
private:
	bool fPrevious;
};

struct Frame { addr_t previous, pc; };
static std::map<addr_t, Frame> sUserFrames;

static status_t
user_memcpy(void* to, const void* from, size_t size)
{
	assert(!sInterruptsEnabled);
	assert(size == sizeof(Frame));
	assert(sCurrent->arch_info.iframes.index < IFRAME_TRACE_DEPTH);
	sUserReads++;
	return user_access([=] {
		auto found = sUserFrames.find((addr_t)from);
		if (found == sUserFrames.end()) {
			sUserFaults++;
			// Simulate the synchronous abort's return to the installed handler.
			sCurrent->fault_handler();
			assert(false);
		}
		memcpy(to, &found->second, size);
	}) ? B_OK : B_ERROR;
}

#include "stack_trace.inc"

static void
SetFrame(addr_t fp, addr_t previous, addr_t pc)
{
	Frame frame = {previous, pc};
	memcpy((void*)fp, &frame, sizeof(frame));
}

static void
SetIFrame(int index, addr_t address, uint64 mode, addr_t pc, addr_t fp)
{
	iframe frame = {};
	frame.spsr = mode;
	frame.elr = pc;
	frame.fp = fp;
	memcpy((void*)address, &frame, sizeof(frame));
	sThread.arch_info.iframes.frames[index] = (iframe*)address;
}

static void
Reset()
{
	memset(sStack + B_PAGE_SIZE, 0, 4 * B_PAGE_SIZE);
	sThread = {};
	sCurrent = &sThread;
	sThread.kernel_stack_base = (addr_t)sStack;
	sThread.kernel_stack_top = (addr_t)sStack + 5 * B_PAGE_SIZE;
	sThread.arch_info.iframes.index = 2;
	sFP = (addr_t)sStack + 0x1400;
	SetFrame(sFP, (addr_t)sStack + 0x1808, kKernelPC + 0x400);
	SetIFrame(1, (addr_t)sStack + 0x1808, PSR_M_EL1h, kKernelPC + 0x100,
		(addr_t)sStack + 0x2000);
	SetFrame((addr_t)sStack + 0x2000, (addr_t)sStack + 0x3007, kKernelPC + 0x200);
	SetIFrame(0, (addr_t)sStack + 0x3007, PSR_M_EL0t, kUserPC + 0x100, 0x10000);
	sUserFrames = {{0x10000, {0x10020, kUserPC + 0x200}},
		{0x10020, {0, kUserPC + 0x300}}};
	sInterruptsEnabled = true;
	sUserReads = sUserFaults = 0;
}

static void
Expect(std::initializer_list<addr_t> expected, int skipIframes = 1,
	int skipFrames = 0, uint32 flags = STACK_TRACE_KERNEL | STACK_TRACE_USER,
	int maxCount = 16)
{
	std::vector<addr_t> output(maxCount > 0 ? maxCount + 2 : 2, 0xfeed);
	bool enabled = sInterruptsEnabled;
	int32 count = arch_get_stack_trace(output.data() + 1, maxCount,
		skipIframes, skipFrames, flags);
	assert(sInterruptsEnabled == enabled);
	assert(count == (int)expected.size());
	assert(output.front() == 0xfeed && output.back() == 0xfeed);
	unsigned i = 1;
	for (addr_t address : expected)
		assert(output[i++] == address);
	while (i < output.size())
		assert(output[i++] == 0xfeed);
}

int
main()
{
	sStack = (uint8*)mmap(nullptr, 6 * B_PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(sStack != MAP_FAILED);
	assert(mprotect(sStack, B_PAGE_SIZE, PROT_NONE) == 0);
	assert(mprotect(sStack + 5 * B_PAGE_SIZE, B_PAGE_SIZE, PROT_NONE) == 0);
	Reset();
	Expect({kKernelPC + 0x100, kKernelPC + 0x200, kUserPC + 0x100,
		kUserPC + 0x200, kUserPC + 0x300});
	assert(sUserReads == 2 && sUserFaults == 0);
	// Captured QEMU VHE frames use EL2h, including other saved PSTATE bits.
	for (uint64 mode : {PSR_M_EL1h, PSR_M_EL2h, uint64(0x60002009)}) {
		Reset();
		SetIFrame(1, (addr_t)sStack + 0x1808, mode, kKernelPC + 0x100,
			(addr_t)sStack + 0x2000);
		Expect({kKernelPC + 0x100, kKernelPC + 0x200, kUserPC + 0x100,
			kUserPC + 0x200, kUserPC + 0x300});
	}
	Reset();
	Expect({kKernelPC + 0x400, kKernelPC + 0x100, kKernelPC + 0x200,
		kUserPC + 0x100, kUserPC + 0x200, kUserPC + 0x300}, 0);
	Expect({kKernelPC + 0x100, kKernelPC + 0x200}, 1, 0, STACK_TRACE_KERNEL);
	Expect({kUserPC + 0x100, kUserPC + 0x200, kUserPC + 0x300}, 0, 0,
		STACK_TRACE_USER);
	Expect({kUserPC + 0x100, kUserPC + 0x200, kUserPC + 0x300}, 2, INT_MAX);
	Expect({kUserPC + 0x200, kUserPC + 0x300}, 0, 1, STACK_TRACE_USER);
	Expect({kKernelPC + 0x100}, 0, 1, STACK_TRACE_KERNEL, 1);
	Expect({kKernelPC + 0x100, kKernelPC + 0x200}, 1, INT_MAX, 3, 2);
	sInterruptsEnabled = false;
	Expect({kKernelPC + 0x100}, 1, 0, 3, 1);
	Expect({}, 3);
	Expect({}, -1);
	Expect({}, 0, -1);
	Expect({}, 0, 0, 0);
	Expect({}, 0, 0, 3, 0);
	Expect({}, 0, 0, 3, -1);
	assert(arch_get_stack_trace(nullptr, 10, 1, 0, 3) == 0);
	sCurrent = nullptr;
	Expect({});
	Reset();
	for (int32 depth : {-1, IFRAME_TRACE_DEPTH + 1}) {
		sThread.arch_info.iframes.index = depth;
		Expect({});
	}

	// Kernel reads may neither touch guard pages nor straddle the stack top.
	for (addr_t address : {(addr_t)sStack, (addr_t)sStack + B_PAGE_SIZE - 8,
		(addr_t)sStack + 5 * B_PAGE_SIZE - 8, (addr_t)sStack + 5 * B_PAGE_SIZE,
		kKernelPC, addr_t(0x10000), UINTPTR_MAX - 7}) {
		Reset();
		sFP = address;
		Expect({}, 0);
		sThread.arch_info.iframes.frames[1] = (iframe*)address;
		Expect({});
	}
	Reset();
	sFP = (addr_t)sStack + B_PAGE_SIZE;
	SetFrame(sFP, 0, kKernelPC + 0x100);
	Expect({kKernelPC + 0x100}, 0);
	sFP = (addr_t)sStack + 5 * B_PAGE_SIZE - sizeof(Frame);
	SetFrame(sFP, 0, kKernelPC + 0x100);
	Expect({kKernelPC + 0x100}, 0);
	sFP += 4;
	Expect({}, 0);

	// Stop on bad PCs/modes and require an explicit EL0 iframe to change stacks.
	for (addr_t pc : {addr_t(0), kUserPC, kKernelPC + 1, addr_t(0x800000000000)}) {
		Reset();
		SetIFrame(1, (addr_t)sStack + 0x1808, PSR_M_EL1h, pc, 0x10000);
		Expect({});
	}
	Reset();
	for (uint64 mode : {uint64(4), uint64(8), uint64(12), PSR_M_32,
		PSR_M_32 | PSR_M_EL1h, PSR_M_32 | PSR_M_EL2h}) {
		SetIFrame(1, (addr_t)sStack + 0x1808, mode, kKernelPC + 0x100, 0x10000);
		Expect({});
	}
	SetIFrame(1, (addr_t)sStack + 0x1808, PSR_M_EL1h, kKernelPC + 0x100, 0x10000);
	Expect({kKernelPC + 0x100});
	assert(sUserReads == 0);
	Reset();
	sUserFrames[0x10000].pc = kKernelPC;
	Expect({kUserPC + 0x100}, 0, 0, STACK_TRACE_USER);

	// Invalid/missing user pages fail through the real nested fault guard.
	for (addr_t fp : {addr_t(0), addr_t(0x10004), USER_TOP - 7,
		UINTPTR_MAX - 7, (addr_t)sStack}) {
		Reset();
		SetIFrame(0, (addr_t)sStack + 0x3007, PSR_M_EL0t, kUserPC + 0x100, fp);
		Expect({kUserPC + 0x100}, 0, 0, STACK_TRACE_USER);
		assert(sUserReads == 0);
	}
	Reset();
	sUserFrames.clear();
	bool outerFaultRecovered = !user_access([] {
		jmp_buf previous;
		memcpy(previous, sThread.fault_handler_state, sizeof(previous));
		auto handler = sThread.fault_handler;
		Expect({kUserPC + 0x100}, 0, 0, STACK_TRACE_USER);
		assert(sThread.fault_handler == handler);
		assert(memcmp(previous, sThread.fault_handler_state, sizeof(previous)) == 0);
		sThread.fault_handler();
		assert(false);
	});
	assert(outerFaultRecovered && sThread.fault_handler == nullptr);
	assert(sUserReads == 1 && sUserFaults == 1);
	Reset();
	assert(user_access([] {
		jmp_buf previous;
		memcpy(previous, sThread.fault_handler_state, sizeof(previous));
		Expect({kUserPC + 0x100, kUserPC + 0x200, kUserPC + 0x300}, 0, 0, 2);
		assert(memcmp(previous, sThread.fault_handler_state, sizeof(previous)) == 0);
	}));
	assert(sThread.fault_handler == nullptr);

	// At maximum exception depth the saved PC is usable, but another fault is not.
	Reset();
	sThread.arch_info.iframes.index = IFRAME_TRACE_DEPTH;
	sThread.arch_info.iframes.frames[2] = sThread.arch_info.iframes.frames[1];
	sThread.arch_info.iframes.frames[3] = sThread.arch_info.iframes.frames[1];
	Expect({kUserPC + 0x100}, 0, 0, STACK_TRACE_USER);
	assert(sUserReads == 0);

	// Reject non-progressing chains, and cap work independently of skip/count.
	Reset();
	sUserFrames[0x10000].previous = 0x10000;
	Expect({kUserPC + 0x100, kUserPC + 0x200}, 0, 0, 2);
	assert(sUserReads == 1);
	Reset();
	sUserFrames[0x10020].previous = 0x10000;
	Expect({kUserPC + 0x100, kUserPC + 0x200, kUserPC + 0x300}, 0, 0, 2);
	assert(sUserReads == 2);
	Reset();
	SetFrame(sFP, sFP, kKernelPC + 0x400);
	Expect({kKernelPC + 0x400}, 0);
	Reset();
	sUserFrames.clear();
	for (addr_t i = 0; i < 2048; i++)
		sUserFrames[0x10000 + i * 16] = {0x10010 + i * 16, kUserPC + 0x200};
	Expect({}, 0, INT_MAX, STACK_TRACE_USER);
	assert(sUserReads == 1023);
	std::vector<addr_t> output(2048, 0);
	assert(arch_get_stack_trace(output.data(), output.size(), 0, 0, 2) == 1024);
	assert(output[0] == kUserPC + 0x100 && output[1023] == kUserPC + 0x200);
	assert(output[1024] == 0);
	assert(munmap(sStack, 6 * B_PAGE_SIZE) == 0);
	puts("ARM64 sampler: frames, bounds, filters, faults and limits passed");
}
