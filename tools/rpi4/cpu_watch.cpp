/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Sample total CPU utilization and the busiest threads during a lab workload.
#include <OS.h>

#include <algorithm>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

struct Sample {
	thread_info info;
	bigtime_t elapsed;
};

static std::map<thread_id, thread_info>
Threads()
{
	std::map<thread_id, thread_info> result;
	int32 teamCookie = 0;
	team_info team;
	while (get_next_team_info(&teamCookie, &team) == B_OK) {
		int32 cookie = 0;
		thread_info thread;
		while (get_next_thread_info(team.team, &cookie, &thread) == B_OK)
			result[thread.thread] = thread;
	}
	return result;
}

int
main(int argc, char** argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 30;
	if (seconds < 1 || seconds > 3600)
		return 2;
	system_info system;
	if (get_system_info(&system) != B_OK)
		return 1;
	std::vector<cpu_info> previous(system.cpu_count), current(system.cpu_count);
	if (get_cpu_info(0, system.cpu_count, previous.data()) != B_OK)
		return 1;
	auto threads = Threads();
	bigtime_t last = system_time();
	for (int second = 1; second <= seconds; second++) {
		snooze_until(last + 1000000, B_SYSTEM_TIMEBASE);
		if (get_cpu_info(0, system.cpu_count, current.data()) != B_OK)
			return 1;
		bigtime_t now = system_time();
		double elapsed = now - last;
		bigtime_t active = 0;
		printf("CPU second=%d", second);
		for (uint32 cpu = 0; cpu < system.cpu_count; cpu++) {
			bigtime_t delta = current[cpu].active_time - previous[cpu].active_time;
			active += delta;
			printf(" cpu%u=%.2f", cpu, 100.0 * delta / elapsed);
		}
		printf(" total=%.2f cores=%.3f\n", 100.0 * active / elapsed / system.cpu_count,
			active / elapsed);
		auto next = Threads();
		std::vector<Sample> busy;
		for (const auto& item : next) {
			auto old = threads.find(item.first);
			if (old == threads.end())
				continue;
			const thread_info& info = item.second;
			if (info.team == B_SYSTEM_TEAM && info.priority == B_IDLE_PRIORITY)
				continue;
			bigtime_t delta = info.user_time + info.kernel_time
				- old->second.user_time - old->second.kernel_time;
			if (delta > 1000)
				busy.push_back({info, delta});
		}
		std::sort(busy.begin(), busy.end(), [](const Sample& a, const Sample& b) {
			return a.elapsed > b.elapsed;
		});
		for (size_t i = 0; i < std::min(busy.size(), size_t(8)); i++) {
			printf("THREAD second=%d id=%ld team=%ld pct=%.2f name=%s\n", second,
				(long)busy[i].info.thread, (long)busy[i].info.team,
				100.0 * busy[i].elapsed / elapsed, busy[i].info.name);
		}
		fflush(stdout);
		threads.swap(next);
		previous.swap(current);
		last = now;
	}
	return 0;
}
