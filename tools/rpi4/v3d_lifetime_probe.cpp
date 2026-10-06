/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Run with other graphics applications closed. The fixed GPU's page table
// must survive the last descriptor, while each client's buffers are released.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>
#include <graphics/v3d/v3d_drm.h>
#include <graphics/v3d/v3d_haiku.h>


static void
Check(bool ok, const char* operation)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s: %s\n", operation, strerror(errno));
		exit(1);
	}
}


static area_id
PageTable(unsigned& buffers)
{
	ssize_t cookie = 0;
	area_info area;
	area_id table = -1;
	unsigned tables = 0;
	buffers = 0;
	while (get_next_area_info(B_SYSTEM_TEAM, &cookie, &area) == B_OK) {
		if (strcmp(area.name, "v3d page table") == 0) {
			Check(area.size == 4 * 1024 * 1024, "4 MiB page table");
			table = area.area;
			tables++;
		}
		if (strcmp(area.name, "v3d buffer") == 0)
			buffers++;
	}
	Check(tables == 1, "exactly one retained page table");
	return table;
}


int
main(int argc, char** argv)
{
	unsigned rounds = argc == 2 ? atoi(argv[1]) : 100;
	if (argc > 2 || rounds == 0 || rounds > 10000) {
		fprintf(stderr, "usage: %s [rounds 1..10000]\n", argv[0]);
		return 2;
	}
	area_id originalTable = -1;
	unsigned originalBuffers = 0;
	for (unsigned round = 0; round < rounds; round++) {
		int device = open(V3D_HAIKU_DEVICE_PATH, O_RDWR);
		Check(device >= 0, "open GPU");
		unsigned buffers;
		area_id table = PageTable(buffers);
		if (round == 0) {
			originalTable = table;
			originalBuffers = buffers;
		}
		Check(table == originalTable && buffers == originalBuffers,
			"stable hardware state before allocation");

		drm_v3d_create_bo create = {};
		create.size = 1024 * 1024;
		Check(ioctl(device, V3D_HAIKU_CREATE_BO, &create, sizeof(create)) == 0,
			"allocate client buffer");
		drm_v3d_mmap_bo map = {};
		map.handle = create.handle;
		Check(ioctl(device, V3D_HAIKU_MMAP_BO, &map, sizeof(map)) == 0,
			"map client buffer");
		uint8* bytes = (uint8*)(addr_t)map.offset;
		for (size_t offset = 0; offset < create.size; offset += B_PAGE_SIZE)
			bytes[offset] = (uint8)(round ^ (offset / B_PAGE_SIZE));
		for (size_t offset = 0; offset < create.size; offset += B_PAGE_SIZE) {
			Check(bytes[offset] == (uint8)(round ^ (offset / B_PAGE_SIZE)),
				"mapped buffer contents");
		}
		Check(delete_area(area_for(bytes)) == B_OK, "unmap client buffer");
		// Deliberately let close release the handle: test the per-file cleanup.
		Check(close(device) == 0, "close GPU");
		table = PageTable(buffers);
		Check(table == originalTable && buffers == originalBuffers,
			"retained table and released client buffer after last close");
	}
	printf("PASS GPU lifetime rounds=%u table=%" B_PRId32
		" retained_bytes=4194304 client_buffers=%u\n", rounds, originalTable,
		originalBuffers);
	return 0;
}
