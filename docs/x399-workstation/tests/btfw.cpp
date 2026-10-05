/* Wake a MediaTek Bluetooth radio up.
 *
 * An MT7922 comes out of reset running a bootloader, not a Bluetooth
 * controller. It answers vendor register reads and its own download protocol,
 * and nothing else - send it the ordinary HCI Reset that every Bluetooth stack
 * starts with and it will take the packet and never reply. That is exactly
 * what this machine's stack ran into. The radio only becomes a Bluetooth
 * controller once its firmware has been handed to it.
 *
 * So this hands it the firmware. It reads the vendor's image, walks the
 * sections meant to be downloaded, and feeds each one to the radio in 250 byte
 * chunks wrapped in MediaTek's WMT protocol, which rides inside a vendor HCI
 * command. When the last chunk is in, it tells the radio to start being a
 * Bluetooth controller.
 *
 * Commands go out the way every HCI command does on a USB Bluetooth device, as
 * a class control transfer on endpoint zero. The answers do not come back on
 * the event endpoint, though - they are collected by a vendor control read,
 * which returns nothing at all until the radio has something to say. That is a
 * kindness here: a control transfer always completes, where a read of the
 * event endpoint with nothing to report waits for ever and cannot be
 * interrupted.
 *
 * This is a bench tool, for establishing the sequence against real hardware
 * where a wrong step costs a power cycle. The driver is where this belongs
 * once the sequence is known to work.
 *
 * usage: btfw [-n] [-v] [device] [firmware]
 *        -n  say what would be sent, send nothing
 *        -v  show the first packets byte for byte
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <OS.h>

#include "usb_raw.h"

/* The bootloader's register windows. */
#define MTK_REG_LEGACY_ID	0x80000008
#define MTK_REG_CHIP_ID		0x70010200
#define MTK_REG_FW_VERSION	0x80021004
#define MTK_REG_FW_FLAVOR	0x70010020
#define MTK_REG_EP_RST_OPT	0x74011890
#define MTK_EP_RST_IN_OUT_OPT	0x00010001

#define MTK_VENDOR_READ		0x63	/* bRequest, with type 0xc0 */
#define MTK_UHW_WRITE		0x02	/* bRequest, with type 0x5e */
#define MTK_WMT_READ		0x01	/* bRequest, with type 0xc0, value 48 */

#define USB_VENDOR_IN		0xc0
#define USB_UHW_OUT		0x5e
#define USB_CLASS_OUT		0x20

/* The vendor HCI command everything below travels inside. */
#define HCI_OPCODE_WMT		0xfc6f
#define HCI_EVENT_WMT		0xe4

#define WMT_PATCH_DOWNLOAD	0x01
#define WMT_FUNC_CTRL		0x06

/* A section whose type's low half is this carries code for the radio; the
 * others describe the image rather than being part of it.
 */
#define SECTION_TYPE_BIN	0x0002

#define PATCH_HEADER_SIZE	32
#define GLOBAL_DESC_SIZE	64
#define SECTION_MAP_SIZE	64
#define SECTION_MAP_COMMON	12	/* type, offset and size stay behind */
#define SECTION_HEADER_BYTES	52	/* the rest is handed to the radio */

#define CHUNK_SIZE		250	/* all an HCI packet length byte allows */

#define WMT_REPLY_MAX		64
#define WMT_POLL_GAP		500		/* microseconds between polls */
#define WMT_TIMEOUT		10000000	/* ten seconds, as Linux uses */

static bool sDryRun = false;
static bool sVerbose = false;
static int sPacketsShown = 0;


static uint32
ReadLE32(const uint8* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32)p[3] << 24);
}


static void
Dump(const char* what, const uint8* data, size_t length)
{
	printf("    %s (%zu bytes):", what, length);
	for (size_t i = 0; i < length && i < 32; i++)
		printf(" %02x", data[i]);
	if (length > 32)
		printf(" ...");
	printf("\n");
}


static status_t
ControlTransfer(int fd, uint8 type, uint8 request, uint16 value, uint16 index,
	void* data, uint16 length, size_t* actual)
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
			sizeof(command)) != 0) {
		return errno != 0 ? (status_t)errno : B_ERROR;
	}
	if (command.control.status != B_USB_RAW_STATUS_SUCCESS)
		return B_IO_ERROR;

	if (actual != NULL)
		*actual = command.control.length;
	return B_OK;
}


static status_t
ReadRegister(int fd, uint32 reg, uint32* out)
{
	uint8 buffer[4] = { 0, 0, 0, 0 };
	status_t status = ControlTransfer(fd, USB_VENDOR_IN, MTK_VENDOR_READ,
		reg >> 16, reg & 0xffff, buffer, sizeof(buffer), NULL);
	if (status != B_OK)
		return status;

	*out = ReadLE32(buffer);
	return B_OK;
}


/* The reset option register lives behind a second, stranger control channel of
 * its own, with request types that are not any standard combination. The bytes
 * go out exactly as given.
 */
static status_t
WriteResetOption(int fd, uint32 reg, uint32 value)
{
	uint8 buffer[4];
	buffer[0] = value & 0xff;
	buffer[1] = (value >> 8) & 0xff;
	buffer[2] = (value >> 16) & 0xff;
	buffer[3] = (value >> 24) & 0xff;

	if (sDryRun)
		return B_OK;

	return ControlTransfer(fd, USB_UHW_OUT, MTK_UHW_WRITE, reg >> 16,
		reg & 0xffff, buffer, sizeof(buffer), NULL);
}


/* Send one WMT command. The packet is an HCI command whose payload is a WMT
 * header and then the command's own data:
 *
 *   6f fc <plen> | <dir> <op> <dlen lo> <dlen hi> <flag> | <data...>
 *
 * where plen counts everything after it, and dlen counts the flag byte and the
 * data - the two lengths overlap, which is easy to get wrong.
 */
static status_t
SendWmt(int fd, uint8 op, uint8 flag, const uint8* data, size_t dlen)
{
	if (dlen > CHUNK_SIZE)
		return B_BAD_VALUE;

	uint8 packet[3 + 5 + CHUNK_SIZE];
	packet[0] = HCI_OPCODE_WMT & 0xff;
	packet[1] = HCI_OPCODE_WMT >> 8;
	packet[2] = (uint8)(5 + dlen);
	packet[3] = 0x01;			/* host to device */
	packet[4] = op;
	packet[5] = (uint8)((dlen + 1) & 0xff);
	packet[6] = (uint8)((dlen + 1) >> 8);
	packet[7] = flag;
	if (dlen > 0)
		memcpy(packet + 8, data, dlen);

	size_t total = 8 + dlen;

	if (sVerbose && sPacketsShown < 6) {
		printf("  -> WMT op %#x flag %u, %zu bytes of data\n", op, flag, dlen);
		Dump("sent", packet, total);
		sPacketsShown++;
	}

	if (sDryRun)
		return B_OK;

	return ControlTransfer(fd, USB_CLASS_OUT, 0x00, 0, 0, packet,
		(uint16)total, NULL);
}


/* Collect the answer. The radio returns nothing until it has something to say,
 * so an empty read means "not yet" rather than an error.
 */
static status_t
ReadWmt(int fd, uint8 expectedOp, uint8* outFlag, uint8* reply,
	size_t* replyLength)
{
	if (sDryRun) {
		*outFlag = 0;
		if (replyLength != NULL)
			*replyLength = 0;
		return B_OK;
	}

	uint8 buffer[WMT_REPLY_MAX];
	bigtime_t deadline = system_time() + WMT_TIMEOUT;

	while (system_time() < deadline) {
		size_t actual = 0;
		memset(buffer, 0, sizeof(buffer));

		status_t status = ControlTransfer(fd, USB_VENDOR_IN, MTK_WMT_READ,
			48, 0, buffer, sizeof(buffer), &actual);
		if (status != B_OK)
			return status;

		/* Nothing to say yet. The radio shows this either by answering with
		 * no bytes at all or by handing back a buffer it has not written an
		 * event into, which happens while it is finishing a section - so an
		 * answer that is not an event is a "wait", not a failure.
		 */
		if (actual < 7 || buffer[0] != HCI_EVENT_WMT) {
			snooze(WMT_POLL_GAP);
			continue;
		}

		if (sVerbose && sPacketsShown <= 6) {
			Dump("reply", buffer, actual);
			sPacketsShown++;
		}

		if (buffer[3] != expectedOp) {
			fprintf(stderr, "[!] answer is for WMT op %#x, we asked %#x\n",
				buffer[3], expectedOp);
			return B_IO_ERROR;
		}

		*outFlag = buffer[6];
		if (reply != NULL && replyLength != NULL) {
			size_t copy = actual < *replyLength ? actual : *replyLength;
			memcpy(reply, buffer, copy);
			*replyLength = actual;
		}
		return B_OK;
	}

	fprintf(stderr, "[!] the radio never answered WMT op %#x\n", expectedOp);
	return B_TIMED_OUT;
}


static status_t
DoWmt(int fd, uint8 op, uint8 flag, const uint8* data, size_t dlen,
	uint8* outFlag)
{
	status_t status = SendWmt(fd, op, flag, data, dlen);
	if (status != B_OK) {
		fprintf(stderr, "[!] sending WMT op %#x: %s\n", op, strerror(status));
		return status;
	}
	return ReadWmt(fd, op, outFlag, NULL, NULL);
}


/* Offer one section to the radio. The radio may say it already has it, in
 * which case its contents are not sent at all - which is what makes a second
 * run cheap.
 */
static status_t
DownloadSection(int fd, const uint8* image, size_t imageSize, uint32 index,
	bool* skipped)
{
	size_t mapOffset = PATCH_HEADER_SIZE + GLOBAL_DESC_SIZE
		+ index * SECTION_MAP_SIZE;
	const uint8* map = image + mapOffset;

	uint32 type = ReadLE32(map);
	uint32 offset = ReadLE32(map + 4);
	uint32 size = ReadLE32(map + 8);
	uint32 loadAddress = ReadLE32(map + 12);
	uint32 loadSize = ReadLE32(map + 16);

	*skipped = true;

	if ((type & 0xffff) != SECTION_TYPE_BIN || loadSize == 0) {
		printf("section %u: nothing to download\n", (unsigned)index);
		return B_OK;
	}
	if ((size_t)offset + size > imageSize) {
		fprintf(stderr, "[!] section %u runs past the end of the file\n",
			(unsigned)index);
		return B_BAD_DATA;
	}

	printf("section %u: %u bytes for %#x\n", (unsigned)index,
		(unsigned)loadSize, (unsigned)loadAddress);

	/* The section's own descriptor is the first thing the radio is told, and
	 * its answer says whether the section is wanted at all. A zero leads it,
	 * choosing the plain download mode.
	 */
	uint8 header[1 + SECTION_HEADER_BYTES];
	header[0] = 0;
	memcpy(header + 1, map + SECTION_MAP_COMMON, SECTION_HEADER_BYTES);

	uint8 flag = 0;
	int retries = 20;
	while (true) {
		status_t status = SendWmt(fd, WMT_PATCH_DOWNLOAD, 0, header,
			sizeof(header));
		if (status != B_OK) {
			fprintf(stderr, "[!] offering section %u: %s\n", (unsigned)index,
				strerror(status));
			return status;
		}
		status = ReadWmt(fd, WMT_PATCH_DOWNLOAD, &flag, NULL, NULL);
		if (status != B_OK)
			return status;

		/* One means it is still thinking about the last one. */
		if (flag != 1)
			break;
		if (--retries <= 0) {
			fprintf(stderr, "[!] section %u never settled\n", (unsigned)index);
			return B_TIMED_OUT;
		}
		snooze(100000);
	}

	if (flag == 2) {
		printf("  the radio already has this section\n");
		return B_OK;
	}

	*skipped = false;

	const uint8* data = image + offset;
	uint32 remaining = loadSize;
	uint32 chunks = 0;
	bool first = true;

	while (remaining > 0) {
		uint32 length = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;

		/* First, middle, last. A section small enough to fit one chunk is
		 * still marked first, which is what the radio expects.
		 */
		uint8 chunkFlag;
		if (first) {
			chunkFlag = 1;
			first = false;
		} else if (remaining - length == 0)
			chunkFlag = 3;
		else
			chunkFlag = 2;

		uint8 answer = 0;
		status_t status = DoWmt(fd, WMT_PATCH_DOWNLOAD, chunkFlag, data,
			length, &answer);
		if (status != B_OK) {
			fprintf(stderr, "[!] chunk %u of section %u: %s\n",
				(unsigned)chunks, (unsigned)index, strerror(status));
			return status;
		}
		if (answer == 1) {
			fprintf(stderr, "[!] the radio stalled on chunk %u of section %u\n",
				(unsigned)chunks, (unsigned)index);
			return B_IO_ERROR;
		}

		data += length;
		remaining -= length;
		chunks++;

		if ((chunks % 250) == 0 || remaining == 0) {
			printf("\r  %u of %u bytes sent in %u chunks",
				(unsigned)(loadSize - remaining), (unsigned)loadSize,
				(unsigned)chunks);
			fflush(stdout);
		}
	}
	printf("\n");

	return B_OK;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	int arg = 1;
	while (arg < argc && argv[arg][0] == '-' && argv[arg][1] != '\0') {
		if (strcmp(argv[arg], "-n") == 0)
			sDryRun = true;
		else if (strcmp(argv[arg], "-v") == 0)
			sVerbose = true;
		else {
			fprintf(stderr, "unknown option %s\n", argv[arg]);
			return 1;
		}
		arg++;
	}

	const char* path = arg < argc ? argv[arg++] : "/dev/bus/usb/0/20";
	const char* firmwarePath = arg < argc ? argv[arg++]
		: "/boot/system/non-packaged/data/firmware/h2generic/"
		  "BT_RAM_CODE_MT7922_1_1_hdr.bin";

	/* The image first, so a missing or wrong file costs the radio nothing. */
	FILE* file = fopen(firmwarePath, "rb");
	if (file == NULL) {
		fprintf(stderr, "[!] cannot read %s: %s\n", firmwarePath,
			strerror(errno));
		return 1;
	}
	fseek(file, 0, SEEK_END);
	long fileSize = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (fileSize < (long)(PATCH_HEADER_SIZE + GLOBAL_DESC_SIZE
			+ SECTION_MAP_SIZE)) {
		fprintf(stderr, "[!] %s is too small to be a firmware image\n",
			firmwarePath);
		fclose(file);
		return 1;
	}

	uint8* image = (uint8*)malloc(fileSize);
	if (image == NULL || (long)fread(image, 1, fileSize, file) != fileSize) {
		fprintf(stderr, "[!] cannot load %s\n", firmwarePath);
		fclose(file);
		return 1;
	}
	fclose(file);

	uint16 imageHardware = image[0x14] | (image[0x15] << 8);
	uint32 sectionCount = ReadLE32(image + 0x2c);

	printf("firmware %s\n", firmwarePath);
	printf("  built %.14s for %.4s, hardware version %#x, %u sections\n",
		(const char*)image, (const char*)image + 16, imageHardware,
		(unsigned)sectionCount);

	if (sectionCount == 0 || sectionCount > 16) {
		fprintf(stderr, "[!] %u sections is not believable\n",
			(unsigned)sectionCount);
		return 1;
	}
	if ((size_t)(PATCH_HEADER_SIZE + GLOBAL_DESC_SIZE
			+ sectionCount * SECTION_MAP_SIZE) > (size_t)fileSize) {
		fprintf(stderr, "[!] the section table runs past the end of the"
			" file\n");
		return 1;
	}

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "[!] cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}

	/* One old part answers on a different register, and is told apart first. */
	uint32 legacyId = 0;
	ReadRegister(fd, MTK_REG_LEGACY_ID, &legacyId);
	if (legacyId == 0x7663) {
		fprintf(stderr, "[!] this is an MT7663, which loads differently\n");
		close(fd);
		return 1;
	}

	uint32 chipId = 0, firmwareVersion = 0;
	if (ReadRegister(fd, MTK_REG_CHIP_ID, &chipId) != B_OK
		|| ReadRegister(fd, MTK_REG_FW_VERSION, &firmwareVersion) != B_OK) {
		fprintf(stderr, "[!] %s does not answer as a MediaTek radio\n", path);
		close(fd);
		return 1;
	}

	/* The radio names the firmware it wants: the part number, and a revision
	 * that counts from one where the register counts from zero.
	 */
	printf("radio MT%04x, firmware revision %u\n",
		(unsigned)(chipId & 0xffff), (unsigned)(firmwareVersion & 0xff) + 1);
	printf("  it wants BT_RAM_CODE_MT%04x_1_%x_hdr.bin\n",
		(unsigned)(chipId & 0xffff), (unsigned)(firmwareVersion & 0xff) + 1);

	if (sDryRun)
		printf("\n-- dry run: nothing will be sent --\n");
	printf("\n");

	bigtime_t started = system_time();

	for (uint32 i = 0; i < sectionCount; i++) {
		bool skipped = false;
		if (DownloadSection(fd, image, fileSize, i, &skipped) != B_OK) {
			close(fd);
			return 1;
		}
	}

	/* Let what was sent settle before asking the radio to use it. */
	snooze(100000);

	if (WriteResetOption(fd, MTK_REG_EP_RST_OPT, MTK_EP_RST_IN_OUT_OPT)
			!= B_OK) {
		fprintf(stderr, "[!] the radio would not take the endpoint reset"
			" option\n");
		close(fd);
		return 1;
	}

	/* And now: stop being a bootloader, start being a Bluetooth controller. */
	uint8 on = 1;
	uint8 answer = 0;
	if (DoWmt(fd, WMT_FUNC_CTRL, 0, &on, 1, &answer) != B_OK) {
		fprintf(stderr, "[!] the radio would not turn Bluetooth on\n");
		close(fd);
		return 1;
	}

	printf("\nthe radio took the firmware and Bluetooth is on"
		" (answer %#x) in %.1f s\n", answer,
		(system_time() - started) / 1000000.0);
	printf("it should answer ordinary HCI commands now.\n");

	free(image);
	close(fd);
	return 0;
}
