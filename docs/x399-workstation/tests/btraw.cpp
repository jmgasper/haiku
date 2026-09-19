/* Talk to the Bluetooth radio directly, with no stack in the way.
 *
 * The Bluetooth stack says its commands are rejected, and the kernel says the
 * USB control endpoint stalled. Either the radio refuses the command or we are
 * asking wrongly, and the stack sits too far above the wire to tell which.
 *
 * So this asks the radio itself. A Bluetooth USB device takes HCI commands as
 * class control transfers on endpoint zero and answers with events on the
 * interrupt endpoint - that is the whole transport. Sending HCI Reset (0x0c03)
 * and waiting for the Command Complete that must follow is the smallest
 * question you can put to the hardware.
 *
 * usage: btraw [/dev/bus/usb/0/20] [opcode-hex]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <OS.h>

#include "usb_raw.h"

#define HCI_RESET		0x0c03
#define HCI_READ_LOCAL_VERSION	0x1001
#define HCI_READ_BD_ADDR	0x1009


static const char*
StatusName(status_t status)
{
	switch (status) {
		case B_USB_RAW_STATUS_SUCCESS: return "ok";
		case B_USB_RAW_STATUS_FAILED: return "failed";
		case B_USB_RAW_STATUS_ABORTED: return "aborted";
		case B_USB_RAW_STATUS_STALLED: return "STALLED";
		case B_USB_RAW_STATUS_CRC_ERROR: return "CRC error";
		case B_USB_RAW_STATUS_TIMEOUT: return "timed out";
		case B_USB_RAW_STATUS_INVALID_CONFIGURATION: return "bad configuration";
		case B_USB_RAW_STATUS_INVALID_INTERFACE: return "bad interface";
		case B_USB_RAW_STATUS_INVALID_ENDPOINT: return "bad endpoint";
		case B_USB_RAW_STATUS_INVALID_STRING: return "bad string";
		case B_USB_RAW_STATUS_NO_MEMORY: return "out of memory";
	}
	return "?";
}


static void
Dump(const char* what, const uint8* data, int length)
{
	printf("  %s (%d bytes):", what, length);
	for (int i = 0; i < length; i++)
		printf(" %02x", data[i]);
	printf("\n");
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* path = argc > 1 ? argv[1] : "/dev/bus/usb/0/20";
	uint16 opcode = argc > 2 ? (uint16)strtol(argv[2], NULL, 16) : HCI_RESET;

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "[!] cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}
	printf("opened %s\n", path);

	usb_raw_command command;

	memset(&command, 0, sizeof(command));
	if (ioctl(fd, B_USB_RAW_COMMAND_GET_VERSION, &command, sizeof(command))
			== 0) {
		printf("raw usb protocol version %#x\n", command.version.status);
	}

	/* An HCI command packet: opcode little endian, then the parameter length.
	 * Reset and the read commands all take no parameters.
	 */
	uint8 packet[3];
	packet[0] = opcode & 0xff;
	packet[1] = opcode >> 8;
	packet[2] = 0;

	printf("sending HCI command %#06x\n", opcode);
	Dump("command", packet, sizeof(packet));

	memset(&command, 0, sizeof(command));
	command.control.request_type = 0x20;	/* class, to the device */
	command.control.request = 0x00;
	command.control.value = 0;
	command.control.index = 0;
	command.control.length = sizeof(packet);
	command.control.data = packet;

	if (ioctl(fd, B_USB_RAW_COMMAND_CONTROL_TRANSFER, &command,
			sizeof(command)) != 0) {
		fprintf(stderr, "[!] the ioctl itself failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("the radio %s the command: %s\n",
		command.control.status == B_USB_RAW_STATUS_SUCCESS
			? "took" : "refused",
		StatusName(command.control.status));

	if (command.control.status != B_USB_RAW_STATUS_SUCCESS) {
		printf("\nthe command never reached the controller, so there is\n"
			"nothing to wait for. The transport is the problem.\n");
		close(fd);
		return 1;
	}

	/* The answer comes back as an event on the interrupt endpoint. Interface 0
	 * endpoint 0 is that endpoint on every Bluetooth class device.
	 */
	uint8 event[260];
	memset(event, 0, sizeof(event));

	memset(&command, 0, sizeof(command));
	command.transfer.interface = 0;
	command.transfer.endpoint = 0;
	command.transfer.data = event;
	command.transfer.length = sizeof(event);

	printf("waiting for the event that answers it...\n");
	if (ioctl(fd, B_USB_RAW_COMMAND_INTERRUPT_TRANSFER, &command,
			sizeof(command)) != 0) {
		fprintf(stderr, "[!] the read ioctl failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	if (command.transfer.status != B_USB_RAW_STATUS_SUCCESS) {
		printf("[!] no event came back: %s\n",
			StatusName(command.transfer.status));
		close(fd);
		return 1;
	}

	int length = (int)command.transfer.length;
	Dump("event", event, length);

	/* Command Complete is event 0x0e: one byte of credits, the opcode it is
	 * answering, then the command's own return parameters, the first of which
	 * is a status byte where zero means it worked.
	 */
	if (length >= 6 && event[0] == 0x0e) {
		uint16 answered = event[3] | (event[4] << 8);
		printf("Command Complete for %#06x, status %#04x (%s)\n",
			answered, event[5], event[5] == 0 ? "success" : "error");
		if (answered == HCI_READ_BD_ADDR && length >= 12) {
			printf("adapter address %02x:%02x:%02x:%02x:%02x:%02x\n",
				event[11], event[10], event[9],
				event[8], event[7], event[6]);
		}
		if (answered == HCI_READ_LOCAL_VERSION && length >= 14) {
			printf("HCI version %u, LMP version %u, manufacturer %u\n",
				event[6], event[9], (unsigned)(event[10] | (event[11] << 8)));
		}
		close(fd);
		return event[5] == 0 ? 0 : 1;
	}

	printf("that is not a Command Complete event\n");
	close(fd);
	return 1;
}
