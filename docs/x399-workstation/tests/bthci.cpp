/* Watch one HCI command go down to the radio and its answer come back up.
 *
 * The Bluetooth server waits for an answer that never arrives, and there are
 * four places it could be getting lost: the driver's write, the radio, the
 * driver's read, or the hand-off from the kernel to userland. The server shows
 * none of them.
 *
 * So this does what the server does, but in the open. It takes the kernel's
 * side of that last hand-off by creating the port the kernel looks for by
 * name, opens the transport itself, sends a single command, and waits on the
 * port. Then it reads the driver's own counters, which say how many transfers
 * it offered, how many the bus took, and how many came back.
 *
 * Between the answer and the counters there is no room left for the fault to
 * hide: either the event arrives here, in which case everything below the
 * server works and the server is at fault, or the counters say which end of
 * the driver is stuck.
 *
 * Run this with the Bluetooth server stopped - the kernel finds that port by
 * name, and two of them would be one too many.
 *
 * usage: bthci [device] [opcode-hex]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <OS.h>

#include <bluetooth/HCI/btHCI_transport.h>

#define HCI_RESET		0x0c03
#define HCI_READ_BD_ADDR	0x1009


static void
Dump(const char* what, const uint8* data, ssize_t length)
{
	printf("  %s (%ld bytes):", what, (long)length);
	for (ssize_t i = 0; i < length && i < 32; i++)
		printf(" %02x", data[i]);
	if (length > 32)
		printf(" ...");
	printf("\n");
}


static void
ShowStatistics(int fd, const char* when)
{
	bt_hci_statistics stats;
	memset(&stats, 0, sizeof(stats));

	if (ioctl(fd, GET_STATS, &stats, sizeof(stats)) != 0) {
		printf("  (the driver would not give up its counters: %s)\n",
			strerror(errno));
		return;
	}

	printf("%s\n", when);
	printf("  sending:  %u offered, %u refused by the bus, %u completed,"
		" %u failed\n", stats.acceptedTX, stats.rejectedTX,
		stats.successfulTX, stats.errorTX);
	printf("  reading:  %u offered, %u refused by the bus, %u completed,"
		" %u failed\n", stats.acceptedRX, stats.rejectedRX,
		stats.successfulRX, stats.errorRX);
	printf("  commands sent %u, events read %u\n",
		stats.commandTX, stats.eventRX);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* path = argc > 1 ? argv[1] : "/dev/bluetooth/h2/h2generic/0";
	uint16 opcode = argc > 2 ? (uint16)strtol(argv[2], NULL, 16) : HCI_RESET;

	/* Stand in for the server: the kernel hands events to whoever owns a port
	 * of this name.
	 */
	if (find_port(BT_USERLAND_PORT_NAME) >= 0) {
		fprintf(stderr, "[!] something already owns \"%s\" - stop the"
			" Bluetooth server first\n", BT_USERLAND_PORT_NAME);
		return 1;
	}

	port_id port = create_port(16, BT_USERLAND_PORT_NAME);
	if (port < 0) {
		fprintf(stderr, "[!] cannot create the event port: %s\n",
			strerror(port));
		return 1;
	}
	printf("listening on \"%s\"\n", BT_USERLAND_PORT_NAME);

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "[!] cannot open %s: %s\n", path, strerror(errno));
		delete_port(port);
		return 1;
	}
	printf("opened %s\n", path);

	hci_id id = 0;
	if (ioctl(fd, GET_HCI_ID, &id, sizeof(id)) == 0)
		printf("the stack calls this adapter %d\n", (int)id);

	/* Bring the transport up: this is what starts the driver reading. */
	uint32 dummy = 0;
	if (ioctl(fd, BT_UP, &dummy, sizeof(dummy)) != 0) {
		fprintf(stderr, "[!] the driver would not come up: %s\n",
			strerror(errno));
		close(fd);
		delete_port(port);
		return 1;
	}
	printf("the transport is up\n\n");

	ShowStatistics(fd, "before sending anything:");

	uint8 command[3];
	command[0] = opcode & 0xff;
	command[1] = opcode >> 8;
	command[2] = 0;

	printf("\nsending HCI command %#06x\n", opcode);
	Dump("command", command, sizeof(command));

	if (ioctl(fd, ISSUE_BT_COMMAND, command, sizeof(command)) != 0) {
		fprintf(stderr, "[!] the driver would not take the command: %s\n",
			strerror(errno));
		ShowStatistics(fd, "\nafter the refusal:");
		close(fd);
		delete_port(port);
		return 1;
	}
	printf("the driver took it\n\n");

	/* Now wait on the port the way the server does. */
	uint8 event[512];
	int32 code = 0;
	printf("waiting up to 5 seconds for the kernel to hand up an event...\n");

	ssize_t received = read_port_etc(port, &code, event, sizeof(event),
		B_TIMEOUT, 5 * 1000 * 1000);

	if (received < 0) {
		printf("[!] nothing came up: %s\n", strerror(received));
	} else {
		printf("the kernel handed up %ld bytes, type %u, adapter %u\n",
			(long)received, (unsigned)GET_PORTCODE_TYPE(code),
			(unsigned)GET_PORTCODE_HID(code));
		Dump("event", event, received);

		if (received >= 6 && event[0] == 0x0e) {
			printf("  Command Complete for %#06x, status %#04x (%s)\n",
				(unsigned)(event[3] | (event[4] << 8)), event[5],
				event[5] == 0 ? "success" : "error");
		}
	}

	printf("\n");
	ShowStatistics(fd, "after:");

	close(fd);
	delete_port(port);
	return received < 0 ? 1 : 0;
}
