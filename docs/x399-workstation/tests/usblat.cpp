// usblat <device> [count]: time several standard control requests to one
// USB device - short and long data stages - to tell a fixed cost per
// transfer from a cost per packet.
#include <USBKit.h>
#include <OS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
measure(BUSBDevice& device, const char* what, uint8 type, uint8 request,
	uint16 value, uint16 index, uint16 length, int count)
{
	uint8 buffer[1024];
	bigtime_t best = B_INFINITE_TIMEOUT, total = 0;
	ssize_t got = 0;
	for (int i = 0; i < count; i++) {
		bigtime_t start = system_time();
		got = device.ControlTransfer(type, request, value, index, length,
			buffer);
		bigtime_t took = system_time() - start;
		if (got < 0) {
			printf("%-28s failed: %s\n", what, strerror(got));
			return;
		}
		total += took;
		if (took < best)
			best = took;
	}
	printf("%-28s %4zd bytes: mean %5.0f us, best %5lld us\n", what, got,
		(double)total / count, (long long)best);
}

int
main(int argc, char** argv)
{
	int count = argc > 2 ? atoi(argv[2]) : 100;
	BUSBDevice device(argv[1]);
	if (device.InitCheck() != B_OK) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	measure(device, "device descriptor, 8", 0x80, 6, 0x0100, 0, 8, count);
	measure(device, "device descriptor, 18", 0x80, 6, 0x0100, 0, 18, count);
	measure(device, "config descriptor, 9", 0x80, 6, 0x0200, 0, 9, count);
	measure(device, "config descriptor, all", 0x80, 6, 0x0200, 0, 1024, count);
	measure(device, "string 0", 0x80, 6, 0x0300, 0, 255, count);
	measure(device, "get status", 0x80, 0, 0, 0, 2, count);
	return 0;
}
