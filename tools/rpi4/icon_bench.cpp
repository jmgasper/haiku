/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Render supplied HVIF files at several sizes. Separate worker bitmaps exercise
// concurrent first use; hashes let two libraries be checked for identical pixels.

#include <Bitmap.h>
#include <IconUtils.h>
#include <OS.h>
#include <image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


struct IconData {
	const char* name;
	uint8* data;
	size_t size;
};


struct Worker {
	IconData* icons;
	int iconCount;
	int rounds;
	int index;
	sem_id ready;
	sem_id start;
	uint64 hashes[6];
	bigtime_t sizeElapsed[6];
	bigtime_t sizeCPU[6];
	bigtime_t elapsed;
	bigtime_t cpu;
};


static uint64
hash_pixels(const BBitmap& bitmap, uint64 hash)
{
	const uint8* bits = (const uint8*)bitmap.Bits();
	int32 width = bitmap.Bounds().IntegerWidth() + 1;
	int32 height = bitmap.Bounds().IntegerHeight() + 1;
	for (int32 y = 0; y < height; y++) {
		for (int32 x = 0; x < width * 4; x++) {
			hash ^= bits[y * bitmap.BytesPerRow() + x];
			hash *= 1099511628211ULL;
		}
	}
	return hash;
}


static status_t
render_icons(void* cookie)
{
	Worker& worker = *(Worker*)cookie;
	const int sizes[] = {16, 24, 32, 64, 128, 256};
	release_sem(worker.ready);
	status_t status;
	do {
		status = acquire_sem(worker.start);
	} while (status == B_INTERRUPTED);
	if (status != B_OK)
		return status;

	thread_info before, after;
	get_thread_info(find_thread(NULL), &before);
	bigtime_t start = system_time();
	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		thread_info sizeBefore, sizeAfter;
		get_thread_info(find_thread(NULL), &sizeBefore);
		bigtime_t sizeStart = system_time();
		BBitmap bitmap(BRect(0, 0, sizes[s] - 1, sizes[s] - 1),
			B_BITMAP_NO_SERVER_LINK, B_RGBA32);
		if (bitmap.InitCheck() != B_OK)
			return bitmap.InitCheck();
		worker.hashes[s] = 1469598103934665603ULL;
		for (int round = 0; round < worker.rounds; round++) {
			for (int i = 0; i < worker.iconCount; i++) {
				memset(bitmap.Bits(), 0xa5, bitmap.BitsLength());
				status = BIconUtils::GetVectorIcon(worker.icons[i].data,
					worker.icons[i].size, &bitmap);
				if (status != B_OK) {
					fprintf(stderr, "render %s: %s\n", worker.icons[i].name,
						strerror(status));
					return status;
				}
				if (round == 0) {
					worker.hashes[s] = hash_pixels(bitmap, worker.hashes[s]);
					printf("PIXELS worker=%d icon=%d size=%d hash=%016llx\n",
						worker.index, i, sizes[s],
						(unsigned long long)hash_pixels(bitmap,
							1469598103934665603ULL));
				}
			}
		}
		worker.sizeElapsed[s] = system_time() - sizeStart;
		get_thread_info(find_thread(NULL), &sizeAfter);
		worker.sizeCPU[s] = sizeAfter.user_time + sizeAfter.kernel_time
			- sizeBefore.user_time - sizeBefore.kernel_time;
	}
	worker.elapsed = system_time() - start;
	get_thread_info(find_thread(NULL), &after);
	worker.cpu = after.user_time + after.kernel_time
		- before.user_time - before.kernel_time;
	return B_OK;
}


int
main(int argc, char** argv)
{
	if (argc < 4 || atoi(argv[1]) < 1 || atoi(argv[1]) > 10000
		|| atoi(argv[2]) < 1 || atoi(argv[2]) > 16) {
		fprintf(stderr, "usage: %s <rounds 1..10000> <workers 1..16> <HVIF files...>\n",
			argv[0]);
		return 2;
	}
	int32 imageCookie = 0;
	image_info image;
	while (get_next_image_info(B_CURRENT_TEAM, &imageCookie, &image) == B_OK) {
		const char* name = strrchr(image.name, '/');
		if (name != NULL && strcmp(name + 1, "libbe.so") == 0)
			printf("LIBBE path=%s\n", image.name);
	}
	const int iconCount = argc - 3;
	IconData* icons = new IconData[iconCount];
	for (int i = 0; i < iconCount; i++) {
		icons[i].name = argv[i + 3];
		FILE* file = fopen(icons[i].name, "rb");
		if (file == NULL || fseek(file, 0, SEEK_END) != 0)
			return 1;
		long size = ftell(file);
		if (size < 4 || size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0)
			return 1;
		icons[i].size = size;
		icons[i].data = new uint8[size];
		if (fread(icons[i].data, 1, size, file) != (size_t)size)
			return 1;
		fclose(file);
	}

	const int workerCount = atoi(argv[2]);
	Worker workers[16] = {};
	thread_id threads[16];
	sem_id ready = create_sem(0, "icon workers ready");
	sem_id start = create_sem(0, "icon workers start");
	if (ready < B_OK || start < B_OK)
		return 1;
	for (int i = 0; i < workerCount; i++) {
		workers[i].icons = icons;
		workers[i].iconCount = iconCount;
		workers[i].rounds = atoi(argv[1]);
		workers[i].index = i;
		workers[i].ready = ready;
		workers[i].start = start;
		threads[i] = spawn_thread(render_icons, "icon render", B_NORMAL_PRIORITY,
			&workers[i]);
		if (threads[i] < B_OK || resume_thread(threads[i]) != B_OK)
			return 1;
	}
	status_t status;
	do {
		status = acquire_sem_etc(ready, workerCount, 0, 0);
	} while (status == B_INTERRUPTED);
	if (status != B_OK || release_sem_etc(start, workerCount, 0) != B_OK)
		return 1;
	bool success = true;
	for (int i = 0; i < workerCount; i++) {
		status_t result;
		if (wait_for_thread(threads[i], &result) != B_OK || result != B_OK)
			success = false;
		if (memcmp(workers[0].hashes, workers[i].hashes,
				sizeof(workers[i].hashes)) != 0)
			success = false;
		printf("TIMING worker=%d renders=%d elapsed_ms=%.3f cpu_ms=%.3f\n", i,
			workers[i].rounds * iconCount * 6, workers[i].elapsed / 1000.,
			workers[i].cpu / 1000.);
		const int sizes[] = {16, 24, 32, 64, 128, 256};
		for (int s = 0; s < 6; s++) {
			printf("SIZE worker=%d size=%d renders=%d elapsed_ms=%.3f cpu_ms=%.3f\n",
				i, sizes[s], workers[i].rounds * iconCount,
				workers[i].sizeElapsed[s] / 1000., workers[i].sizeCPU[s] / 1000.);
		}
	}
	delete_sem(start);
	delete_sem(ready);
	for (int i = 0; i < iconCount; i++)
		delete[] icons[i].data;
	delete[] icons;
	puts(success ? "ICON_BENCH_PASS" : "ICON_BENCH_FAIL");
	return success ? 0 : 1;
}
