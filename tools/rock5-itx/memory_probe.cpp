/*
 * Memory integrity check for the dedicated ROCK 5 lab.
 * Distributed under the terms of the MIT License.
 */

#include <OS.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


struct Probe;

struct Worker {
	Probe* probe;
	unsigned index;
	bool mismatch;
	uint64 badIndex;
	uint64 expected;
	uint64 actual;
};

struct Probe {
	volatile uint64* memory;
	uint64 words;
	unsigned workers;
	unsigned passes;
	sem_id start;
	bool abort;
	bool injectError;
	pthread_barrier_t barrier;
};


static uint64
Pattern(uint64 index, unsigned pass)
{
	uint64 value = index * UINT64_C(0x9e3779b97f4a7c15)
		^ (pass + 1) * UINT64_C(0xd1b54a32d192ed03);
	return value ^ (value >> 29);
}


static int32
RunWorker(void* cookie)
{
	Worker& worker = *(Worker*)cookie;
	Probe& probe = *worker.probe;
	if (acquire_sem(probe.start) != B_OK || probe.abort)
		return B_ERROR;

	for (unsigned pass = 0; pass < probe.passes; pass++) {
		uint64 begin = probe.words * worker.index / probe.workers;
		uint64 end = probe.words * (worker.index + 1) / probe.workers;
		for (uint64 index = begin; index < end; index++)
			probe.memory[index] = Pattern(index, pass);
		if (probe.injectError && worker.index == 0 && pass == 0)
			probe.memory[0] ^= 1;
		pthread_barrier_wait(&probe.barrier);

		// Read a different worker's range after all writers have finished.
		unsigned owner = (worker.index + 1) % probe.workers;
		begin = probe.words * owner / probe.workers;
		end = probe.words * (owner + 1) / probe.workers;
		for (uint64 index = begin; index < end; index++) {
			uint64 actual = probe.memory[index];
			uint64 expected = Pattern(index, pass);
			if (actual != expected && !worker.mismatch) {
				worker.mismatch = true;
				worker.badIndex = index;
				worker.expected = expected;
				worker.actual = actual;
			}
		}
		pthread_barrier_wait(&probe.barrier);
	}
	return B_OK;
}


static bool
ParseNumber(const char* text, uint64& value)
{
	char* end;
	errno = 0;
	value = strtoull(text, &end, 10);
	return errno == 0 && end != text && *end == '\0' && text[0] != '-';
}


int
main(int argc, char** argv)
{
	system_info info;
	if (get_system_info(&info) != B_OK)
		return 1;
	uint64 mib = 64;
	uint64 workers = info.cpu_count;
	uint64 passes = 2;
	if (argc > 5 || (argc > 1 && !ParseNumber(argv[1], mib))
		|| (argc > 2 && !ParseNumber(argv[2], workers))
		|| (argc > 3 && !ParseNumber(argv[3], passes)) || mib < 1 || mib > 12288
		|| workers < 1 || workers > 64 || passes < 1 || passes > 8
		|| (argc == 5 && strcmp(argv[4], "--inject-error") != 0)) {
		fprintf(stderr, "Usage: %s [MiB (1..12288)] [workers (1..64)] [passes (1..8)]"
			" [--inject-error]\n",
			argv[0]);
		return 2;
	}
	uint64 bytes = mib * 1024 * 1024;
	uint64 freeBytes = (info.max_pages - info.used_pages) * B_PAGE_SIZE;
	if (bytes > freeBytes / 4 * 3 || bytes > SIZE_MAX) {
		fprintf(stderr, "Requested memory exceeds 75%% of presently free RAM.\n");
		return 2;
	}

	// This is an explicit lab test. Lock its pages so a >4 GiB run cannot
	// merely cycle a small set of physical pages through swap.
	void* memory = NULL;
	area_id area = create_area("ROCK memory probe", &memory, B_ANY_ADDRESS,
		(size_t)bytes, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < B_OK) {
		fprintf(stderr, "create_area: %s\n", strerror(area));
		return 1;
	}
	Probe probe = {};
	probe.memory = (volatile uint64*)memory;
	probe.words = bytes / sizeof(uint64);
	probe.workers = workers;
	probe.passes = passes;
	probe.injectError = argc == 5;
	probe.start = create_sem(0, "ROCK memory probe start");
	if (probe.start < B_OK
		|| pthread_barrier_init(&probe.barrier, NULL, workers) != 0) {
		delete_sem(probe.start);
		delete_area(area);
		return 1;
	}
	Worker state[64] = {};
	thread_id threads[64];
	cpu_info before[64], after[64];
	unsigned cpuCount = info.cpu_count < 64 ? info.cpu_count : 64;
	bool cpuInfo = get_cpu_info(0, cpuCount, before) == B_OK;
	unsigned created = 0;
	for (; created < workers; created++) {
		state[created].probe = &probe;
		state[created].index = created;
		threads[created] = spawn_thread(RunWorker, "ROCK memory worker",
			B_NORMAL_PRIORITY, &state[created]);
		if (threads[created] < B_OK)
			break;
	}
	probe.abort = created != workers;
	printf("ROCK5_MEMORY_BEGIN bytes=%" PRIu64 " workers=%" PRIu64
		" passes=%" PRIu64 " locked=yes cpus=%" PRIu32 "\n",
		bytes, workers, passes, info.cpu_count);
	fflush(stdout);
	alarm(120);
	bigtime_t start = system_time();
	for (unsigned index = 0; index < created; index++)
		resume_thread(threads[index]);
	if (created > 0)
		release_sem_etc(probe.start, created, 0);
	bool passed = !probe.abort;
	for (unsigned index = 0; index < created; index++) {
		status_t status = B_ERROR;
		if (wait_for_thread(threads[index], &status) != B_OK || status != B_OK)
			passed = false;
		if (state[index].mismatch) {
			passed = false;
			printf("MISMATCH word=%" PRIu64 " expected=%016" PRIx64
				" actual=%016" PRIx64 "\n", state[index].badIndex,
				state[index].expected, state[index].actual);
		}
	}
	bigtime_t elapsed = system_time() - start;
	alarm(0);
	if (cpuInfo && get_cpu_info(0, cpuCount, after) == B_OK) {
		for (unsigned index = 0; index < cpuCount; index++) {
			printf("CPU %u enabled=%d active_us=%" PRId64 "\n", index,
				after[index].enabled, after[index].active_time - before[index].active_time);
		}
	}
	pthread_barrier_destroy(&probe.barrier);
	delete_sem(probe.start);
	delete_area(area);
	printf("ROCK5_MEMORY_%s bytes=%" PRIu64 " elapsed_us=%" PRId64 "\n",
		passed ? "PASS" : "FAIL", bytes, elapsed);
	return passed ? 0 : 1;
}
