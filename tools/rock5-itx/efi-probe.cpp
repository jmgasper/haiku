// SPDX-License-Identifier: MIT
// A small ARM64 EFI diagnostic. It leaves boot services running and records
// firmware interfaces before the Haiku loader takes responsibility for them.
#include <efi/system-table.h>
#include <efi/protocol/graphics-output.h>
#include <efi/protocol/loaded-image.h>
#include <efi/protocol/serial-io.h>
#include <efi/protocol/simple-file-system.h>
#include "efi-file-path.h"
#include "efi-pci-read.h"

static efi_system_table* systemTable;
static efi_file_protocol* volume;
static efi_file_protocol* logFile;
static efi_serial_io_protocol* serial;
static bool failed;

extern "C" void* memcpy(void* destination, const void* source, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++)
		static_cast<uint8_t*>(destination)[i] = static_cast<const uint8_t*>(source)[i];
	return destination;
}

extern "C" void* memset(void* destination, int value, size_t bytes)
{
	for (size_t i = 0; i < bytes; i++)
		static_cast<uint8_t*>(destination)[i] = value;
	return destination;
}

static bool error(efi_status status)
{
	return (status & EFI_ERROR_MASK) != 0;
}

static void text(const char* message)
{
	char16_t wide[256];
	size_t size = 0;
	while (message[size] && size < 255) {
		wide[size] = message[size];
		size++;
	}
	wide[size] = 0;
	if (systemTable->ConOut)
		systemTable->ConOut->OutputString(systemTable->ConOut, wide);
	if (serial) {
		size_t length = size;
		serial->Write(serial, &length, const_cast<char*>(message));
	}
	if (logFile) {
		size_t length = size;
		if (error(logFile->Write(logFile, &length, const_cast<char*>(message))) || length != size)
			failed = true;
	}
}

static void number(const char* name, uint64_t value)
{
	char digits[19] = "0x0000000000000000";
	for (int i = 17; i >= 2; i--) {
		digits[i] = "0123456789abcdef"[value & 15];
		value >>= 4;
	}
	text(name); text(digits); text("\r\n");
}

static void blob(const char16_t* name, const void* data, size_t size)
{
	if (!volume)
		return;
	efi_file_protocol* file = nullptr;
	efi_status status = volume->Open(volume, &file, name,
		EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
	if (!error(status)) {
		size_t written = size;
		status = file->Write(file, &written, const_cast<void*>(data));
		if (written != size)
			status = EFI_DEVICE_ERROR;
		if (error(file->Flush(file)))
			status = EFI_DEVICE_ERROR;
		file->Close(file);
	}
	if (error(status)) {
		failed = true;
		number("file_write_error=", status);
	}
}

static uint32_t be32(const uint8_t* data)
{
	return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16)
		| (uint32_t(data[2]) << 8) | data[3];
}

static uint32_t le32(const uint8_t* data)
{
	return uint32_t(data[0]) | (uint32_t(data[1]) << 8)
		| (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

static uint64_t le64(const uint8_t* data)
{
	return le32(data) | (uint64_t(le32(data + 4)) << 32);
}

static void fileIndex(char16_t* name, size_t offset, size_t index)
{
	for (int i = 3; i >= 0; i--) {
		name[offset + i] = u"0123456789abcdef"[index & 15];
		index >>= 4;
	}
}

static void pciInventory()
{
	auto boot = systemTable->BootServices;
	efi_guid rootGuid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
	efi_handle* handles = nullptr;
	size_t count = 0;
	efi_status status = boot->LocateHandleBuffer(ByProtocol, &rootGuid, nullptr,
		&count, &handles);
	number("pci_root_locate_status=", status);
	if (!error(status) && count <= 32) {
		number("pci_root_count=", count);
		for (size_t i = 0; i < count; i++) {
			efi_pci_root_bridge_io_protocol* root = nullptr;
			status = boot->HandleProtocol(handles[i], &rootGuid,
				reinterpret_cast<void**>(&root));
			if (error(status)) {
				failed = true;
				continue;
			}
			text("PCI_ROOT_BEGIN\r\n");
			number("index=", i);
			number("segment=", root->SegmentNumber);
			void* resources = nullptr;
			status = root->Configuration(root, &resources);
			number("configuration_status=", status);
			if (!error(status) && resources != nullptr) {
				// UEFI Configuration returns QWORD address descriptors and an
				// end tag. This buffer belongs to the firmware, not the caller.
				auto data = static_cast<const uint8_t*>(resources);
				size_t bytes = 0;
				bool ended = false;
				for (unsigned j = 0; j < 32; j++) {
					if (data[bytes] == 0x79) {
						bytes += 2;
						ended = true;
						break;
					}
					if (data[bytes] != 0x8a || data[bytes + 1] != 43
						|| data[bytes + 2] != 0)
						break;
					const uint8_t* range = data + bytes;
					number("resource_type=", range[3]);
					number("resource_flags=", range[4] | (uint64_t(range[5]) << 8));
					number("resource_granularity=", le64(range + 6));
					number("resource_minimum=", le64(range + 14));
					number("resource_maximum=", le64(range + 22));
					number("resource_translation=", le64(range + 30));
					number("resource_length=", le64(range + 38));
					bytes += 46;
				}
				if (ended) {
					char16_t name[] = u"\\pci-root-0000.bin";
					fileIndex(name, 10, i);
					blob(name, data, bytes);
				} else {
					failed = true;
					text("PCI_RESOURCE_DESCRIPTOR_INVALID\r\n");
				}
			} else if (status != EFI_UNSUPPORTED) {
				failed = true;
			}
			text("PCI_ROOT_END\r\n");
		}
	} else if (status != EFI_NOT_FOUND) {
		failed = true;
	}
	if (handles)
		boot->FreePool(handles);

	efi_guid pciGuid = ROCK5_EFI_PCI_IO_PROTOCOL_GUID;
	handles = nullptr;
	count = 0;
	status = boot->LocateHandleBuffer(ByProtocol, &pciGuid, nullptr, &count, &handles);
	number("pci_device_locate_status=", status);
	if (!error(status) && count <= 256) {
		number("pci_device_count=", count);
		for (size_t i = 0; i < count; i++) {
			efi_pci_io_read_view* pci = nullptr;
			status = boot->HandleProtocol(handles[i], &pciGuid,
				reinterpret_cast<void**>(&pci));
			if (error(status)) {
				failed = true;
				continue;
			}
			size_t segment = 0, bus = 0, device = 0, function = 0;
			status = pci->GetLocation(pci, &segment, &bus, &device, &function);
			alignas(4) uint8_t config[256] = {};
			if (!error(status))
				status = pci->Pci.Read(pci, EfiPciWidthUint32, 0, 64, config);
			text("PCI_DEVICE_BEGIN\r\n");
			number("index=", i);
			number("read_status=", status);
			if (!error(status)) {
				number("segment=", segment);
				number("bus=", bus);
				number("device=", device);
				number("function=", function);
				number("vendor_device=", le32(config));
				number("class_revision=", le32(config + 8));
				number("command_status=", le32(config + 4));
				char16_t name[] = u"\\pci-device-0000.bin";
				fileIndex(name, 12, i);
				blob(name, config, sizeof(config));
			} else {
				failed = true;
			}
			text("PCI_DEVICE_END\r\n");
		}
	} else if (status != EFI_NOT_FOUND) {
		failed = true;
	}
	if (handles)
		boot->FreePool(handles);
}

static void acpi(const uint8_t* rsdp)
{
	// Configuration tables are supplied by firmware. Bound lengths before saving.
	number("acpi_rsdp=", reinterpret_cast<uintptr_t>(rsdp));
	if (rsdp[15] < 2) {
		blob(u"\\haiku-rsdp.bin", rsdp, 20);
		return;
	}
	blob(u"\\haiku-rsdp.bin", rsdp, 36);
	auto xsdt = reinterpret_cast<const uint8_t*>(uintptr_t(le64(rsdp + 24)));
	if (!xsdt)
		return;
	uint32_t length = le32(xsdt + 4);
	if (length < 36 || length > 36 + 128 * 8) {
		failed = true;
		text("ACPI_XSDT_LENGTH_INVALID\r\n");
		return;
	}
	blob(u"\\haiku-xsdt.bin", xsdt, length);
	for (uint32_t offset = 36; offset + 8 <= length; offset += 8) {
		auto table = reinterpret_cast<const uint8_t*>(uintptr_t(le64(xsdt + offset)));
		if (!table)
			continue;
		char signature[5] = {char(table[0]), char(table[1]), char(table[2]), char(table[3]), 0};
		text("acpi_table="); text(signature); text("\r\n");
		number("address=", reinterpret_cast<uintptr_t>(table));
		uint32_t bytes = le32(table + 4);
		number("length=", bytes);
		if (bytes < 36 || bytes > 2 * 1024 * 1024) {
			failed = true;
			continue;
		}
		char16_t name[] = u"\\haiku-acpi-00.bin";
		unsigned index = (offset - 36) / 8;
		name[12] = u"0123456789abcdef"[index >> 4];
		name[13] = u"0123456789abcdef"[index & 15];
		blob(name, table, bytes);
	}
}

extern "C" efi_status EFIAPI efi_main(efi_handle image, efi_system_table* table)
{
	systemTable = table;
	if (table->Hdr.Signature != EFI_SYSTEM_TABLE_SIGNATURE)
		return EFI_INVALID_PARAMETER;
	auto boot = table->BootServices;
	boot->SetWatchdogTimer(0, 0, 0, nullptr);
	efi_guid serialGuid = EFI_SERIAL_IO_PROTOCOL_GUID;
	boot->LocateProtocol(&serialGuid, nullptr, reinterpret_cast<void**>(&serial));
	text("HAIKU_EFI_PROBE_ENTRY\r\n");
	efi_guid loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	efi_guid fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	efi_loaded_image_protocol* loaded = nullptr;
	efi_simple_file_system_protocol* fs = nullptr;
	if (!error(boot->HandleProtocol(image, &loadedGuid, reinterpret_cast<void**>(&loaded)))
		&& !error(boot->HandleProtocol(loaded->DeviceHandle, &fsGuid, reinterpret_cast<void**>(&fs)))
		&& !error(fs->OpenVolume(fs, &volume))) {
		volume->Open(volume, &logFile, u"\\haiku-efi-probe.txt",
			EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
	}
	text("HAIKU_EFI_PROBE_V2\r\n");
	number("uefi_revision=", table->Hdr.Revision);
	number("firmware_revision=", table->FirmwareRevision);
	uint64_t exceptionLevel;
	asm volatile("mrs %0, CurrentEL" : "=r"(exceptionLevel));
	number("exception_level=", exceptionLevel >> 2);
	number("configuration_table_count=", table->NumberOfTableEntries);
	pciInventory();

	efi_guid graphicsGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	efi_graphics_output_protocol* graphics = nullptr;
	efi_status status = boot->LocateProtocol(&graphicsGuid, nullptr, reinterpret_cast<void**>(&graphics));
	number("gop_status=", status);
	if (!error(status) && graphics->Mode && graphics->Mode->Info) {
		auto mode = graphics->Mode;
		number("framebuffer_base=", mode->FrameBufferBase);
		number("framebuffer_bytes=", mode->FrameBufferSize);
		number("width=", mode->Info->HorizontalResolution);
		number("height=", mode->Info->VerticalResolution);
		number("pixels_per_scanline=", mode->Info->PixelsPerScanLine);
		number("pixel_format=", mode->Info->PixelFormat);
	} else {
		failed = true;
	}

	efi_guid dtGuid = DEVICE_TREE_GUID;
	efi_guid acpiGuid = ACPI_20_TABLE_GUID;
	for (size_t i = 0; i < table->NumberOfTableEntries; i++) {
		auto& entry = table->ConfigurationTable[i];
		if (entry.VendorGuid.equals(dtGuid)) {
			auto dt = static_cast<const uint8_t*>(entry.VendorTable);
			number("dtb_address=", reinterpret_cast<uintptr_t>(dt));
			uint32_t bytes = be32(dt + 4);
			if (be32(dt) == 0xd00dfeed && bytes >= 40 && bytes <= 16 * 1024 * 1024) {
				number("dtb_bytes=", bytes);
				blob(u"\\haiku-firmware.dtb", dt, bytes);
			} else {
				failed = true;
				text("DTB_HEADER_INVALID\r\n");
			}
		}
		if (entry.VendorGuid.equals(acpiGuid))
			acpi(static_cast<const uint8_t*>(entry.VendorTable));
	}

	alignas(8) static uint8_t map[65536];
	size_t mapSize = sizeof(map), key = 0, stride = 0;
	uint32_t version = 0;
	status = boot->GetMemoryMap(&mapSize, reinterpret_cast<efi_memory_descriptor*>(map),
		&key, &stride, &version);
	number("memory_map_status=", status);
	if (!error(status) && stride >= sizeof(efi_memory_descriptor) && mapSize % stride == 0) {
		number("memory_descriptor_bytes=", stride);
		number("memory_descriptor_version=", version);
		number("memory_descriptor_count=", mapSize / stride);
		blob(u"\\haiku-memory-map.bin", map, mapSize);
		for (size_t offset = 0; offset < mapSize; offset += stride) {
			auto descriptor = reinterpret_cast<efi_memory_descriptor*>(map + offset);
			number("memory_type=", descriptor->Type);
			number("physical_start=", descriptor->PhysicalStart);
			number("pages=", descriptor->NumberOfPages);
			number("attributes=", descriptor->Attribute);
		}
	} else {
		failed = true;
	}
	text(failed ? "HAIKU_EFI_PROBE_ERRORS\r\n" : "HAIKU_EFI_PROBE_COMPLETE\r\n");
	if (logFile) {
		logFile->Flush(logFile);
		logFile->Close(logFile);
		logFile = nullptr;
	}
	if (volume)
		volume->Close(volume);
	// Leave time for HDMI capture; no kernel handoff or storage firmware writes.
	boot->Stall(30000000);
	// A combined diagnostic/recovery image can resume Linux after evidence is saved.
	efi_handle recovery = nullptr;
	status = load_sibling(image, table, u"\\recovery.efi", &recovery);
	if (!error(status)) {
		text("HAIKU_EFI_PROBE_START_RECOVERY\r\n");
		return boot->StartImage(recovery, nullptr, nullptr);
	}
	return failed ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
