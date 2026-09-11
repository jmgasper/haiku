/*
 * Distributed under the terms of the MIT License.
 */
#ifndef EFI_DEVICE_PATH_MATCH_H
#define EFI_DEVICE_PATH_MATCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>


// Return the non-end prefix length when parent describes child or an ancestor
// of child. Device paths supplied by firmware have no separate buffer length;
// reject malformed nodes and instance separators encountered while comparing,
// and bound the traversal length.
static inline size_t
efi_device_path_prefix_length(const void* parentPath, const void* childPath)
{
	if (parentPath == NULL || childPath == NULL)
		return 0;

	const uint8_t* parent = (const uint8_t*)parentPath;
	const uint8_t* child = (const uint8_t*)childPath;
	const size_t maxLength = 4096;
	for (size_t offset = 0; offset <= maxLength - 4;) {
		// Nodes can be unaligned; Length is a little-endian 16-bit field.
		size_t parentLength = parent[offset + 2] | (parent[offset + 3] << 8);
		size_t childLength = child[offset + 2] | (child[offset + 3] << 8);
		if (parentLength < 4 || childLength < 4
			|| parentLength > maxLength - offset || childLength > maxLength - offset)
			return 0;
		if (child[offset] == 0x7f && (child[offset + 1] != 0xff || childLength != 4))
			return 0;

		if (parent[offset] == 0x7f) {
			return parent[offset + 1] == 0xff && parentLength == 4 ? offset : 0;
		}
		if (child[offset] == 0x7f || parentLength != childLength
			|| memcmp(parent + offset, child + offset, parentLength) != 0)
			return 0;
		offset += parentLength;
	}
	return 0;
}

#endif // EFI_DEVICE_PATH_MATCH_H
