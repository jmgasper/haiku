/* Look for Bluetooth Low Energy devices, and say what they claim to be.
 *
 * Haiku's Bluetooth stack knows nothing about Low Energy: it has no LE
 * commands, it does not ask the controller to report LE events, and the one
 * event it would receive it would not parse. So nothing in the system can say
 * whether a given radio does LE at all, let alone what is advertising nearby -
 * and the first thing a mouse that only speaks LE needs is to be seen.
 *
 * This asks the radio directly. A Bluetooth USB controller takes HCI commands
 * as class control transfers on endpoint zero and answers with events on an
 * interrupt endpoint; that is the whole transport, and going through it here
 * means no part of the stack has to be right for this to work. The sequence is
 * the one every LE host runs at startup: unmask the LE event, say the host
 * supports LE, ask what the controller's LE half can do, then set the scan
 * going and read what comes back.
 *
 * An advertising report carries what the device wants strangers to know:
 * its address, whether that address is a real one or a random one it made up,
 * how loudly it is heard, and a list of small typed fields - a name, the
 * services it offers, what kind of thing it is. That is enough to pick a mouse
 * out of a room.
 *
 * The radio must already have its firmware (the driver gives it that when the
 * stack opens it, so it does after a normal boot) and nothing else may be
 * reading its event endpoint, so stop the Bluetooth server first. A read of
 * that endpoint with nothing to report waits without a timeout, so run this
 * under `timeout -s KILL`: a kill is the one thing that interrupts it, and the
 * kernel then cancels the transfer properly.
 *
 * usage: btle [-d device] [-t seconds] [-p] [-v]
 *        -d  the radio, /dev/bus/usb/0/20 by default
 *        -t  how long to scan for, 10 seconds by default
 *        -p  passive scan: listen only, never ask for a scan response
 *        -v  print every event byte for byte
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <OS.h>

#include "usb_raw.h"

#define USB_CLASS_OUT			0x20	/* class request, host to device */

/* The HCI commands this needs. The opcode is the group in the top six bits
 * and the command in the rest; the LE commands are group 8.
 */
#define HCI_RESET			0x0c03
#define HCI_SET_EVENT_MASK		0x0c01
#define HCI_READ_LOCAL_VERSION		0x1001
#define HCI_READ_LOCAL_COMMANDS		0x1002
#define HCI_READ_LOCAL_FEATURES		0x1003
#define HCI_WRITE_LE_HOST_SUPPORT	0x0c6d
#define HCI_LE_READ_BUFFER_SIZE		0x2002
#define HCI_LE_READ_LOCAL_FEATURES	0x2003
#define HCI_LE_SET_SCAN_PARAMETERS	0x200b
#define HCI_LE_SET_SCAN_ENABLE		0x200c

#define HCI_EVENT_COMMAND_COMPLETE	0x0e
#define HCI_EVENT_COMMAND_STATUS	0x0f
#define HCI_EVENT_LE_META		0x3e

#define LE_SUBEVENT_ADVERTISING_REPORT	0x02

#define MAX_DEVICES			64
#define MAX_EVENT			320

static bool sVerbose = false;


struct seen_device {
	uint8	address[6];
	uint8	addressType;
	int8	rssi;
	int	reports;
	bool	connectable;
	bool	scanResponse;
	uint16	appearance;
	bool	hasAppearance;
	char	name[64];
	uint16	services[16];
	int	serviceCount;
};

static seen_device sDevices[MAX_DEVICES];
static int sDeviceCount = 0;


static void
Dump(const char* what, const uint8* data, size_t length)
{
	printf("    %s (%zu bytes):", what, length);
	for (size_t i = 0; i < length && i < 64; i++)
		printf(" %02x", data[i]);
	if (length > 64)
		printf(" ...");
	printf("\n");
}


static const char*
StatusName(status_t status)
{
	switch (status) {
		case B_USB_RAW_STATUS_SUCCESS: return "ok";
		case B_USB_RAW_STATUS_FAILED: return "failed";
		case B_USB_RAW_STATUS_ABORTED: return "aborted";
		case B_USB_RAW_STATUS_STALLED: return "stalled";
		case B_USB_RAW_STATUS_CRC_ERROR: return "CRC error";
		case B_USB_RAW_STATUS_TIMEOUT: return "timed out";
		case B_USB_RAW_STATUS_INVALID_ENDPOINT: return "bad endpoint";
	}
	return "?";
}


/* What an LE address type means. A device is allowed to advertise under an
 * address it invented, and a resolvable one changes every fifteen minutes, so
 * which kind it is decides whether writing it down is worth anything.
 */
static const char*
AddressTypeName(uint8 type)
{
	switch (type) {
		case 0x00: return "public";
		case 0x01: return "random";
		case 0x02: return "public identity";
		case 0x03: return "random identity";
	}
	return "?";
}


/* The generic-access appearance value: a category in the top ten bits and a
 * subtype in the rest. 0x03c0 is the human interface category.
 */
static const char*
AppearanceName(uint16 appearance)
{
	switch (appearance) {
		case 0x0000: return "unknown";
		case 0x03c0: return "human interface device";
		case 0x03c1: return "keyboard";
		case 0x03c2: return "mouse";
		case 0x03c3: return "joystick";
		case 0x03c4: return "gamepad";
		case 0x03c5: return "digitizer tablet";
		case 0x03c8: return "digital pen";
		case 0x0080: return "generic computer";
		case 0x0040: return "generic phone";
	}
	if ((appearance & 0xffc0) == 0x03c0)
		return "human interface device (other)";
	return "";
}


static const char*
ServiceName(uint16 uuid)
{
	switch (uuid) {
		case 0x1800: return "generic access";
		case 0x1801: return "generic attribute";
		case 0x180a: return "device information";
		case 0x180f: return "battery";
		case 0x1812: return "human interface device";
		case 0x1813: return "scan parameters";
		case 0xfe59: return "Nordic firmware update";
	}
	return NULL;
}


static seen_device*
FindOrAddDevice(const uint8* address, uint8 addressType)
{
	for (int i = 0; i < sDeviceCount; i++) {
		if (memcmp(sDevices[i].address, address, 6) == 0
			&& sDevices[i].addressType == addressType)
			return &sDevices[i];
	}

	if (sDeviceCount == MAX_DEVICES)
		return NULL;

	seen_device* device = &sDevices[sDeviceCount++];
	memset(device, 0, sizeof(*device));
	memcpy(device->address, address, 6);
	device->addressType = addressType;
	return device;
}


static status_t
ControlTransfer(int fd, uint8 type, uint8 request, uint16 value, uint16 index,
	void* data, uint16 length)
{
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.control.request_type = type;
	command.control.request = request;
	command.control.value = value;
	command.control.index = index;
	command.control.length = length;
	command.control.data = data;

	if (ioctl(fd, B_USB_RAW_COMMAND_CONTROL_TRANSFER, &command,
			sizeof(command)) != 0)
		return errno != 0 ? (status_t)errno : B_ERROR;

	if (command.control.status != B_USB_RAW_STATUS_SUCCESS) {
		fprintf(stderr, "[!] the radio refused the command: %s\n",
			StatusName(command.control.status));
		return B_IO_ERROR;
	}

	return B_OK;
}


static status_t
SendCommand(int fd, uint16 opcode, const uint8* parameters, uint8 length)
{
	uint8 packet[3 + 255];
	packet[0] = opcode & 0xff;
	packet[1] = opcode >> 8;
	packet[2] = length;
	if (length > 0)
		memcpy(packet + 3, parameters, length);

	if (sVerbose)
		Dump("command", packet, 3 + length);

	return ControlTransfer(fd, USB_CLASS_OUT, 0x00, 0, 0, packet,
		(uint16)(3 + length));
}


/* Where the events come up. The index is into the interface's endpoint list
 * rather than an endpoint address, so the list has to be walked to find which
 * entry is the interrupt one - on this controller it is the first, but that is
 * a property of its descriptors and not a rule.
 */
static int sEventEndpoint = -1;
static int sAclOutEndpoint = -1;
static int sAclInEndpoint = -1;


static status_t
FindEndpoints(int fd, bool print)
{
	usb_raw_command command;

	uint32 alternate = 0;
	memset(&command, 0, sizeof(command));
	command.alternate.config_index = 0;
	command.alternate.interface_index = 0;
	if (ioctl(fd, B_USB_RAW_COMMAND_GET_ACTIVE_ALT_INTERFACE_INDEX, &command,
			sizeof(command)) == 0)
		alternate = command.alternate.alternate_info;

	usb_interface_descriptor interface;
	memset(&command, 0, sizeof(command));
	command.interface_etc.descriptor = &interface;
	command.interface_etc.config_index = 0;
	command.interface_etc.interface_index = 0;
	command.interface_etc.alternate_index = alternate;
	if (ioctl(fd, B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR_ETC, &command,
			sizeof(command)) != 0)
		return B_ERROR;

	if (print) {
		printf("interface 0 (alternate %" B_PRIu32 ") has %u endpoints\n",
			alternate, interface.num_endpoints);
	}

	for (uint32 i = 0; i < interface.num_endpoints; i++) {
		usb_endpoint_descriptor endpoint;
		memset(&command, 0, sizeof(command));
		command.endpoint_etc.descriptor = &endpoint;
		command.endpoint_etc.config_index = 0;
		command.endpoint_etc.interface_index = 0;
		command.endpoint_etc.alternate_index = alternate;
		command.endpoint_etc.endpoint_index = i;
		if (ioctl(fd, B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR_ETC, &command,
				sizeof(command)) != 0)
			continue;

		bool in = (endpoint.endpoint_address & 0x80) != 0;
		uint8 type = endpoint.attributes & 0x03;
		const char* kind = type == USB_ENDPOINT_ATTR_INTERRUPT ? "interrupt"
			: type == USB_ENDPOINT_ATTR_BULK ? "bulk"
			: type == USB_ENDPOINT_ATTR_ISOCHRONOUS ? "isochronous" : "control";

		if (print) {
			printf("  endpoint %" B_PRIu32 ": %s %s, address %#02x,"
				" %u bytes a packet\n", i, kind, in ? "in" : "out",
				endpoint.endpoint_address, endpoint.max_packet_size);
		}

		if (type == USB_ENDPOINT_ATTR_INTERRUPT && in && sEventEndpoint < 0)
			sEventEndpoint = (int)i;
		if (type == USB_ENDPOINT_ATTR_BULK && in && sAclInEndpoint < 0)
			sAclInEndpoint = (int)i;
		if (type == USB_ENDPOINT_ATTR_BULK && !in && sAclOutEndpoint < 0)
			sAclOutEndpoint = (int)i;
	}

	if (sEventEndpoint < 0)
		return B_DEV_INVALID_PIPE;

	if (print) {
		printf("events come up on endpoint index %d; ACL data would go out on"
			" %d and come in on %d\n\n", sEventEndpoint, sAclOutEndpoint,
			sAclInEndpoint);
	}

	return B_OK;
}


/* Read one event. This waits without a timeout, so it is only ever called
 * when something is known to be coming: an answer to a command just sent, or
 * an advertising report while the scan is running.
 */
static ssize_t
ReadEvent(int fd, uint8* buffer, size_t size)
{
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.transfer.interface = 0;
	command.transfer.endpoint = (uint32)sEventEndpoint;
	command.transfer.data = buffer;
	command.transfer.length = size;

	if (ioctl(fd, B_USB_RAW_COMMAND_INTERRUPT_TRANSFER, &command,
			sizeof(command)) != 0)
		return errno != 0 ? (ssize_t)-errno : -1;

	if (command.transfer.status != B_USB_RAW_STATUS_SUCCESS) {
		fprintf(stderr, "[!] reading an event: %s\n",
			StatusName(command.transfer.status));
		return -1;
	}

	return (ssize_t)command.transfer.length;
}


static void
ParseAdvertisingData(seen_device* device, const uint8* data, uint8 length)
{
	uint8 offset = 0;
	while (offset + 1 < length) {
		uint8 fieldLength = data[offset];
		if (fieldLength == 0 || offset + fieldLength >= length + 1u)
			break;

		uint8 type = data[offset + 1];
		const uint8* value = data + offset + 2;
		uint8 valueLength = fieldLength - 1;

		switch (type) {
			case 0x08:	/* shortened name */
			case 0x09:	/* complete name */
			{
				/* A complete name replaces a shortened one, never the
				 * other way around.
				 */
				if (device->name[0] != '\0' && type == 0x08)
					break;
				uint8 copy = valueLength < sizeof(device->name) - 1
					? valueLength : (uint8)(sizeof(device->name) - 1);
				memcpy(device->name, value, copy);
				device->name[copy] = '\0';
				break;
			}

			case 0x02:	/* incomplete list of 16 bit service UUIDs */
			case 0x03:	/* complete list */
			case 0x14:	/* services solicited */
				for (uint8 i = 0; i + 1 < valueLength; i += 2) {
					uint16 uuid = value[i] | (value[i + 1] << 8);
					bool known = false;
					for (int j = 0; j < device->serviceCount; j++) {
						if (device->services[j] == uuid)
							known = true;
					}
					if (!known && device->serviceCount
							< (int)(sizeof(device->services) / 2))
						device->services[device->serviceCount++] = uuid;
				}
				break;

			case 0x19:	/* appearance */
				if (valueLength >= 2) {
					device->appearance = value[0] | (value[1] << 8);
					device->hasAppearance = true;
				}
				break;
		}

		offset += fieldLength + 1;
	}
}


static void
HandleAdvertisingReport(const uint8* data, uint8 length)
{
	if (length < 1)
		return;

	uint8 reports = data[0];
	uint8 offset = 1;

	for (uint8 i = 0; i < reports; i++) {
		if (offset + 9 > length)
			return;

		uint8 eventType = data[offset];
		uint8 addressType = data[offset + 1];
		const uint8* address = data + offset + 2;
		uint8 dataLength = data[offset + 8];

		if (offset + 9 + dataLength + 1 > length)
			return;

		const uint8* advertising = data + offset + 9;
		int8 rssi = (int8)data[offset + 9 + dataLength];

		seen_device* device = FindOrAddDevice(address, addressType);
		if (device != NULL) {
			device->reports++;
			device->rssi = rssi;
			/* 0x00 is a connectable undirected advertisement, 0x01 a
			 * connectable directed one, 0x04 a scan response.
			 */
			if (eventType == 0x00 || eventType == 0x01)
				device->connectable = true;
			if (eventType == 0x04)
				device->scanResponse = true;
			ParseAdvertisingData(device, advertising, dataLength);
		}

		offset += 9 + dataLength + 1;
	}
}


/* Read events until the answer to this opcode turns up, handling anything else
 * that arrives on the way. Returns the controller's status byte, or -1.
 */
static int
WaitForCommand(int fd, uint16 opcode, uint8* returnParameters,
	size_t* returnLength)
{
	uint8 event[MAX_EVENT];

	for (int attempt = 0; attempt < 64; attempt++) {
		ssize_t received = ReadEvent(fd, event, sizeof(event));
		if (received < 3)
			return -1;

		if (sVerbose)
			Dump("event", event, received);

		uint8 code = event[0];
		uint8 parameterLength = event[1];

		if (code == HCI_EVENT_COMMAND_COMPLETE && parameterLength >= 3) {
			uint16 answered = event[3] | (event[4] << 8);
			if (answered != opcode)
				continue;

			size_t length = received - 5;
			if (returnParameters != NULL && returnLength != NULL) {
				if (length > *returnLength)
					length = *returnLength;
				memcpy(returnParameters, event + 5, length);
				*returnLength = length;
			}
			return length > 0 ? event[5] : 0;
		}

		if (code == HCI_EVENT_COMMAND_STATUS && parameterLength >= 4) {
			uint16 answered = event[4] | (event[5] << 8);
			if (answered != opcode)
				continue;
			return event[2];
		}

		if (code == HCI_EVENT_LE_META && parameterLength >= 1
			&& event[2] == LE_SUBEVENT_ADVERTISING_REPORT) {
			HandleAdvertisingReport(event + 3, (uint8)(received - 3));
		}
	}

	return -1;
}


static const char*
ErrorName(int status)
{
	switch (status) {
		case 0x00: return "ok";
		case 0x01: return "unknown command";
		case 0x02: return "no connection";
		case 0x0c: return "command disallowed in this state";
		case 0x11: return "unsupported feature or parameter";
		case 0x12: return "invalid parameters";
		case 0x1a: return "unsupported remote feature";
		case -1: return "no answer";
	}
	return "error";
}


static bool
Command(int fd, const char* what, uint16 opcode, const uint8* parameters,
	uint8 length, uint8* returnParameters = NULL, size_t* returnLength = NULL)
{
	printf("%-34s", what);
	fflush(stdout);

	if (SendCommand(fd, opcode, parameters, length) != B_OK) {
		printf(" the radio would not take it\n");
		return false;
	}

	int status = WaitForCommand(fd, opcode, returnParameters, returnLength);
	if (status != 0) {
		printf(" %s (%#02x)\n", ErrorName(status), status < 0 ? 0 : status);
		return false;
	}

	printf(" ok\n");
	return true;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char* path = "/dev/bus/usb/0/20";
	int seconds = 10;
	bool active = true;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
			path = argv[++i];
		else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (strcmp(argv[i], "-p") == 0)
			active = false;
		else if (strcmp(argv[i], "-v") == 0)
			sVerbose = true;
		else {
			fprintf(stderr, "usage: %s [-d device] [-t seconds] [-p] [-v]\n",
				argv[0]);
			return 1;
		}
	}

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "[!] cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}
	printf("radio: %s\n", path);

	if (FindEndpoints(fd, true) != B_OK) {
		fprintf(stderr, "[!] this device has no interrupt endpoint to read"
			" events from\n");
		close(fd);
		return 1;
	}

	/* Start from a known state. Everything below assumes the controller has
	 * forgotten whatever the stack last told it.
	 */
	if (!Command(fd, "resetting the controller", HCI_RESET, NULL, 0)) {
		fprintf(stderr, "\n[!] the controller does not answer at all. It has"
			" probably not been given its firmware - run btfw first.\n");
		close(fd);
		return 1;
	}

	uint8 answer[64];
	size_t answerLength;

	answerLength = sizeof(answer);
	if (Command(fd, "asking its version", HCI_READ_LOCAL_VERSION, NULL, 0,
			answer, &answerLength) && answerLength >= 8) {
		printf("  HCI version %u, LMP version %u, made by %u\n", answer[1],
			answer[4], (unsigned)(answer[5] | (answer[6] << 8)));
	}

	/* Bit 38 of the controller's feature list is LE support, and bit 39 says
	 * it can do LE and classic at the same time. A controller without the
	 * first has no LE half at all and nothing below will work.
	 */
	answerLength = sizeof(answer);
	if (Command(fd, "asking what it supports", HCI_READ_LOCAL_FEATURES, NULL, 0,
			answer, &answerLength) && answerLength >= 9) {
		bool le = (answer[4] & 0x40) != 0;
		bool both = (answer[4] & 0x80) != 0;
		printf("  Low Energy: %s; LE and classic together: %s\n",
			le ? "yes" : "NO", both ? "yes" : "no");
		if (!le) {
			fprintf(stderr, "\n[!] this controller has no LE support.\n");
			close(fd);
			return 1;
		}
	}

	/* The controller reports LE events only if the host unmasks them, and it
	 * only accepts LE commands at all once the host says it speaks LE. Both
	 * are off after a reset, which is why a stack that does not know about LE
	 * sees none of this.
	 */
	const uint8 eventMask[8]
		= { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
	Command(fd, "unmasking the LE event", HCI_SET_EVENT_MASK, eventMask, 8);

	const uint8 leHost[2] = { 0x01, 0x00 };
	Command(fd, "saying the host speaks LE", HCI_WRITE_LE_HOST_SUPPORT,
		leHost, 2);

	answerLength = sizeof(answer);
	if (Command(fd, "asking its LE buffer size", HCI_LE_READ_BUFFER_SIZE,
			NULL, 0, answer, &answerLength) && answerLength >= 4) {
		uint16 packetLength = answer[1] | (answer[2] << 8);
		printf("  %u packets of %u bytes%s\n", answer[3], packetLength,
			packetLength == 0 && answer[3] == 0
				? " (shares the classic buffers)" : "");
	}

	answerLength = sizeof(answer);
	if (Command(fd, "asking its LE features", HCI_LE_READ_LOCAL_FEATURES,
			NULL, 0, answer, &answerLength) && answerLength >= 9) {
		printf("  features %02x %02x %02x %02x %02x %02x %02x %02x\n",
			answer[1], answer[2], answer[3], answer[4], answer[5], answer[6],
			answer[7], answer[8]);
		printf("  encryption: %s, LE Secure Connections: %s,"
			" data length extension: %s\n",
			(answer[1] & 0x01) ? "yes" : "no",
			(answer[2] & 0x08) ? "yes" : "no",
			(answer[1] & 0x20) ? "yes" : "no");
	}

	/* Scan parameters: how often to listen, for how long, and whether to ask
	 * each advertiser for more. Active scanning gets the scan response, which
	 * is where most devices put their name, at the cost of transmitting.
	 */
	uint8 scanParameters[7];
	scanParameters[0] = active ? 0x01 : 0x00;
	scanParameters[1] = 0x60;	/* interval, 60 * 0.625ms = 60ms */
	scanParameters[2] = 0x00;
	scanParameters[3] = 0x30;	/* window, 30ms of every 60 */
	scanParameters[4] = 0x00;
	scanParameters[5] = 0x00;	/* our own address: the public one */
	scanParameters[6] = 0x00;	/* report everything, filter nothing */
	Command(fd, "setting the scan up", HCI_LE_SET_SCAN_PARAMETERS,
		scanParameters, 7);

	const uint8 scanOn[2] = { 0x01, 0x00 };
		/* duplicates are not filtered: a device's name often arrives in a
		 * later report than its address did.
		 */
	if (!Command(fd, "starting the scan", HCI_LE_SET_SCAN_ENABLE, scanOn, 2)) {
		close(fd);
		return 1;
	}

	printf("\nlistening for %d seconds%s...\n", seconds,
		active ? "" : " (passive)");

	bigtime_t end = system_time() + (bigtime_t)seconds * 1000000;
	uint8 event[MAX_EVENT];
	int events = 0;

	while (system_time() < end) {
		ssize_t received = ReadEvent(fd, event, sizeof(event));
		if (received < 2)
			break;
		events++;

		if (sVerbose)
			Dump("event", event, received);

		if (event[0] == HCI_EVENT_LE_META && received >= 3
			&& event[2] == LE_SUBEVENT_ADVERTISING_REPORT) {
			HandleAdvertisingReport(event + 3, (uint8)(received - 3));
		}
	}

	const uint8 scanOff[2] = { 0x00, 0x00 };
	SendCommand(fd, HCI_LE_SET_SCAN_ENABLE, scanOff, 2);
	WaitForCommand(fd, HCI_LE_SET_SCAN_ENABLE, NULL, NULL);

	printf("\n%d events read, %d device%s seen\n\n", events, sDeviceCount,
		sDeviceCount == 1 ? "" : "s");

	for (int i = 0; i < sDeviceCount; i++) {
		seen_device* device = &sDevices[i];
		printf("%02x:%02x:%02x:%02x:%02x:%02x  %-16s %4d dBm  %s%s\n",
			device->address[5], device->address[4], device->address[3],
			device->address[2], device->address[1], device->address[0],
			AddressTypeName(device->addressType), device->rssi,
			device->connectable ? "connectable" : "not connectable",
			device->scanResponse ? ", answered a scan" : "");

		if (device->name[0] != '\0')
			printf("    name:       %s\n", device->name);
		if (device->hasAppearance) {
			printf("    appearance: %#06x %s\n", device->appearance,
				AppearanceName(device->appearance));
		}
		if (device->serviceCount > 0) {
			printf("    services:  ");
			for (int j = 0; j < device->serviceCount; j++) {
				const char* name = ServiceName(device->services[j]);
				printf(" %04x%s%s%s", device->services[j],
					name != NULL ? " (" : "", name != NULL ? name : "",
					name != NULL ? ")" : "");
			}
			printf("\n");
		}
	}

	/* Say plainly whether anything here is a mouse: either it advertises the
	 * human interface service or it says it looks like one.
	 */
	printf("\n");
	bool foundHid = false;
	for (int i = 0; i < sDeviceCount; i++) {
		seen_device* device = &sDevices[i];
		bool hidService = false;
		for (int j = 0; j < device->serviceCount; j++) {
			if (device->services[j] == 0x1812)
				hidService = true;
		}
		bool hidAppearance = device->hasAppearance
			&& (device->appearance & 0xffc0) == 0x03c0;

		if (hidService || hidAppearance) {
			foundHid = true;
			printf("a human interface device: %02x:%02x:%02x:%02x:%02x:%02x"
				" (%s)%s%s\n", device->address[5], device->address[4],
				device->address[3], device->address[2], device->address[1],
				device->address[0], AddressTypeName(device->addressType),
				device->name[0] != '\0' ? ", " : "", device->name);
		}
	}
	if (!foundHid)
		printf("nothing nearby advertises itself as a keyboard or mouse\n");

	close(fd);
	return 0;
}
