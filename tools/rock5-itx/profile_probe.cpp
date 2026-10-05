/* Bounded workloads for native ARM64 profiler acceptance. MIT license. */
#include <OS.h>
#include <syscalls.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" uint64 rock5_profile_user_leaf(uint64 iterations);
extern "C" uint64 rock5_profile_bad_fp(uint64 iterations, addr_t fp);


extern "C" __attribute__((noinline)) uint64
rock5_profile_user_middle(uint64 iterations)
{
	return rock5_profile_user_leaf(iterations) + 1;
}


extern "C" __attribute__((noinline)) uint64
rock5_profile_user_outer(uint64 iterations)
{
	return rock5_profile_user_middle(iterations) + 1;
}


extern "C" __attribute__((noinline)) bool
rock5_profile_syscalls(thread_id thread, thread_info* inaccessible, uint64& copies)
{
	thread_info info;
	for (unsigned i = 0; i < 1024; i++) {
		if (get_thread_info(thread, &info) != B_OK || info.thread != thread
			|| _kern_get_thread_info(thread, inaccessible) != B_BAD_ADDRESS) {
			return false;
		}
		copies++;
	}
	return true;
}


int
main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "Usage: %s user|syscalls|inaccessible|cross-page|cycle|"
			"misaligned|noncanonical|kernel-fp|no-fp seconds(1..15)\n", argv[0]);
		return 2;
	}
	char* end;
	long seconds = strtol(argv[2], &end, 10);
	if (*end != 0 || seconds < 1 || seconds > 15)
		return 2;
	const char* mode = argv[1];
	uint8* memory = (uint8*)mmap(NULL, 2 * B_PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (memory == MAP_FAILED)
		return 1;
	uint8* guard = memory + B_PAGE_SIZE;
	if (mprotect(guard, B_PAGE_SIZE, PROT_NONE) != 0)
		return 1;
	addr_t fp = 0;
	bool user = strcmp(mode, "user") == 0;
	bool syscalls = strcmp(mode, "syscalls") == 0;
	if (strcmp(mode, "inaccessible") == 0)
		fp = (addr_t)guard;
	else if (strcmp(mode, "cross-page") == 0)
		fp = (addr_t)guard - sizeof(addr_t);
	else if (strcmp(mode, "cycle") == 0) {
		fp = (addr_t)memory;
		((addr_t*)memory)[0] = fp;
		((addr_t*)memory)[1] = (addr_t)&rock5_profile_user_leaf;
	} else if (strcmp(mode, "misaligned") == 0)
		fp = (addr_t)guard + 1;
	else if (strcmp(mode, "noncanonical") == 0)
		fp = 0x0008000000000000ULL;
	else if (strcmp(mode, "kernel-fp") == 0)
		fp = 0xffff000000000000ULL;
	else if (!user && !syscalls && strcmp(mode, "no-fp") != 0)
		return 2;

	alarm(30);
	thread_id thread = find_thread(NULL);
	bigtime_t deadline = system_time() + seconds * 1000000LL;
	uint64 loops = 0, copies = 0, checksum = 0;
	do {
		if (syscalls) {
			if (!rock5_profile_syscalls(thread, (thread_info*)guard, copies))
				return 1;
		} else if (user)
			checksum += rock5_profile_user_outer(500000);
		else
			checksum += rock5_profile_bad_fp(500000, fp);
		loops++;
	} while (system_time() < deadline);
	thread_info info;
	if (get_thread_info(thread, &info) != B_OK || info.thread != thread
		|| munmap(memory, 2 * B_PAGE_SIZE) != 0) {
		return 1;
	}
	alarm(0);
	printf("ROCK5_PROFILE_WORKLOAD_PASS mode=%s loops=%" B_PRIu64
		" invalid_copies=%" B_PRIu64 " checksum=%" B_PRIu64 "\n",
		mode, loops, copies, checksum);
	return 0;
}
