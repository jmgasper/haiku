// usbportstat <root hub device> <ports>: hub GET_STATUS for each port, printing
// connection and speed as the hub reports them.
#include <USBKit.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char** argv)
{
	BUSBDevice hub(argv[1]);
	int ports = atoi(argv[2]);
	for (int port = 1; port <= ports; port++) {
		uint16 status[2] = { 0, 0 };
		if (hub.ControlTransfer(0xa3, 0, 0, port, 4, status) < 0)
			continue;
		if ((status[0] & 1) == 0)
			continue;
		printf("port %2d: status 0x%04x change 0x%04x: %s\n", port, status[0],
			status[1], (status[0] & (1 << 10)) ? "high speed"
				: (status[0] & (1 << 9)) ? "low speed"
				: (status[0] & (1 << 11)) ? "(bit 11 set)" : "full speed");
	}
	return 0;
}
