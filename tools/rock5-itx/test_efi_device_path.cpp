// SPDX-License-Identifier: MIT
#include "device_path_match.h"

#include <assert.h>
#include <vector>

int
main()
{
	// Two USB disks sharing a controller, and a partition on the second disk.
	const uint8_t diskA[] = {1, 2, 5, 0, 0, 3, 5, 6, 0, 1, 0, 0x7f, 0xff, 4, 0};
	const uint8_t diskB[] = {1, 2, 5, 0, 0, 3, 5, 6, 0, 2, 0, 0x7f, 0xff, 4, 0};
	const uint8_t partitionB[] = {
		1, 2, 5, 0, 0, 3, 5, 6, 0, 2, 0, 4, 1, 8, 0, 1, 0, 0, 0, 0x7f, 0xff, 4, 0
	};
	const uint8_t controller[] = {1, 2, 5, 0, 0, 0x7f, 0xff, 4, 0};
	assert(efi_device_path_prefix_length(diskA, partitionB) == 0);
	assert(efi_device_path_prefix_length(diskB, partitionB) == 11);
	assert(efi_device_path_prefix_length(diskB, diskB) == 11);
	assert(efi_device_path_prefix_length(partitionB, diskB) == 0);
	assert(efi_device_path_prefix_length(controller, partitionB) == 5);
	assert(efi_device_path_prefix_length(diskB, partitionB)
		> efi_device_path_prefix_length(controller, partitionB));
	assert(efi_device_path_prefix_length(NULL, partitionB) == 0);

	const uint8_t empty[] = {0x7f, 0xff, 4, 0};
	const uint8_t shortNode[] = {1, 4, 3, 0};
	const uint8_t instance[] = {1, 2, 5, 0, 0, 0x7f, 1, 4, 0};
	assert(efi_device_path_prefix_length(empty, partitionB) == 0);
	assert(efi_device_path_prefix_length(shortNode, partitionB) == 0);
	assert(efi_device_path_prefix_length(instance, partitionB) == 0);
	assert(efi_device_path_prefix_length(controller, instance) == 0);

	// A path without an end node must terminate the scan at its bounded limit.
	std::vector<uint8_t> endless(4096);
	for (size_t i = 0; i < endless.size(); i += 4) {
		endless[i] = 1;
		endless[i + 1] = 4;
		endless[i + 2] = 4;
	}
	assert(efi_device_path_prefix_length(endless.data(), endless.data()) == 0);
	return 0;
}
