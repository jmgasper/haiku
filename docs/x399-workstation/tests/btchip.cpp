/* Ask the MediaTek radio which chip it is.
 *
 * The controller ignores standard Bluetooth commands until its firmware is
 * downloaded, so the usual "say hello" does not work on a cold radio. But the
 * bootloader does answer vendor control reads of its registers, and the chip
 * identifier sits at a known address. Reading it proves the radio is reachable
 * and tells us which firmware it wants, without sending anything that could
 * leave a pipe waiting for an answer that never comes.
 *
 * A control read is safe in a way an interrupt read is not: the device either
 * answers it or refuses it, where an interrupt endpoint with nothing to say
 * simply stays silent and the read waits for ever.
 *
 * usage: btchip [/dev/bus/usb/0/20] [register-hex ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <OS.h>

#include "usb_raw.h"

/* The bootloader's register window. 0x70010200 holds the chip identifier;
 * 0x70010204 holds its revision on the parts that have one.
 */
#define MTK_REG_CHIP_ID		0x70010200
#define MTK_REG_CHIP_REV	0x70010204

#define MTK_VENDOR_READ		0x63	/* bRequest for a register read */
#define USB_VENDOR_IN		0xc0	/* vendor request, device, IN */


static bool
ReadRegister(int fd, uint32 reg, uint32* out)
{
	uint8 buffer[4] = { 0, 0, 0, 0 };
	usb_raw_command command;

	memset(&command, 0, sizeof(command));
	command.control.request_type = USB_VENDOR_IN;
	command.control.request = MTK_VENDOR_READ;
	command.control.value = reg >> 16;
	command.control.index = reg & 0xffff;
	command.control.length = sizeof(buffer);
	command.control.data = buffer;

	if (ioctl(fd, B_USB_RAW_COMMAND_CONTROL_TRANSFER, &command,
			sizeof(command)) != 0) {
		printf("  %#010x: the ioctl failed: %s\n", (unsigned)reg,
			strerror(errno));
		return false;
	}

	if (command.control.status != B_USB_RAW_STATUS_SUCCESS) {
		printf("  %#010x: the radio refused the read (status %d)\n",
			(unsigned)reg, (int)command.control.status);
		return false;
	}

	*out = buffer[0] | (buffer[1] << 8) | (buffer[2] << 16)
		| ((uint32)buffer[3] << 24);
	printf("  %#010x = %#010x\n", (unsigned)reg, (unsigned)*out);
	return true;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char* path = argc > 1 ? argv[1] : "/dev/bus/usb/0/20";

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "[!] cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}
	printf("reading registers from %s\n", path);

	if (argc > 2) {
		for (int i = 2; i < argc; i++) {
			uint32 value = 0;
			ReadRegister(fd, (uint32)strtoul(argv[i], NULL, 16), &value);
		}
		close(fd);
		return 0;
	}

	uint32 chipId = 0;
	if (!ReadRegister(fd, MTK_REG_CHIP_ID, &chipId)) {
		printf("\nthe bootloader would not answer, so either this is not a\n"
			"MediaTek part or it does not take register reads this way.\n");
		close(fd);
		return 1;
	}

	uint32 revision = 0;
	ReadRegister(fd, MTK_REG_CHIP_REV, &revision);

	/* The identifier is the bare part number, and the revision's low half
	 * carries the hardware version that picks which firmware fits: a firmware
	 * file names its own version in its header, and the two must agree.
	 */
	unsigned part = chipId & 0xffff;
	printf("\nthis is a MediaTek MT%04x, hardware version %#04x\n",
		part, (unsigned)(revision & 0xff));
	if (part == 0x7922) {
		printf("it wants BT_RAM_CODE_MT7922_1_1_hdr.bin,"
			" whose header must say version %#04x too\n",
			(unsigned)(revision & 0xff));
	}

	close(fd);
	return 0;
}
