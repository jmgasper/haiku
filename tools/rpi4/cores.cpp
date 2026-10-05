/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_cores
// How much arithmetic one to four threads get done in a second, left to the
// scheduler and with each thread kept on a processor of its own: the two
// should not differ. (They did: four threads got three cores' worth.)


#include <stdint.h>
#include <stdio.h>
#include <thread>
#include <vector>

#include <OS.h>


extern "C" status_t _kern_set_thread_affinity(thread_id id,
	const void* userMask, size_t size);

static volatile uint64_t sSink;


static double
spin(bigtime_t duration)
{
	bigtime_t start = system_time();
	uint64_t rounds = 0;
	uint64_t x = 88172645463325252ull;
	while (system_time() - start < duration) {
		for (int i = 0; i < 10000; i++) {
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
		}
		rounds++;
	}
	sSink = x;
	return rounds * 1e6 / duration;
}


int
main()
{
	system_info info;
	get_system_info(&info);
	int cpus = (int)info.cpu_count;

	for (int count = 1; count <= cpus; count++) {
		for (int pinned = 0; pinned < 2; pinned++) {
			std::vector<double> rates(count);
			std::vector<std::thread> threads;
			for (int t = 0; t < count; t++) {
				threads.emplace_back([&, t]() {
					if (pinned != 0) {
						uint32 mask[2] = {1u << t, 0};
						_kern_set_thread_affinity(find_thread(NULL), mask,
							sizeof(mask));
					}
					rates[t] = spin(1000000);
				});
			}
			for (std::thread& thread : threads)
				thread.join();

			double sum = 0;
			printf("%d %s threads:", count, pinned != 0 ? "pinned" : "free");
			for (double rate : rates) {
				printf(" %.0f", rate);
				sum += rate;
			}
			printf(" = %.0f\n", sum);
		}
	}
	return 0;
}
