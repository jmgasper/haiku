/*
 * Bounded SMP, process and fault-recovery checks for the dedicated lab.
 * Distributed under the terms of the MIT License.
 */

#include <OS.h>
#include <syscalls.h>

#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>


struct Worker {
	unsigned cpu;
	status_t affinity;
	uint64 samples;
	uint64 wrongCpu;
};

static pthread_mutex_t sClockLock = PTHREAD_MUTEX_INITIALIZER;
static bigtime_t sLastTime;
static bigtime_t sDeadline;
static uint64 sBackwards;
static sem_id sStart;
static sigjmp_buf sFaultReturn;
static volatile sig_atomic_t sExpectedFault;
static volatile sig_atomic_t sFaults;
static void* sGuard;


static int32
CheckClock(void* cookie)
{
	Worker& worker = *(Worker*)cookie;
	uint32 mask[32] = {};
	mask[worker.cpu / 32] = 1U << (worker.cpu % 32);
	worker.affinity = _kern_set_thread_affinity(0, mask, sizeof(mask));
	if (worker.affinity != B_OK)
		return worker.affinity;
	if (acquire_sem(sStart) != B_OK)
		return B_ERROR;
	while (system_time() < sDeadline) {
		if (_kern_get_cpu() != (int)worker.cpu)
			worker.wrongCpu++;
		pthread_mutex_lock(&sClockLock);
		bigtime_t now = system_time();
		if (now < sLastTime)
			sBackwards++;
		sLastTime = now;
		pthread_mutex_unlock(&sClockLock);
		worker.samples++;
		snooze(50);
	}
	return B_OK;
}


static void
FaultHandler(int signal, siginfo_t* info, void*)
{
	if (signal != SIGSEGV || !sExpectedFault || info->si_addr != sGuard)
		_exit(128 + signal);
	sExpectedFault = 0;
	sFaults++;
	siglongjmp(sFaultReturn, 1);
}


int
main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "--child") == 0)
		return atoi(argv[2]);
	unsigned seconds = argc == 2 ? atoi(argv[1]) : 5;
	if (argc > 2 || seconds < 1 || seconds > 30) {
		fprintf(stderr, "Usage: %s [seconds (1..30)]\n", argv[0]);
		return 2;
	}
	system_info info;
	if (get_system_info(&info) != B_OK || info.cpu_count < 1 || info.cpu_count > 64)
		return 1;
	alarm(60);
	printf("ROCK5_PLATFORM_BEGIN cpus=%" B_PRIu32 " seconds=%u forks=32\n",
		info.cpu_count, seconds);
	fflush(stdout);
	sStart = create_sem(0, "platform probe start");
	if (sStart < B_OK)
		return 1;
	Worker workers[64] = {};
	thread_id threads[64];
	unsigned created = 0;
	for (; created < info.cpu_count; created++) {
		workers[created].cpu = created;
		threads[created] = spawn_thread(CheckClock, "pinned clock probe",
			B_NORMAL_PRIORITY, &workers[created]);
		if (threads[created] < B_OK)
			break;
	}
	bool passed = created == info.cpu_count;
	sDeadline = system_time() + seconds * 1000000LL;
	for (unsigned i = 0; i < created; i++)
		resume_thread(threads[i]);
	if (created > 0)
		release_sem_etc(sStart, created, 0);

	// Fork while the other threads are running. The child touches a private
	// page, then immediately execs; it does not acquire inherited user locks.
	volatile uint8* page = (volatile uint8*)mmap(NULL, B_PAGE_SIZE,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	unsigned completed = 0;
	if ((void*)page == MAP_FAILED)
		passed = false;
	else {
		page[0] = 0x55;
		for (unsigned i = 0; i < 32; i++) {
			char value[16];
			unsigned expected = (i * 7 + 3) % 64;
			snprintf(value, sizeof(value), "%u", expected);
			pid_t child = fork();
			if (child == 0) {
				page[0] = 0xaa;
				execl(argv[0], argv[0], "--child", value, NULL);
				_exit(127);
			}
			int status;
			if (child < 0 || waitpid(child, &status, 0) != child
				|| !WIFEXITED(status) || WEXITSTATUS(status) != (int)expected
				|| page[0] != 0x55) {
				passed = false;
				break;
			}
			completed++;
		}
		munmap((void*)page, B_PAGE_SIZE);
	}
	printf("ROCK5_FORKEXEC completed=%u expected=32\n", completed);
	for (unsigned i = 0; i < created; i++) {
		status_t status;
		if (wait_for_thread(threads[i], &status) != B_OK || status != B_OK
			|| workers[i].samples == 0 || workers[i].wrongCpu != 0)
			passed = false;
		printf("CPU %u affinity=%" B_PRId32 " samples=%" B_PRIu64
			" wrong_cpu=%" B_PRIu64 "\n", i, workers[i].affinity,
			workers[i].samples, workers[i].wrongCpu);
	}
	delete_sem(sStart);
	printf("ROCK5_CLOCK backwards=%" B_PRIu64 "\n", sBackwards);
	passed &= sBackwards == 0;

	// All worker threads have exited before using the signal return context.
	sGuard = mmap(NULL, B_PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	struct sigaction action = {}, previous;
	action.sa_sigaction = FaultHandler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	if (sGuard == MAP_FAILED || sigaction(SIGSEGV, &action, &previous) != 0)
		passed = false;
	else {
		for (unsigned i = 0; i < 8; i++) {
			if (sigsetjmp(sFaultReturn, 1) == 0) {
				sExpectedFault = 1;
				*(volatile uint8*)sGuard = 0;
				sExpectedFault = 0;
				passed = false;
				break;
			}
			if (mprotect(sGuard, B_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
				passed = false;
				break;
			}
			*(volatile uint8*)sGuard = i;
			if (mprotect(sGuard, B_PAGE_SIZE, PROT_NONE) != 0) {
				passed = false;
				break;
			}
		}
		sigaction(SIGSEGV, &previous, NULL);
		munmap(sGuard, B_PAGE_SIZE);
	}
	passed &= sFaults == 8;
	printf("ROCK5_FAULTS recovered=%d expected=8\n", (int)sFaults);
	alarm(0);
	printf("ROCK5_PLATFORM_%s\n", passed ? "PASS" : "FAIL");
	return passed ? 0 : 1;
}
