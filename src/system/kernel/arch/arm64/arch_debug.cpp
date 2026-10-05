/*
 * Copyright 2019-2026 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include <arch/debug.h>

#include <arch_cpu.h>
#include <debug.h>
#include <debug_heap.h>
#include <elf.h>
#include <kernel.h>
#include <kimage.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <vm/vm_types.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>

#include "VMSAv8TranslationMap.h"

#define NUM_PREVIOUS_LOCATIONS 32

extern struct iframe_stack gBootFrameStack;


static bool
already_visited(addr_t* visited, int32* _last, int32* _num, addr_t fp)
{
	int32 last = *_last;
	int32 num = *_num;

	for (int32 i = 0; i < num; i++) {
		if (visited[(NUM_PREVIOUS_LOCATIONS + last - i)
				% NUM_PREVIOUS_LOCATIONS] == fp) {
			return true;
		}
	}

	*_last = last = (last + 1) % NUM_PREVIOUS_LOCATIONS;
	visited[last] = fp;

	if (num < NUM_PREVIOUS_LOCATIONS)
		*_num = num + 1;

	return false;
}


static status_t
get_next_frame(addr_t fp, addr_t *next, addr_t *ip)
{
	if (fp == 0)
		return B_BAD_VALUE;

	addr_t frame[2]; // [0] = saved fp, [1] = return address
	if (debug_memcpy(B_CURRENT_TEAM, frame, (void*)fp, sizeof(frame)) != B_OK)
		return B_BAD_ADDRESS;

	*next = frame[0];
	*ip = frame[1];
	return B_OK;
}


static status_t
lookup_symbol(Thread* thread, addr_t address, addr_t* _baseAddress,
	const char** _symbolName, const char** _imageName, bool* _exactMatch)
{
	status_t status = B_ENTRY_NOT_FOUND;

	if (address >= KERNEL_BASE) {
		// a kernel symbol
		status = elf_debug_lookup_symbol_address(address, _baseAddress,
			_symbolName, _imageName, _exactMatch);
	} else if (thread != NULL && thread->team != NULL) {
		// try to locate the image in the images loaded into user space
		status = elf_debug_lookup_user_symbol_address(thread->team, address,
			_baseAddress, _symbolName, _imageName, _exactMatch);
	}

	return status;
}


static void
set_debug_argument_variable(int32 index, uint64 value)
{
	char name[8];
	snprintf(name, sizeof(name), "_arg%" B_PRId32, index);
	set_debug_variable(name, value);
}


template<typename Type>
static Type
read_function_argument_value(void* argument, bool& _valueKnown)
{
	Type value;
	if (debug_memcpy(B_CURRENT_TEAM, &value, argument, sizeof(Type)) == B_OK) {
		_valueKnown = true;
		return value;
	}

	_valueKnown = false;
	return 0;
}


static status_t
print_demangled_call(const char* image, const char* symbol, addr_t args,
	bool noObjectMethod, bool addDebugVariables)
{
	// Since arm64 uses registers rather than the stack for the first
	// 8 integer and 8 floating-point arguments, we cannot use
	// the same method as arm or x86 to read the function arguments from the stack.
	// For now just print out the function signature without the argument values.

	static const size_t kBufferSize = 256;
	char* buffer = (char*)debug_malloc(kBufferSize);
	if (buffer == NULL)
		return B_NO_MEMORY;

	bool isObjectMethod;
	const char* name = debug_demangle_symbol(symbol, buffer, kBufferSize,
		&isObjectMethod);
	if (name == NULL) {
		debug_free(buffer);
		return B_ERROR;
	}

	kprintf("<%s> %s(", image, name);

	size_t length;
	int32 type, i = 0;
	uint32 cookie = 0;
	while (debug_get_next_demangled_argument(&cookie, symbol, buffer,
			kBufferSize, &type, &length) == B_OK) {
		if (i++ > 0)
			kprintf(", ");

		if (buffer[0])
			kprintf("%s", buffer);
		else
			kprintf("???");
	}

	debug_free(buffer);

	kprintf(")");
	return B_OK;
}


static void
print_stack_frame(Thread* thread, addr_t ip, addr_t calleeFp, addr_t fp,
	int32 callIndex, bool demangle)
{
	const char* symbol;
	const char* image;
	addr_t baseAddress;
	bool exactMatch;
	status_t status;
	addr_t diff;

	diff = fp - calleeFp;

	// kernel space/user space switch
	if (calleeFp > fp)
		diff = 0;

	status = lookup_symbol(thread, ip, &baseAddress, &symbol, &image,
		&exactMatch);

	kprintf("%2" B_PRId32 " %0*lx (+%4ld) %0*lx   ", callIndex,
		B_PRINTF_POINTER_WIDTH, fp, diff, B_PRINTF_POINTER_WIDTH, ip);

	if (status == B_OK) {
		if (exactMatch && demangle) {
			status = print_demangled_call(image, symbol,
				fp, false, false);
		}

		if (!exactMatch || !demangle || status != B_OK) {
			if (symbol != NULL) {
				kprintf("<%s> %s%s", image, symbol,
					exactMatch ? "" : " (nearest)");
			} else
				kprintf("<%s@%p> <unknown>", image, (void*)baseAddress);
		}

		kprintf(" + %#04lx\n", ip - baseAddress);
	} else {
		VMArea *area = NULL;
		if (thread != NULL && thread->team != NULL
			&& thread->team->address_space != NULL) {
			area = thread->team->address_space->LookupArea(ip);
		}
		if (area != NULL) {
			kprintf("%" B_PRId32 ":%s@%p + %#lx\n", area->id, area->name,
				(void*)area->Base(), ip - area->Base());
		} else
			kprintf("\n");
	}
}

static int
stack_trace(int argc, char **argv)
{
	static const char* usage = "usage: %s [-d] [ <thread id> ]\n"
		"Prints a stack trace for the current, respectively the specified\n"
		"thread.\n"
		"  -d           -  Disables the demangling of the symbols.\n"
		"  <thread id>  -  The ID of the thread for which to print the stack\n"
		"                  trace.\n";
	bool demangle = true;
	int32 threadIndex = 1;
	if (argc > 1 && !strcmp(argv[1], "-d")) {
		demangle = false;
		threadIndex++;
	}

	if (argc > threadIndex + 1
		|| (argc == 2 && strcmp(argv[1], "--help") == 0)) {
		kprintf(usage, argv[0]);
		return 0;
	}

	addr_t previousLocations[NUM_PREVIOUS_LOCATIONS];
	Thread* thread = thread_get_current_thread();
	addr_t fp = arm64_get_fp();

	uint64 savedTTBR0 = 0;

	if (argc > threadIndex) {
		thread_id id = strtoul(argv[threadIndex], NULL, 0);
		Thread* target = Thread::GetDebug(id);
		if (target == NULL) {
			kprintf("could not find thread %" B_PRId32 "\n", id);
			return 0;
		}
		if (target != thread) {
			if (target->state == B_THREAD_RUNNING) {
				// TODO
				kprintf("Thread %" B_PRId32 " is running on another CPU. "
						"Tracing running threads isn't supported on arm64 yet\n",
					id);
				return 0;
			}
			thread = target;
			// x29 (frame pointer) is stored at regs[10]
			fp = thread->arch_info.regs[10];

			// If switched to user-space, save current TTBR0 and switch to the thread's one
			if (thread->team != NULL && thread->team->address_space != NULL) {
				auto* map = dynamic_cast<VMSAv8TranslationMap*>(
					thread->team->address_space->TranslationMap());
				savedTTBR0 = READ_SPECIALREG(TTBR0_EL1);
				WRITE_SPECIALREG(TTBR0_EL1, map->UserTTBR0());
				arm64_isb();
			}
		}
	}

	int32 num = 0, last = 0;
	struct iframe_stack *frameStack;

	if (argc > threadIndex) {
		// another thread: its frame pointer is where the context switch
		// left it (x29 follows x19 to x28 in arch_thread::regs)
		thread_id id = strtoul(argv[threadIndex], NULL, 0);
		Thread* other = Thread::GetDebug(id);
		if (other == NULL) {
			kprintf("could not find thread %" B_PRId32 "\n", id);
			return 0;
		}
		if (other != thread) {
			thread = other;
			fp = other->arch_info.regs[10];
		}
	}

	// We don't have a thread pointer early in the boot process
	if (thread != NULL)
		frameStack = &thread->arch_info.iframes;
	else
		frameStack = &gBootFrameStack;

	int32 i;
	for (i = 0; i < frameStack->index; i++) {
		kprintf("iframe %p (end = %p)\n",
			frameStack->frames[i], frameStack->frames[i] + 1);
	}

	if (thread != NULL) {
		kprintf("stack trace for thread 0x%" B_PRIx32 " \"%s\"\n", thread->id,
			thread->name);

		kprintf("    kernel stack: %p to %p\n",
			(void *)thread->kernel_stack_base,
			(void *)(thread->kernel_stack_top));
		if (thread->user_stack_base != 0) {
			kprintf("      user stack: %p to %p\n",
				(void *)thread->user_stack_base,
				(void *)(thread->user_stack_base + thread->user_stack_size));
		}
	}

	kprintf("frame            caller     <image>:function + offset\n");

	for (int32 callIndex = 0;; callIndex++) {
		// see if the frame pointer matches the iframe
		struct iframe *frame = NULL;
		for (i = 0; i < frameStack->index; i++) {
			if (fp == (addr_t)frameStack->frames[i]) {
				// it's an iframe
				frame = frameStack->frames[i];
				break;
			}
		}

		if (frame) {
			kprintf("iframe at %p\n", frame);
			dprintf("ELR=%016lx SPSR=%016lx\n", frame->elr, frame->spsr);
			dprintf("LR =%016lx SP  =%016lx FP =%016lx\n", frame->lr, frame->sp, frame->fp);
			dprintf("ESR=%016lx FAR =%016lx\n", frame->esr, frame->far);
			print_stack_frame(thread, frame->elr, fp, frame->fp, callIndex, demangle);
			fp = frame->fp;
		} else {
			addr_t ip, next;

			if (get_next_frame(fp, &next, &ip) != B_OK) {
				kprintf("%08lx -- read fault\n", fp);
				break;
			}

			if (ip == 0 || fp == 0)
				break;

			print_stack_frame(thread, ip, fp, next, callIndex, demangle);
			fp = next;
		}

		if (already_visited(previousLocations, &last, &num, fp)) {
			kprintf("circular stack frame: %p!\n", (void *)fp);
			break;
		}
		if (fp == 0)
			break;
	}

	if (savedTTBR0 != 0) {
		WRITE_SPECIALREG(TTBR0_EL1, savedTTBR0);
		arm64_isb();
	}

	return 0;
}


// #pragma mark -


void
arch_debug_save_registers(struct arch_debug_registers* registers)
{
}


bool
arch_debug_walk_stack(Thread* thread, bool (*callback)(void*, addr_t), void* context)
{
	return false;
}


void
arch_debug_stack_trace(void)
{
	stack_trace(0, NULL);
}


struct sampled_iframe {
	addr_t address;
	addr_t pc;
	addr_t fp;
	uint64 spsr;
};


static bool
is_sampled_kernel_stack_range(Thread* thread, addr_t address, size_t size)
{
	// kernel_stack_base includes the unmapped guard page. The rest of this
	// thread's stack is wired; never follow a frame into some other kernel area.
	return IS_KERNEL_ADDRESS(address)
		&& address >= thread->kernel_stack_base
		&& address - thread->kernel_stack_base
			>= KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE
		&& address < thread->kernel_stack_top
		&& size <= thread->kernel_stack_top - address;
}


static bool
read_sampled_iframe(Thread* thread, int32 index, sampled_iframe& frame)
{
	frame.address = (addr_t)thread->arch_info.iframes.frames[index];
	if (!is_sampled_kernel_stack_range(thread, frame.address, sizeof(iframe)))
		return false;

	// EXCEPTION_HANDLER sets FP to the raw iframe, not an AAPCS64 frame
	// record. The raw frame need not be naturally aligned (EL0's kernel SP
	// currently starts at kernel_stack_top - 1), so copy the fields as bytes.
	const uint8* source = (const uint8*)frame.address;
	memcpy(&frame.pc, source + offsetof(iframe, elr), sizeof(frame.pc));
	memcpy(&frame.fp, source + offsetof(iframe, fp), sizeof(frame.fp));
	memcpy(&frame.spsr, source + offsetof(iframe, spsr), sizeof(frame.spsr));
	return true;
}


int32
arch_get_stack_trace(addr_t* returnAddresses, int32 maxCount,
	int32 skipIframes, int32 skipFrames, uint32 flags)
{
	flags &= STACK_TRACE_KERNEL | STACK_TRACE_USER;
	if (returnAddresses == NULL || maxCount <= 0 || skipIframes < 0
		|| skipFrames < 0 || flags == 0) {
		return 0;
	}

	// Also support callers outside a timer interrupt. Keep the current thread
	// and its iframe list stable, and make user_memcpy fail without paging in
	// an absent user stack page. Its guard preserves any interrupted copy's
	// fault handler and saved jump buffer.
	InterruptsLocker interruptsLocker;
	Thread* thread = thread_get_current_thread();
	if (thread == NULL)
		return 0;

	// thread_exit() installs the kernel address space and moves the thread to
	// the kernel team before destroying its old user areas. Its saved EL0
	// iframe still exists then, but no longer describes the active user map.
	if (thread->team == NULL || thread->team == team_get_kernel_team()) {
		flags &= ~STACK_TRACE_USER;
		if (flags == 0)
			return 0;
	}

	int32 iframeCount = thread->arch_info.iframes.index;
	if (iframeCount < 0 || iframeCount > IFRAME_TRACE_DEPTH
		|| skipIframes > iframeCount) {
		return 0;
	}

	addr_t fp = arm64_get_fp();
	int32 iframeIndex = iframeCount - 1;
	if (skipIframes > 0 || flags == STACK_TRACE_USER) {
		// The profiler skips its timer interrupt. Start at that saved PC
		// directly, without walking all the profiler/interrupt-handler calls.
		if (skipIframes > 0) {
			iframeIndex = iframeCount - skipIframes;
			skipFrames = 0;
		}
		fp = 0;
		for (; iframeIndex >= 0; iframeIndex--) {
			sampled_iframe frame;
			if (!read_sampled_iframe(thread, iframeIndex, frame))
				return 0;
			if (flags != STACK_TRACE_USER
				|| (frame.spsr & PSR_M_MASK) == PSR_M_EL0t) {
				fp = frame.address;
				break;
			}
		}
	}

	bool kernel = true;
	int32 count = 0;
	// Bound work even when skipping an arbitrarily long/corrupt user chain.
	const int32 kMaxFrames = 1024;
	for (int32 walked = 0; fp != 0 && count < maxCount && walked < kMaxFrames;
		walked++) {
		bool wasKernel = kernel;
		addr_t nextFP, pc;
		int32 foundIFrame = -1;
		if (kernel) {
			for (int32 i = iframeIndex; i >= 0; i--) {
				if ((addr_t)thread->arch_info.iframes.frames[i] == fp) {
					foundIFrame = i;
					break;
				}
			}
		}

		if (foundIFrame >= 0) {
			sampled_iframe frame;
			if (!read_sampled_iframe(thread, foundIFrame, frame))
				break;
			uint64 mode = frame.spsr & PSR_M_MASK;
			// The EFI loader keeps the kernel at EL2 when VHE is available.
			if ((frame.spsr & PSR_M_32) != 0
				|| (mode != PSR_M_EL0t && mode != PSR_M_EL1h && mode != PSR_M_EL2h)) {
				break;
			}
			kernel = mode != PSR_M_EL0t;
			iframeIndex = foundIFrame - 1;
			nextFP = frame.fp;
			pc = frame.pc;
		} else {
			struct {
				addr_t previous;
				addr_t return_address;
			} frame;
			if ((fp & (sizeof(addr_t) - 1)) != 0)
				break;
			if (kernel) {
				if (!is_sampled_kernel_stack_range(thread, fp, sizeof(frame)))
					break;
				memcpy(&frame, (const void*)fp, sizeof(frame));
			} else {
				// A failed copy needs one more InterruptScope in do_sync_handler.
				if (iframeCount == IFRAME_TRACE_DEPTH
					|| !is_user_address_range((const void*)fp, sizeof(frame))
					|| user_memcpy(&frame, (const void*)fp, sizeof(frame)) != B_OK) {
					break;
				}
			}
			nextFP = frame.previous;
			pc = frame.return_address;
		}

		if ((pc & 3) != 0 || (kernel ? !IS_KERNEL_ADDRESS(pc) : !IS_USER_ADDRESS(pc)))
			break;
		if ((flags & (kernel ? STACK_TRACE_KERNEL : STACK_TRACE_USER)) == 0)
			break;
		if (skipFrames > 0)
			skipFrames--;
		else
			returnAddresses[count++] = pc;

		// Frame records progress up a descending stack. Only a saved EL0
		// exception may cross from the kernel stack into a user stack.
		if (nextFP != 0 && nextFP <= fp && wasKernel == kernel)
			break;
		fp = nextFP;
	}
	return count;
}


void*
arch_debug_get_interrupt_pc(bool* _isSyscall)
{
	return NULL;
}


bool
arch_is_debug_variable_defined(const char* variableName)
{
	return false;
}


status_t
arch_set_debug_variable(const char* variableName, uint64 value)
{
	return B_ENTRY_NOT_FOUND;
}


status_t
arch_get_debug_variable(const char* variableName, uint64* value)
{
	return B_ENTRY_NOT_FOUND;
}


void
arch_debug_snooze(bigtime_t duration)
{
	spin(duration);
}


status_t
arch_debug_init(kernel_args *args)
{
	add_debugger_command("where", &stack_trace, "Same as \"sc\"");
	add_debugger_command("bt", &stack_trace, "Same as \"sc\" (as in gdb)");
	add_debugger_command("sc", &stack_trace, "Stack crawl for current thread");

	return B_NO_ERROR;
}


void
arch_debug_unset_current_thread(void)
{
}


ssize_t
arch_debug_gdb_get_registers(char* buffer, size_t bufferSize)
{
	return B_NOT_SUPPORTED;
}
