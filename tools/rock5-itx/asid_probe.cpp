/* Bounded live-process private-memory checks. Distributed under the MIT License. */
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __HAIKU__
#include <OS.h>
#include <syscalls.h>
#else
#include <sched.h>
#endif

static constexpr unsigned kMaximumChildren = 400;
static constexpr unsigned kMaximumCPUs = 8;
static volatile sig_atomic_t sInterrupted;
static uint64_t sDeadline;
static unsigned sCPUs, sCPUIds[kMaximumCPUs];
static size_t sPageBytes;
static volatile uint64_t* sPrivate;
static int sGates[kMaximumChildren];
static pid_t sChildren[kMaximumChildren];

static uint64_t Now()
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) _exit(90);
	return uint64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}
static bool Active() { return !sInterrupted && Now() < sDeadline; }
static void Interrupted(int) { sInterrupted = 1; }
static bool DiscoverCPUs()
{
#ifdef __HAIKU__
	system_info info;
	if (get_system_info(&info) != B_OK) return false;
	sCPUs = std::min<unsigned>(info.cpu_count, kMaximumCPUs);
	for (unsigned i = 0; i < sCPUs; ++i) sCPUIds[i] = i;
#else
	cpu_set_t allowed;
	if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
	for (unsigned i = 0; i < CPU_SETSIZE && sCPUs < kMaximumCPUs; ++i)
		if (CPU_ISSET(i, &allowed)) sCPUIds[sCPUs++] = i;
#endif
	return sCPUs != 0;
}
static bool Pin(unsigned ordinal)
{
	unsigned cpu = sCPUIds[ordinal % sCPUs];
#ifdef __HAIKU__
	uint32_t mask[32] = {};
	mask[cpu / 32] = 1U << (cpu % 32);
	return _kern_set_thread_affinity(0, mask, sizeof(mask)) == B_OK
		&& _kern_get_cpu() == int(cpu);
#else
	cpu_set_t mask;
	CPU_ZERO(&mask); CPU_SET(cpu, &mask);
	return sched_setaffinity(0, sizeof(mask), &mask) == 0 && sched_getcpu() == int(cpu);
#endif
}
static uint64_t Pattern(unsigned child, unsigned round, unsigned word)
{
	return 0x5a69c30784d12be0ULL ^ (uint64_t(child + 1) * 0x9e3779b97f4a7c15ULL)
		^ (uint64_t(round + 1) * 0xd1b54a32d192ed03ULL) ^ (uint64_t(word) * 0x100000001b3ULL);
}
static void Fill(unsigned child, unsigned round)
{
	for (unsigned i = 0; i < sPageBytes / sizeof(uint64_t); ++i)
		sPrivate[i] = Pattern(child, round, i);
}
static bool Check(unsigned child, unsigned round)
{
	for (unsigned i = 0; i < sPageBytes / sizeof(uint64_t); ++i)
		if (sPrivate[i] != Pattern(child, round, i)) return false;
	return true;
}
struct Reply {
	uint32_t child, round, good, reserved;
	uint64_t address;
};
static bool ReplyToParent(int fd, unsigned child, unsigned round, bool good)
{
	Reply reply = {child, round, good ? 1U : 0U, 0, uint64_t(uintptr_t(sPrivate))};
	static_assert(sizeof(Reply) <= 512, "shared pipe messages must be atomic");
	ssize_t bytes;
	do { bytes = write(fd, &reply, sizeof(reply)); } while (bytes < 0 && errno == EINTR);
	return bytes == ssize_t(sizeof(reply));
}
static void Child(unsigned index, int gate, int reply, unsigned corrupt)
{
	signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGALRM, SIG_DFL);
	alarm(100);
	bool good = Pin(index);
	Fill(index, 0);
	if (!ReplyToParent(reply, index, 0, good && Check(index, 0))) _exit(2);
	unsigned previous = 0;
	for (;;) {
		unsigned char command;
		ssize_t bytes;
		do { bytes = read(gate, &command, 1); } while (bytes < 0 && errno == EINTR);
		if (bytes != 1) _exit(3);
		if (command == 255) _exit(0);
		good = command == previous + 1 && Pin(index + command);
		if (index == corrupt && command == 1) sPrivate[17] ^= 1;
		good &= Check(index, previous);
		if (good) Fill(index, command);
		if (!ReplyToParent(reply, index, command, good && Check(index, command))) _exit(4);
		if (!good) _exit(5);
		previous = command;
	}
}
static bool ReceiveRound(int fd, unsigned count, unsigned round)
{
	bool seen[kMaximumChildren] = {};
	for (unsigned done = 0; done < count; ++done) {
		Reply reply = {};
		size_t have = 0;
		while (have < sizeof(reply) && Active()) {
			pollfd wait = {fd, POLLIN, 0};
			int ready = poll(&wait, 1, 200);
			if (ready < 0 && errno == EINTR) continue;
			if (ready < 0) return false;
			if (ready == 0) continue;
			ssize_t bytes = read(fd, (char*)&reply + have, sizeof(reply) - have);
			if (bytes < 0 && errno == EINTR) continue;
			if (bytes <= 0) return false;
			have += bytes;
		}
		if (have != sizeof(reply) || reply.child >= count || seen[reply.child]
			|| reply.round != round || reply.good != 1 || reply.reserved != 0
			|| reply.address != uint64_t(uintptr_t(sPrivate))) return false;
		seen[reply.child] = true;
	}
	return true;
}

static unsigned Reap(unsigned count, unsigned& successful, bool& good)
{
	unsigned remaining = 0;
	for (unsigned i = 0; i < count; ++i) {
		if (sChildren[i] == 0) continue;
		int status = 0;
		pid_t result = waitpid(sChildren[i], &status, WNOHANG);
		if (result == sChildren[i]) {
			sChildren[i] = 0;
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0) ++successful;
			else {
				printf("ROCK5_ASID_CHILD_EXIT child=%u pid=%ld status=%#x exited=%d code=%d signal=%d\n",
					i, (long)result, status, WIFEXITED(status),
					WIFEXITED(status) ? WEXITSTATUS(status) : -1,
					WIFSIGNALED(status) ? WTERMSIG(status) : 0);
				good = false;
			}
		} else if (result < 0 && errno != EINTR) {
			printf("ROCK5_ASID_CHILD_WAIT_ERROR child=%u pid=%ld errno=%d\n",
				i, (long)sChildren[i], errno);
			sChildren[i] = 0;
			good = false;
		} else ++remaining;
	}
	return remaining;
}
int main(int argc, char** argv)
{
	unsigned count = 280, rounds = 4, corrupt = kMaximumChildren;
	for (int i = 1; i < argc; i += 2) {
		if (i + 1 >= argc) return 2;
		char* end;
		unsigned long value = strtoul(argv[i + 1], &end, 10);
		if (!argv[i + 1][0] || *end || value > 1000) return 2;
		if (strcmp(argv[i], "--children") == 0) count = value;
		else if (strcmp(argv[i], "--rounds") == 0) rounds = value;
		else if (strcmp(argv[i], "--corrupt-child") == 0) corrupt = value;
		else return 2;
	}
	if (count < 1 || count > kMaximumChildren || rounds < 1 || rounds > 16
		|| (corrupt != kMaximumChildren && corrupt >= count)) return 2;
	if (!DiscoverCPUs()) return 1;
	struct rlimit limit;
	if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return 1;
	rlim_t required = count + 32;
	if (limit.rlim_cur < required) {
		if (limit.rlim_max < required) return 1;
		limit.rlim_cur = required;
		if (setrlimit(RLIMIT_NOFILE, &limit) != 0) return 1;
	}
	sPageBytes = size_t(sysconf(_SC_PAGESIZE));
	if (sPageBytes < 1024 || sPageBytes > 65536) return 1;
	void* mapping = mmap(nullptr, 3 * sPageBytes, PROT_NONE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mapping == MAP_FAILED) return 1;
	sPrivate = (volatile uint64_t*)((char*)mapping + sPageBytes);
	if (mprotect((void*)sPrivate, sPageBytes, PROT_READ | PROT_WRITE) != 0) return 1;
	Fill(kMaximumChildren + 1, 0);
	int replies[2];
	if (pipe(replies) != 0) return 1;
	std::fill_n(sGates, kMaximumChildren, -1);
	signal(SIGTERM, Interrupted); signal(SIGINT, Interrupted); signal(SIGALRM, Interrupted);
	signal(SIGPIPE, SIG_IGN);
	uint64_t started = Now();
	sDeadline = started + 90000000;
	alarm(90);
	printf("ROCK5_ASID_POOL_BEGIN children=%u rounds=%u cpus=%u page_bytes=%zu private_va=%p\n",
		count, rounds, sCPUs, sPageBytes, (void*)sPrivate);
	fflush(stdout);
	unsigned created = 0;
	for (; created < count && Active(); ++created) {
		int gate[2];
		if (pipe(gate) != 0) break;
		sGates[created] = gate[1];
		pid_t pid = fork();
		if (pid == 0) {
			close(replies[0]);
			for (unsigned i = 0; i <= created; ++i) close(sGates[i]);
			Child(created, gate[0], replies[1], corrupt);
			_exit(99);
		}
		close(gate[0]);
		if (pid < 0) { close(gate[1]); sGates[created] = -1; break; }
		sChildren[created] = pid;
	}
	close(replies[1]);
	bool good = created == count && ReceiveRound(replies[0], count, 0)
		&& Check(kMaximumChildren + 1, 0);
	printf("ROCK5_ASID_POOL_READY created=%u expected=%u ready=%u\n", created, count, good ? count : 0);
	fflush(stdout);
	unsigned completed = 0;
	for (unsigned round = 1; good && round <= rounds && Active(); ++round) {
		unsigned char command = round;
		for (unsigned i = 0; i < count; ++i) {
			ssize_t bytes;
			do { bytes = write(sGates[i], &command, 1); } while (bytes < 0 && errno == EINTR && Active());
			if (bytes != 1) { good = false; break; }
		}
		good = good && ReceiveRound(replies[0], count, round) && Check(kMaximumChildren + 1, 0);
		if (good) ++completed;
		printf("ROCK5_ASID_POOL_ROUND round=%u verified=%u expected=%u\n", round, good ? count : 0, count);
		fflush(stdout);
	}
	good &= completed == rounds && Active();
	printf("ROCK5_ASID_POOL_STOP_BEGIN good=%d elapsed_us=%" PRIu64 "\n",
		good, Now() - started);
	fflush(stdout);
	for (unsigned i = 0; i < created; ++i) {
		if (good) {
			unsigned char command = 255;
			if (write(sGates[i], &command, 1) != 1) {
				printf("ROCK5_ASID_CHILD_STOP_ERROR child=%u errno=%d\n", i, errno);
				good = false;
			}
		}
		close(sGates[i]);
		if (!good) kill(sChildren[i], SIGTERM);
	}
	unsigned successful = 0, remaining = created;
	uint64_t deadline = Now() + 5000000;
	while (remaining && Now() < deadline) {
		remaining = Reap(created, successful, good);
		if (remaining) usleep(1000);
	}
	if (remaining) {
		printf("ROCK5_ASID_POOL_REAP_DEADLINE remaining=%u elapsed_us=%" PRIu64 "\n",
			remaining, Now() - started);
		fflush(stdout);
		good = false;
		for (unsigned i = 0; i < created; ++i) if (sChildren[i] > 0) kill(sChildren[i], SIGKILL);
		deadline = Now() + 5000000;
		while (remaining && Now() < deadline) {
			remaining = Reap(created, successful, good);
			if (remaining) usleep(1000);
		}
	}
	good &= successful == count && remaining == 0;
	alarm(0);
	close(replies[0]);
	munmap(mapping, 3 * sPageBytes);
	printf("ROCK5_ASID_POOL_END children=%u rounds=%u completed=%u exited=%u remaining=%u\n",
		count, rounds, completed, successful, remaining);
	printf("ROCK5_ASID_POOL_%s\n", good ? "PASS" : "FAIL");
	return good ? 0 : 1;
}
