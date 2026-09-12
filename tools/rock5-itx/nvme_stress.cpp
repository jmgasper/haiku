/*
 * Bounded concurrent I/O diagnostic for the dedicated ROCK 5 lab.
 * Distributed under the terms of the MIT License.
 */

#ifdef __HAIKU__
#include <Drivers.h>
#include <sys/ioctl.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


static const uint64_t kMiB = 1024 * 1024;
static const unsigned kMaxWorkers = 8;

struct Probe;

struct Worker {
	Probe* probe;
	unsigned index;
	uint64_t* buffer;
	bool failed;
};

struct Probe {
	int fd;
	uint64_t offset;
	uint64_t bytesPerWorker;
	unsigned workers;
	unsigned rounds;
	bool write;
	bool start;
	bool abort;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	pthread_barrier_t barrier;
	Worker state[kMaxWorkers];
};


static uint64_t
Microseconds()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return uint64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}


static uint64_t
Pattern(uint64_t wordOffset, unsigned round)
{
	uint64_t value = wordOffset * UINT64_C(0x9e3779b97f4a7c15)
		^ uint64_t(round + 1) * UINT64_C(0xd1b54a32d192ed03);
	return value ^ (value >> 29);
}


static bool
Transfer(Worker& worker, bool write, uint64_t offset)
{
	size_t done = 0;
	while (done < kMiB) {
		void* buffer = (char*)worker.buffer + done;
		ssize_t count = write
			? pwrite(worker.probe->fd, buffer, kMiB - done, offset + done)
			: pread(worker.probe->fd, buffer, kMiB - done, offset + done);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			fprintf(stderr, "worker=%u %s offset=%" PRIu64 ": %s\n",
				worker.index, write ? "write" : "read", offset + done,
				count < 0 ? strerror(errno) : "unexpected end of device");
			worker.failed = true;
			return false;
		}
		done += count;
	}
	return true;
}


static void
WriteRegion(Worker& worker, unsigned round)
{
	Probe& probe = *worker.probe;
	uint64_t start = probe.offset + worker.index * probe.bytesPerWorker;
	for (uint64_t position = 0; position < probe.bytesPerWorker; position += kMiB) {
		uint64_t offset = start + position;
		for (uint64_t word = 0; word < kMiB / 8; word++)
			worker.buffer[word] = Pattern(offset / 8 + word, round);
		if (!Transfer(worker, true, offset))
			return;
	}
}


static void
ReadRegion(Worker& worker, unsigned round)
{
	Probe& probe = *worker.probe;
	// Each worker verifies another worker's range, in reverse block order.
	unsigned owner = (worker.index + 1) % probe.workers;
	uint64_t start = probe.offset + owner * probe.bytesPerWorker;
	for (uint64_t remaining = probe.bytesPerWorker; remaining != 0; remaining -= kMiB) {
		uint64_t offset = start + remaining - kMiB;
		memset(worker.buffer, 0xa5, kMiB);
		if (!Transfer(worker, false, offset))
			return;
		for (uint64_t word = 0; word < kMiB / 8; word++) {
			uint64_t expected = Pattern(offset / 8 + word, round);
			if (worker.buffer[word] != expected) {
				fprintf(stderr, "MISMATCH worker=%u offset=%" PRIu64
					" expected=%016" PRIx64 " actual=%016" PRIx64 "\n",
					worker.index, offset + word * 8, expected, worker.buffer[word]);
				worker.failed = true;
				return;
			}
		}
	}
}


static void
CheckWorkers(Probe& probe)
{
	for (unsigned index = 0; index < probe.workers; index++)
		probe.abort |= probe.state[index].failed;
}


static void*
RunWorker(void* cookie)
{
	Worker& worker = *(Worker*)cookie;
	Probe& probe = *worker.probe;
	pthread_mutex_lock(&probe.mutex);
	while (!probe.start)
		pthread_cond_wait(&probe.condition, &probe.mutex);
	bool abort = probe.abort;
	pthread_mutex_unlock(&probe.mutex);
	if (abort)
		return NULL;

	unsigned firstRound = probe.write ? 0 : probe.rounds - 1;
	for (unsigned round = firstRound; round < probe.rounds; round++) {
		uint64_t start = Microseconds();
		if (probe.write) {
			WriteRegion(worker, round);
			pthread_barrier_wait(&probe.barrier);
			if (worker.index == 0) {
				if (fsync(probe.fd) != 0) {
					perror("fsync");
					worker.failed = true;
				}
				CheckWorkers(probe);
				printf("ROCK5_NVME_STRESS_WRITE round=%u bytes=%" PRIu64
					" elapsed_us=%" PRIu64 " status=%s\n", round + 1,
					probe.bytesPerWorker * probe.workers, Microseconds() - start,
					probe.abort ? "fail" : "pass");
				fflush(stdout);
			}
			pthread_barrier_wait(&probe.barrier);
		}
		start = Microseconds();
		if (!probe.abort)
			ReadRegion(worker, round);
		pthread_barrier_wait(&probe.barrier);
		if (worker.index == 0) {
			CheckWorkers(probe);
			printf("ROCK5_NVME_STRESS_READ round=%u bytes=%" PRIu64
				" elapsed_us=%" PRIu64 " status=%s\n", round + 1,
				probe.bytesPerWorker * probe.workers, Microseconds() - start,
				probe.abort ? "fail" : "pass");
			fflush(stdout);
		}
		pthread_barrier_wait(&probe.barrier);
		if (probe.abort)
			break;
	}
	return NULL;
}


static bool
Number(const char* text, uint64_t& value)
{
	char* end;
	errno = 0;
	value = strtoull(text, &end, 10);
	return errno == 0 && end != text && *end == '\0' && text[0] != '-';
}


int
main(int argc, char** argv)
{
	uint64_t offsetMiB, regionMiB, workers, rounds;
	if (argc != 7 || (strcmp(argv[1], "write") && strcmp(argv[1], "verify"))
		|| !Number(argv[3], offsetMiB) || offsetMiB > 8 * 1024 * 1024
		|| !Number(argv[4], regionMiB) || regionMiB < 1 || regionMiB > 8192
		|| !Number(argv[5], workers) || workers < 1 || workers > kMaxWorkers
		|| regionMiB % workers != 0
		|| !Number(argv[6], rounds) || rounds < 1 || rounds > 32) {
		fprintf(stderr, "Usage: %s write|verify PATH OFFSET_MiB REGION_MiB WORKERS ROUNDS\n"
			"Writes destroy exactly the chosen region. REGION must divide evenly\n"
			"among 1..8 workers. Verify reads only the final round's pattern.\n"
			"Only existing regular files or the lab NVMe raw namespace are accepted.\n",
			argv[0]);
		return 2;
	}
	Probe probe = {};
	probe.write = strcmp(argv[1], "write") == 0;
	probe.fd = open(argv[2], probe.write ? O_RDWR : O_RDONLY);
	if (probe.fd < 0) {
		perror("open");
		return 1;
	}
	struct stat st;
	if (fstat(probe.fd, &st) != 0)
		return 1;
	uint64_t capacity = S_ISREG(st.st_mode) ? st.st_size : 0;
#ifdef __HAIKU__
	if (!S_ISREG(st.st_mode) && strcmp(argv[2], "/dev/disk/nvme/0/raw") == 0) {
		device_geometry geometry;
		size_t deviceBytes;
		if (ioctl(probe.fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) == 0
			&& geometry.bytes_per_sector == 512 && !geometry.read_only
			&& ioctl(probe.fd, B_GET_DEVICE_SIZE, &deviceBytes, sizeof(deviceBytes)) == 0) {
			capacity = deviceBytes;
		}
	}
#endif
	probe.offset = offsetMiB * kMiB;
	uint64_t bytes = regionMiB * kMiB;
	if (probe.offset > capacity || bytes > capacity - probe.offset) {
		fprintf(stderr, "Unsupported target or requested range exceeds capacity.\n");
		close(probe.fd);
		return 2;
	}
	probe.workers = workers;
	probe.rounds = rounds;
	probe.bytesPerWorker = bytes / workers;
	if (pthread_mutex_init(&probe.mutex, NULL) != 0
		|| pthread_cond_init(&probe.condition, NULL) != 0
		|| pthread_barrier_init(&probe.barrier, NULL, workers) != 0)
		return 1;
	pthread_t threads[kMaxWorkers];
	unsigned created = 0;
	for (; created < workers; created++) {
		Worker& worker = probe.state[created];
		worker.probe = &probe;
		worker.index = created;
		if (posix_memalign((void**)&worker.buffer, 4096, kMiB) != 0
			|| pthread_create(&threads[created], NULL, RunWorker, &worker) != 0)
			break;
	}
	printf("ROCK5_NVME_STRESS_BEGIN mode=%s offset=%" PRIu64 " bytes=%" PRIu64
		" workers=%u rounds=%u block_bytes=%" PRIu64 "\n",
		argv[1], probe.offset, bytes, probe.workers, probe.rounds, kMiB);
	fflush(stdout);
	alarm(600);
	uint64_t start = Microseconds();
	pthread_mutex_lock(&probe.mutex);
	probe.abort = created != workers;
	probe.start = true;
	pthread_cond_broadcast(&probe.condition);
	pthread_mutex_unlock(&probe.mutex);
	bool joinFailed = false;
	for (unsigned index = 0; index < created; index++)
		joinFailed |= pthread_join(threads[index], NULL) != 0;
	if (joinFailed)
		return 1;
	alarm(0);
	for (unsigned index = 0; index < workers; index++)
		free(probe.state[index].buffer);
	pthread_barrier_destroy(&probe.barrier);
	pthread_cond_destroy(&probe.condition);
	pthread_mutex_destroy(&probe.mutex);
	close(probe.fd);
	printf("ROCK5_NVME_STRESS_%s elapsed_us=%" PRIu64 "\n",
		probe.abort ? "FAIL" : "PASS", Microseconds() - start);
	return probe.abort ? 1 : 0;
}
