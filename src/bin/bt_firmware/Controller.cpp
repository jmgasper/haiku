/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * USB HCI transport for controllers that are not yet running operational
 * firmware, and firmware file lookup.
 */


#include "BluetoothFirmware.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <FindDirectory.h>

#include "usb_raw.h"


static const uint16_t kOpcodeSecureSend = 0xfc09;


static void
Watchdog(unsigned milliseconds)
{
	alarm(milliseconds == 0 ? 0 : (milliseconds + 999) / 1000);
}


Controller::Controller(int fd, const char* path)
	:
	bootloader(false),
	fFd(fd),
	fPath(path),
	fDownloadDone(false),
	fDownloadResult(0),
	fBooted(false)
{
	fInterruptIn.index = fBulkOut.index = fBulkIn.index = -1;
}


Controller::~Controller()
{
	close(fFd);
}


bool
Controller::_FindEndpoints()
{
	usb_interface_descriptor interface;
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.interface.descriptor = &interface;
	command.interface.config_index = 0;
	command.interface.interface_index = 0;
	if (ioctl(fFd, B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR, &command,
			sizeof(command)) < 0
		|| command.interface.status != B_USB_RAW_STATUS_SUCCESS) {
		ERROR("%s: cannot read interface 0\n", Path());
		return false;
	}

	for (uint32 i = 0; i < interface.num_endpoints; i++) {
		usb_endpoint_descriptor endpoint;
		memset(&command, 0, sizeof(command));
		command.endpoint.descriptor = &endpoint;
		command.endpoint.config_index = 0;
		command.endpoint.interface_index = 0;
		command.endpoint.endpoint_index = i;
		if (ioctl(fFd, B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR, &command,
				sizeof(command)) < 0
			|| command.endpoint.status != B_USB_RAW_STATUS_SUCCESS) {
			continue;
		}
		const bool in = (endpoint.endpoint_address & 0x80) != 0;
		const uint8_t type = endpoint.attributes & 0x03;
		const size_t maxPacket = endpoint.max_packet_size & 0x7ff;
		Endpoint* target = NULL;
		if (type == 0x03 && in)
			target = &fInterruptIn;
		else if (type == 0x02 && in)
			target = &fBulkIn;
		else if (type == 0x02 && !in)
			target = &fBulkOut;
		if (target != NULL && target->index < 0) {
			target->index = i;
			target->maxPacket = maxPacket != 0 ? maxPacket : 64;
		}
	}

	if (fInterruptIn.index < 0 || fBulkOut.index < 0 || fBulkIn.index < 0) {
		ERROR("%s: interface 0 lacks the HCI endpoints\n", Path());
		return false;
	}
	TRACE("%s: endpoints interrupt-in %d bulk-out %d bulk-in %d\n", Path(),
		(int)fInterruptIn.index, (int)fBulkOut.index, (int)fBulkIn.index);
	return true;
}


bool
Controller::Start()
{
	return _FindEndpoints();
}


bool
Controller::_ReadEvent(bool bulk, std::vector<uint8_t>& event,
	unsigned timeout)
{
	std::vector<uint8_t>& pending = fPending[bulk ? 1 : 0];
	const Endpoint& endpoint = bulk ? fBulkIn : fInterruptIn;
	std::vector<uint8_t> packet(endpoint.maxPacket);

	// One maximum size packet per transfer: an event that ends exactly on a
	// packet boundary still completes its transfer, and longer events are
	// put together from consecutive packets.
	while (pending.size() < 2 || pending.size() < (size_t)pending[1] + 2) {
		usb_raw_command command;
		memset(&command, 0, sizeof(command));
		command.transfer.interface = 0;
		command.transfer.endpoint = endpoint.index;
		command.transfer.data = packet.data();
		command.transfer.length = packet.size();
		Watchdog(timeout);
		const int result = ioctl(fFd,
			bulk ? B_USB_RAW_COMMAND_BULK_TRANSFER
				: B_USB_RAW_COMMAND_INTERRUPT_TRANSFER,
			&command, sizeof(command));
		Watchdog(0);
		if (result < 0 || command.transfer.status != B_USB_RAW_STATUS_SUCCESS) {
			ERROR("%s: %s event read failed (status %d)\n", Path(),
				bulk ? "bulk" : "interrupt", (int)command.transfer.status);
			return false;
		}
		pending.insert(pending.end(), packet.begin(),
			packet.begin() + command.transfer.length);
	}

	const size_t length = (size_t)pending[1] + 2;
	event.assign(pending.begin(), pending.begin() + length);
	pending.erase(pending.begin(), pending.begin() + length);
	TRACE("  event %02x, %zu bytes (%s)\n", event[0], event.size(),
		bulk ? "bulk" : "interrupt");

	// Intel vendor events that later steps wait for (btusb_recv_event_intel)
	if (event[0] == 0xff && event.size() >= 3) {
		if (event[2] == 0x06 && event.size() >= 4) {
			fDownloadDone = true;
			fDownloadResult = event[3];
		} else if (event[2] == 0x02) {
			fBooted = true;
		}
	}
	return true;
}


bool
Controller::_Send(uint16_t opcode, const void* parameters, size_t length)
{
	if (length > 255)
		return false;
	uint8_t packet[258] = { (uint8_t)opcode, (uint8_t)(opcode >> 8),
		(uint8_t)length };
	if (length != 0)
		memcpy(packet + 3, parameters, length);

	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	int result;
	Watchdog(kTransferWatchdog);
	if (bootloader && opcode == kOpcodeSecureSend) {
		command.transfer.interface = 0;
		command.transfer.endpoint = fBulkOut.index;
		command.transfer.data = packet;
		command.transfer.length = length + 3;
		result = ioctl(fFd, B_USB_RAW_COMMAND_BULK_TRANSFER, &command,
			sizeof(command));
		Watchdog(0);
		if (result < 0 || command.transfer.status != B_USB_RAW_STATUS_SUCCESS
			|| command.transfer.length != length + 3) {
			ERROR("%s: bulk command %04x failed (status %d)\n", Path(),
				opcode, (int)command.transfer.status);
			return false;
		}
		return true;
	}

	command.control.request_type = 0x20;
	command.control.request = 0;
	command.control.value = 0;
	command.control.index = 0;
	command.control.length = length + 3;
	command.control.data = packet;
	result = ioctl(fFd, B_USB_RAW_COMMAND_CONTROL_TRANSFER, &command,
		sizeof(command));
	Watchdog(0);
	if (result < 0 || command.control.status != B_USB_RAW_STATUS_SUCCESS
		|| command.control.length != length + 3) {
		ERROR("%s: command %04x failed (status %d)\n", Path(), opcode,
			(int)command.control.status);
		return false;
	}
	return true;
}


bool
Controller::Command(uint16_t opcode, const void* parameters, size_t length,
	uint8_t* reply, size_t* replyLength, unsigned timeout)
{
	if (!_Send(opcode, parameters, length))
		return false;
	const bool bulk = bootloader && opcode == kOpcodeSecureSend;
	std::vector<uint8_t> event;
	for (int i = 0; i < 32; i++) {
		if (!_ReadEvent(bulk, event, timeout))
			return false;
		if (event[0] == 0x0f && event.size() >= 6
			&& (event[4] | (event[5] << 8)) == opcode && event[2] != 0) {
			ERROR("%s: command %04x rejected, status 0x%02x\n", Path(),
				opcode, event[2]);
			return false;
		}
		if (event[0] != 0x0e || event.size() < 6
			|| (event[3] | (event[4] << 8)) != opcode) {
			continue;
		}
		if (event[5] != 0) {
			ERROR("%s: command %04x failed, status 0x%02x\n", Path(),
				opcode, event[5]);
			return false;
		}
		if (reply != NULL && replyLength != NULL) {
			const size_t available = event.size() - 6;
			if (available > *replyLength)
				return false;
			memcpy(reply, event.data() + 6, available);
			*replyLength = available;
		}
		return true;
	}
	ERROR("%s: command %04x: no completion\n", Path(), opcode);
	return false;
}


bool
Controller::Post(uint16_t opcode, const void* parameters, size_t length)
{
	return _Send(opcode, parameters, length);
}


bool
Controller::CommandExpecting(uint16_t opcode, const void* parameters,
	size_t length, uint8_t eventCode, std::vector<uint8_t>& event,
	unsigned timeout)
{
	if (!_Send(opcode, parameters, length))
		return false;
	for (int i = 0; i < 32; i++) {
		if (!_ReadEvent(false, event, timeout))
			return false;
		if (event[0] == 0x0f && event.size() >= 6
			&& (event[4] | (event[5] << 8)) == opcode && event[2] != 0) {
			ERROR("%s: command %04x rejected, status 0x%02x\n", Path(),
				opcode, event[2]);
			return false;
		}
		if (event[0] != eventCode)
			continue;
		if (eventCode == 0x0e && (event.size() < 5
				|| (event[3] | (event[4] << 8)) != opcode)) {
			continue;
		}
		return true;
	}
	ERROR("%s: command %04x: no event 0x%02x\n", Path(), opcode, eventCode);
	return false;
}


bool
Controller::SecureSend(uint8_t type, const uint8_t* data, size_t length)
{
	while (length > 0) {
		const size_t part = length > 252 ? 252 : length;
		uint8_t parameters[253];
		parameters[0] = type;
		memcpy(parameters + 1, data, part);
		if (!Command(kOpcodeSecureSend, parameters, part + 1, NULL, NULL,
				kInitTimeout)) {
			return false;
		}
		data += part;
		length -= part;
	}
	return true;
}


void
Controller::ClearVendorState()
{
	fDownloadDone = false;
	fBooted = false;
}


bool
Controller::WaitDownloadResult(unsigned timeout, uint8_t& result)
{
	std::vector<uint8_t> event;
	for (int i = 0; !fDownloadDone && i < 16; i++) {
		if (!_ReadEvent(false, event, timeout))
			return false;
	}
	result = fDownloadResult;
	return fDownloadDone;
}


bool
Controller::WaitBootup(unsigned timeout)
{
	std::vector<uint8_t> event;
	for (int i = 0; !fBooted && i < 16; i++) {
		if (!_ReadEvent(false, event, timeout))
			return false;
	}
	return fBooted;
}


// #pragma mark - firmware files


static std::vector<std::string> sFirmwareDirectories;


void
InitFirmwareDirectories(const std::vector<std::string>& extra)
{
	sFirmwareDirectories = extra;
	const directory_which directories[] = {
		B_USER_NONPACKAGED_DATA_DIRECTORY,
		B_USER_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};
	for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]);
			i++) {
		char path[B_PATH_NAME_LENGTH];
		if (find_directory(directories[i], -1, false, path, sizeof(path))
				== B_OK) {
			sFirmwareDirectories.push_back(std::string(path) + "/firmware");
		}
	}
}


bool
FindFirmware(const char* subdirectory, const char* name, std::string& path)
{
	for (size_t i = 0; i < sFirmwareDirectories.size(); i++) {
		std::string candidate = sFirmwareDirectories[i] + "/" + subdirectory
			+ "/" + name;
		struct stat info;
		if (stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
			path = candidate;
			return true;
		}
	}
	return false;
}


bool
LoadFirmware(const char* subdirectory, const char* name,
	std::vector<uint8_t>& data, bool required)
{
	std::string path;
	if (!FindFirmware(subdirectory, name, path)) {
		if (required)
			ERROR("firmware %s/%s not found\n", subdirectory, name);
		return false;
	}
	FILE* file = fopen(path.c_str(), "rb");
	if (file == NULL) {
		ERROR("cannot open %s: %s\n", path.c_str(), strerror(errno));
		return false;
	}
	fseek(file, 0, SEEK_END);
	const long size = ftell(file);
	rewind(file);
	if (size <= 0 || size > 8 * 1024 * 1024) {
		fclose(file);
		ERROR("%s: unexpected size %ld\n", path.c_str(), size);
		return false;
	}
	data.resize(size);
	const bool ok = fread(data.data(), 1, data.size(), file) == data.size();
	fclose(file);
	if (ok)
		LOG("  firmware file: %s (%ld bytes)\n", path.c_str(), size);
	return ok;
}
