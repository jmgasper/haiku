// SPDX-License-Identifier: MIT
#pragma once
#include <efi/system-table.h>
#include <efi/protocol/loaded-image.h>

static efi_status load_sibling(efi_handle parent, efi_system_table* table,
	const char16_t* filename, efi_handle* child)
{
	efi_guid loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	efi_guid pathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
	efi_loaded_image_protocol* loaded = nullptr;
	efi_device_path_protocol* base = nullptr;
	auto boot = table->BootServices;
	efi_status status = boot->HandleProtocol(parent, &loadedGuid, reinterpret_cast<void**>(&loaded));
	if (status & EFI_ERROR_MASK)
		return status;
	status = boot->HandleProtocol(loaded->DeviceHandle, &pathGuid, reinterpret_cast<void**>(&base));
	if (status & EFI_ERROR_MASK)
		return status;
	size_t baseSize = 0;
	auto node = base;
	while (node->Type != DEVICE_PATH_END) {
		if (node->Length < 4 || baseSize + node->Length > 4096)
			return EFI_UNSUPPORTED;
		baseSize += node->Length;
		node = reinterpret_cast<efi_device_path_protocol*>(reinterpret_cast<uint8_t*>(base) + baseSize);
	}
	if (node->SubType != DEVICE_PATH_ENTIRE_END)
		return EFI_UNSUPPORTED;
	size_t filenameSize = 0;
	while (filename[filenameSize])
		filenameSize++;
	filenameSize = (filenameSize + 1) * sizeof(char16_t);
	if (filenameSize > 1024)
		return EFI_INVALID_PARAMETER;
	uint8_t* path = nullptr;
	status = boot->AllocatePool(EfiLoaderData, baseSize + 4 + filenameSize + 4,
		reinterpret_cast<void**>(&path));
	if (status & EFI_ERROR_MASK)
		return status;
	for (size_t i = 0; i < baseSize; i++)
		path[i] = reinterpret_cast<uint8_t*>(base)[i];
	auto fileNode = reinterpret_cast<efi_device_path_protocol*>(path + baseSize);
	fileNode->Type = DEVICE_PATH_MEDIA;
	fileNode->SubType = MEDIA_FILEPATH_DP;
	fileNode->Length = 4 + filenameSize;
	for (size_t i = 0; i < filenameSize; i++)
		path[baseSize + 4 + i] = reinterpret_cast<const uint8_t*>(filename)[i];
	auto end = reinterpret_cast<efi_device_path_protocol*>(path + baseSize + 4 + filenameSize);
	end->Type = DEVICE_PATH_END;
	end->SubType = DEVICE_PATH_ENTIRE_END;
	end->Length = 4;
	status = boot->LoadImage(false, parent, reinterpret_cast<efi_device_path_protocol*>(path),
		nullptr, 0, child);
	boot->FreePool(path);
	return status;
}
