// SPDX-License-Identifier: MIT
// Launch the recovery kernel's EFI stub with options from this same USB volume.
#include "efi-file-path.h"
#include <efi/protocol/simple-file-system.h>

extern "C" efi_status EFIAPI efi_main(efi_handle image, efi_system_table* table)
{
	auto boot = table->BootServices;
	boot->SetWatchdogTimer(0, 0, 0, nullptr);
	if (table->ConOut)
		table->ConOut->OutputString(table->ConOut,
			const_cast<char16_t*>(u"HAIKU_LAB_ROOBI_EFI_ENTRY\r\n"));
	efi_guid loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	efi_guid fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	efi_loaded_image_protocol* loaded = nullptr;
	efi_simple_file_system_protocol* fs = nullptr;
	efi_file_protocol* volume = nullptr;
	efi_file_protocol* optionsFile = nullptr;
	efi_status status = boot->HandleProtocol(image, &loadedGuid, reinterpret_cast<void**>(&loaded));
	if (status & EFI_ERROR_MASK)
		return status;
	status = boot->HandleProtocol(loaded->DeviceHandle, &fsGuid, reinterpret_cast<void**>(&fs));
	if (status & EFI_ERROR_MASK)
		return status;
	status = fs->OpenVolume(fs, &volume);
	if (status & EFI_ERROR_MASK)
		return status;
	status = volume->Open(volume, &optionsFile, u"\\roobi-options.txt", EFI_FILE_MODE_READ, 0);
	volume->Close(volume);
	if (status & EFI_ERROR_MASK)
		return status;
	char ascii[2048];
	size_t bytes = sizeof(ascii);
	status = optionsFile->Read(optionsFile, &bytes, ascii);
	optionsFile->Close(optionsFile);
	if ((status & EFI_ERROR_MASK) || bytes == sizeof(ascii))
		return EFI_BAD_BUFFER_SIZE;
	static char16_t options[2048];
	for (size_t i = 0; i < bytes; i++) {
		if (static_cast<unsigned char>(ascii[i]) > 127 || ascii[i] == 0)
			return EFI_INVALID_PARAMETER;
		options[i] = (ascii[i] == '\r' || ascii[i] == '\n') ? ' ' : ascii[i];
	}
	options[bytes] = 0;
	efi_handle kernel = nullptr;
	status = load_sibling(image, table, u"\\roobi-kernel.efi", &kernel);
	if (status & EFI_ERROR_MASK)
		return status;
	status = boot->HandleProtocol(kernel, &loadedGuid, reinterpret_cast<void**>(&loaded));
	if (status & EFI_ERROR_MASK)
		return status;
	loaded->LoadOptions = options;
	loaded->LoadOptionsSize = (bytes + 1) * sizeof(char16_t);
	if (table->ConOut)
		table->ConOut->OutputString(table->ConOut,
			const_cast<char16_t*>(u"HAIKU_LAB_ROOBI_KERNEL_START\r\n"));
	return boot->StartImage(kernel, nullptr, nullptr);
}
