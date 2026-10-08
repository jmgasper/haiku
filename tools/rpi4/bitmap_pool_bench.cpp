/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Measure retained shared bitmap storage and allocation CPU. Keep a small
// bitmap alive while larger temporary bitmaps grow and reuse the same pool.

#include <Application.h>
#include <Bitmap.h>
#include <OS.h>
#include <image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>


static void
require(bool value, const char* message)
{
	if (!value) {
		fprintf(stderr, "BITMAP_POOL_FAIL %s\n", message);
		exit(1);
	}
}


static uint64
hash(const BBitmap& bitmap)
{
	const uint8* bytes = static_cast<const uint8*>(bitmap.Bits());
	uint64 value = 1469598103934665603ULL;
	for (size_t i = 0; i < bitmap.BitsLength(); i++) {
		value ^= bytes[i];
		value *= 1099511628211ULL;
	}
	return value;
}


static uint64
mapped_bytes()
{
	ssize_t cookie = 0;
	area_info info = {};
	uint64 total = 0;
	while (get_next_area_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (strcmp(info.name, "server_memory") == 0)
			total += info.size;
	}
	return total;
}


static void
fence()
{
	BBitmap bitmap(BRect(0, 0, 1, 1), B_RGB32);
	require(bitmap.InitCheck() == B_OK, "server fence");
}


static bigtime_t
client_cpu()
{
	team_usage_info info = {};
	require(get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &info) == B_OK,
		"client CPU");
	return info.user_time + info.kernel_time;
}


static team_id
server_team()
{
	int32 cookie = 0;
	team_info team = {};
	while (get_next_team_info(&cookie, &team) == B_OK) {
		if (strstr(team.args, "app_server") == NULL)
			continue;
		int32 imageCookie = 0;
		image_info image = {};
		while (get_next_image_info(team.team, &imageCookie, &image) == B_OK) {
			if (image.type == B_APP_IMAGE) {
				printf("APPSERVER team=%" B_PRId32 " path=%s\n", team.team,
					image.name);
				return team.team;
			}
		}
	}
	return -1;
}


static bigtime_t
server_cpu(team_id team)
{
	int32 cookie = 0;
	thread_info info = {};
	char prefix[32];
	snprintf(prefix, sizeof(prefix), "a:%" B_PRId32 ":", be_app->Team());
	while (get_next_thread_info(team, &cookie, &info) == B_OK) {
		if (strncmp(info.name, prefix, strlen(prefix)) == 0)
			return info.user_time + info.kernel_time;
	}
	require(false, "app_server client thread");
	return 0;
}


static void
fill_and_check(BBitmap& bitmap, uint8 value)
{
	memset(bitmap.Bits(), value, bitmap.BitsLength());
	const uint8* pixels = static_cast<const uint8*>(bitmap.Bits());
	for (size_t offset = 0; offset < bitmap.BitsLength(); offset += B_PAGE_SIZE)
		require(pixels[offset] == value, "page contents");
	require(pixels[bitmap.BitsLength() - 1] == value, "final byte");
}


static void
resize_workload(int rounds, bool swapFirst, team_id server)
{
	BBitmap anchor(BRect(0, 0, 31, 31), B_RGB32);
	require(anchor.InitCheck() == B_OK, "resize anchor");
	memset(anchor.Bits(), 0x6d, anchor.BitsLength());
	const uint64 anchorHash = hash(anchor);
	std::unique_ptr<BBitmap> previous;
	uint64 writes = 0;
	int bitmaps = 0;
	bigtime_t client = client_cpu();
	bigtime_t cpu = server_cpu(server);
	bigtime_t start = system_time();
	for (int round = 0; round < rounds; round++) {
		for (int width = 64; width <= 2048; width += 16) {
			if (!swapFirst)
				previous.reset();
			std::unique_ptr<BBitmap> bitmap(new BBitmap(
				BRect(0, 0, width - 1, width * 3 / 4 - 1), B_RGB32));
			require(bitmap->InitCheck() == B_OK, "resize bitmap");
			fill_and_check(*bitmap, 0x35 + round % 127);
			require(hash(anchor) == anchorHash, "resize anchor intact");
			writes += bitmap->BitsLength();
			bitmaps++;
			previous = std::move(bitmap);
		}
		previous.reset();
		fence();
	}
	bigtime_t wall = system_time() - start;
	cpu = server_cpu(server) - cpu;
	client = client_cpu() - client;
	area_info info = {};
	require(get_area_info(anchor.Area(), &info) == B_OK, "resize anchor area");
	printf("RESIZE swap_first=%d rounds=%d bitmaps=%d written_bytes=%" B_PRIu64
		" retained_mapped=%" B_PRIu64 " anchor_area_bytes=%zu"
		" anchor_ram_bytes=%" B_PRIu64
		" wall_ms=%.3f client_ms=%.3f server_ms=%.3f\n",
		int(swapFirst), rounds, bitmaps, writes, mapped_bytes(), info.size,
		uint64(info.ram_size), wall / 1000., client / 1000., cpu / 1000.);
	puts("BITMAP_RESIZE_PASS");
}


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 2;
	if (argc > 3 || rounds < 1 || rounds > 2000
		|| (argc == 3 && strcmp(argv[2], "free") != 0
			&& strcmp(argv[2], "swap") != 0)) {
		fprintf(stderr, "usage: %s [rounds 1..2000] [free|swap]\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	BApplication app("application/x-vnd.airOS-bitmap-pool-bench");
	team_id server = server_team();
	require(server >= 0, "app_server");
	if (argc == 3) {
		resize_workload(rounds, strcmp(argv[2], "swap") == 0, server);
		return 0;
	}

	printf("POOL_INITIAL mapped=%" B_PRIu64 "\n", mapped_bytes());
	std::unique_ptr<BBitmap> anchor(new BBitmap(BRect(0, 0, 31, 31), B_RGB32));
	require(anchor->InitCheck() == B_OK, "anchor");
	memset(anchor->Bits(), 0x6d, anchor->BitsLength());
	const uint64 anchorHash = hash(*anchor);
	printf("POOL_ANCHOR mapped=%" B_PRIu64 " area=%" B_PRId32
		" hash=%016" B_PRIx64 "\n", mapped_bytes(), anchor->Area(), anchorHash);
	const size_t sizes[] = {1 << 20, 4 << 20, 16 << 20, 64 << 20};
	for (size_t bytes : sizes) {
		uint64 peak = 0;
		bool shared = true;
		bigtime_t client = client_cpu();
		bigtime_t cpu = server_cpu(server);
		bigtime_t start = system_time();
		for (int round = 0; round < rounds; round++) {
			std::unique_ptr<BBitmap> temporary(new BBitmap(
				BRect(0, 0, 1023, int(bytes / 4096) - 1), B_RGB32));
			require(temporary->InitCheck() == B_OK
				&& temporary->BitsLength() == bytes, "temporary bitmap");
			fill_and_check(*temporary, 0x35 + round % 127);
			uint64 current = mapped_bytes();
			if (current > peak)
				peak = current;
			shared &= temporary->Area() == anchor->Area();
			temporary.reset();
			fence();
			require(hash(*anchor) == anchorHash, "live anchor survives free");
		}
		bigtime_t wall = system_time() - start;
		cpu = server_cpu(server) - cpu;
		client = client_cpu() - client;
		area_info info = {};
		require(get_area_info(anchor->Area(), &info) == B_OK, "anchor area");
		printf("POOL bytes=%zu rounds=%d shared=%d peak_mapped=%" B_PRIu64
			" retained_mapped=%" B_PRIu64 " anchor_area_bytes=%zu"
			" anchor_ram_bytes=%" B_PRIu64
			" wall_ms=%.3f client_ms=%.3f server_ms=%.3f\n",
			bytes, rounds, int(shared), peak, mapped_bytes(), info.size,
			uint64(info.ram_size), wall / 1000., client / 1000., cpu / 1000.);
	}
	anchor.reset();
	fence();
	printf("POOL_FINAL mapped=%" B_PRIu64 "\n", mapped_bytes());
	puts("BITMAP_POOL_PASS");
	return 0;
}
