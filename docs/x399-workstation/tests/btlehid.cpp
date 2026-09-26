/* Connect to a Bluetooth Low Energy mouse, pair with it, and read what it
 * sends when it moves.
 *
 * A mouse that only speaks Low Energy is not reachable by anything Haiku has.
 * There is no LE in the stack at all: no LE commands, no LE events, no ATT, no
 * Security Manager. This walks the whole path from an advertising report to a
 * mouse report against the real hardware, so that what goes into the stack
 * afterwards is a sequence already known to work rather than a reading of the
 * specification.
 *
 * The path, in the order it happens here:
 *
 *   The controller is asked to connect to the advertiser. That gives a
 *   connection handle, and from then on everything travels as ACL data on the
 *   bulk endpoints rather than as commands and events.
 *
 *   Inside the ACL data is L2CAP, which on an LE link needs no setting up at
 *   all: three channel numbers are fixed and always open - 4 for attributes,
 *   5 for signalling, 6 for the Security Manager.
 *
 *   On channel 4 the device's attributes are read: its services, and within
 *   the human interface service the report map that describes the shape of its
 *   reports and the report characteristics the reports arrive on.
 *
 *   Reading those needs an encrypted link, and encryption needs a key, so on
 *   channel 6 we pair. A mouse has nothing to type a number on and no screen
 *   to show one, so the pairing is "just works": both sides use a key of zero
 *   and each proves it has the same one. What comes out is a short term key
 *   that encrypts this connection, and then a long term key the device keeps,
 *   so that next time it can just reconnect.
 *
 *   With the link encrypted, we subscribe to the report characteristics, and
 *   the mouse starts sending. The reports are ordinary HID reports, the same
 *   shape as a USB mouse sends.
 *
 * Only the smallest useful part of each protocol is here, and nothing that a
 * mouse does not need.
 *
 * This goes through the transport driver rather than talking to the USB device
 * itself, because a Low Energy link needs commands, events and data all at
 * once: the answer to a request can arrive while a mouse report is on its way.
 * The raw USB interface serialises everything on one lock per device, so a read
 * of one endpoint stops every other transfer, while the driver already runs its
 * endpoints properly and hands what it reads to a port. Reading that port is
 * both timeout-able and in the right order.
 *
 * The port is the one the Bluetooth server normally owns, so the server has to
 * be out of the way:  launch_roster stop x-vnd.haiku-bluetooth_server
 *
 * usage: btlehid [-d device] [-a address] [-r|-P] [-s seconds] [-v] [-k file]
 *        -d  the transport, /dev/bluetooth/h2/h2generic/0 by default
 *        -a  the device to connect to; without this, scan for a mouse
 *        -r  the address is a public one (default: random)
 *        -P  do not pair, only look
 *        -s  how long to listen for reports, 20 seconds by default
 *        -k  where the keys live, ~/config/settings/btlehid_keys by default
 *        -v  print every packet byte for byte
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <OS.h>

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_acl.h>
#include <bluetooth/HCI/btHCI_transport.h>

extern "C" {
#include "aes.h"
}

#define USB_CLASS_OUT			0x20

/* HCI commands. */
#define HCI_RESET			0x0c03
#define HCI_SET_EVENT_MASK		0x0c01
#define HCI_READ_LOCAL_FEATURES		0x1003
#define HCI_WRITE_LE_HOST_SUPPORT	0x0c6d
#define HCI_DISCONNECT			0x0406
#define HCI_LE_SET_EVENT_MASK		0x2001
#define HCI_LE_READ_BUFFER_SIZE		0x2002
#define HCI_LE_SET_SCAN_PARAMETERS	0x200b
#define HCI_LE_SET_SCAN_ENABLE		0x200c
#define HCI_LE_CREATE_CONNECTION	0x200d
#define HCI_LE_CREATE_CONNECTION_CANCEL	0x200e
#define HCI_LE_START_ENCRYPTION		0x2019

/* HCI events. */
#define HCI_EVENT_DISCONNECTION_COMPLETE	0x05
#define HCI_EVENT_ENCRYPTION_CHANGE		0x08
#define HCI_EVENT_COMMAND_COMPLETE		0x0e
#define HCI_EVENT_COMMAND_STATUS		0x0f
#define HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS	0x13
#define HCI_EVENT_LE_META			0x3e

#define LE_SUBEVENT_CONNECTION_COMPLETE		0x01
#define LE_SUBEVENT_ADVERTISING_REPORT		0x02
#define LE_SUBEVENT_CONNECTION_UPDATE		0x03
#define LE_SUBEVENT_LONG_TERM_KEY_REQUEST	0x05
#define LE_SUBEVENT_DATA_LENGTH_CHANGE		0x07
#define LE_SUBEVENT_ENHANCED_CONNECTION_COMPLETE 0x0a
#define LE_SUBEVENT_PHY_UPDATE			0x0c

/* L2CAP channels that are always there on an LE link. */
#define L2CAP_CID_ATTRIBUTE		0x0004
#define L2CAP_CID_LE_SIGNALLING		0x0005
#define L2CAP_CID_SECURITY_MANAGER	0x0006

/* Attribute protocol. */
#define ATT_ERROR_RESPONSE		0x01
#define ATT_EXCHANGE_MTU_REQUEST	0x02
#define ATT_EXCHANGE_MTU_RESPONSE	0x03
#define ATT_FIND_INFORMATION_REQUEST	0x04
#define ATT_FIND_INFORMATION_RESPONSE	0x05
#define ATT_READ_BY_TYPE_REQUEST	0x08
#define ATT_READ_BY_TYPE_RESPONSE	0x09
#define ATT_READ_REQUEST		0x0a
#define ATT_READ_RESPONSE		0x0b
#define ATT_READ_BLOB_REQUEST		0x0c
#define ATT_READ_BLOB_RESPONSE		0x0d
#define ATT_READ_BY_GROUP_TYPE_REQUEST	0x10
#define ATT_READ_BY_GROUP_TYPE_RESPONSE	0x11
#define ATT_WRITE_REQUEST		0x12
#define ATT_WRITE_RESPONSE		0x13
#define ATT_HANDLE_VALUE_NOTIFICATION	0x1b

#define GATT_PRIMARY_SERVICE		0x2800
#define GATT_CHARACTERISTIC		0x2803
#define GATT_CLIENT_CONFIGURATION	0x2902
#define GATT_REPORT_REFERENCE		0x2908

#define UUID_HID_SERVICE		0x1812
#define UUID_BATTERY_SERVICE		0x180f
#define UUID_DEVICE_INFORMATION		0x180a
#define UUID_REPORT_MAP			0x2a4b
#define UUID_REPORT			0x2a4d
#define UUID_PROTOCOL_MODE		0x2a4e
#define UUID_HID_CONTROL_POINT		0x2a4c
#define UUID_HID_INFORMATION		0x2a4a
#define UUID_BATTERY_LEVEL		0x2a19
#define UUID_MANUFACTURER_NAME		0x2a29
#define UUID_MODEL_NUMBER		0x2a24

/* Security Manager. */
#define SMP_PAIRING_REQUEST		0x01
#define SMP_PAIRING_RESPONSE		0x02
#define SMP_PAIRING_CONFIRM		0x03
#define SMP_PAIRING_RANDOM		0x04
#define SMP_PAIRING_FAILED		0x05
#define SMP_ENCRYPTION_INFORMATION	0x06
#define SMP_MASTER_IDENTIFICATION	0x07
#define SMP_IDENTITY_INFORMATION	0x08
#define SMP_IDENTITY_ADDRESS		0x09
#define SMP_SIGNING_INFORMATION		0x0a
#define SMP_SECURITY_REQUEST		0x0b

#define MAX_PACKET			1024

static bool sVerbose = false;
static int sDevice = -1;
static port_id sPort = -1;
static uint16 sConnection = 0xffff;
static uint16 sAttMtu = 23;


/* What comes up the port: an event or a whole ACL packet, and which is which
 * is in the port's code rather than in the bytes.
 */
struct queued_packet {
	uint8	type;		/* 0: HCI event, 1: ACL data */
	uint16	length;
	uint8	data[MAX_PACKET];
};


static void
Dump(const char* what, const uint8* data, size_t length)
{
	printf("    %s (%zu bytes):", what, length);
	for (size_t i = 0; i < length && i < 48; i++)
		printf(" %02x", data[i]);
	if (length > 48)
		printf(" ...");
	printf("\n");
}


static const char*
AddressToString(const uint8* address)
{
	static char text[32];
	snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[5],
		address[4], address[3], address[2], address[1], address[0]);
	return text;
}


static void
AddressFromString(const char* text, uint8* address)
{
	unsigned values[6] = { 0, 0, 0, 0, 0, 0 };
	sscanf(text, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2],
		&values[3], &values[4], &values[5]);
	for (int i = 0; i < 6; i++)
		address[i] = (uint8)values[5 - i];
}


//	#pragma mark - the crypto the Security Manager needs


/* The Security Manager's functions are defined over numbers whose most
 * significant byte comes first, while everything in a packet has its least
 * significant byte first. Rather than have every caller remember that, the
 * reversal lives here, in the one place that does arithmetic.
 */
static void
Reverse(uint8* out, const uint8* in, size_t length)
{
	for (size_t i = 0; i < length; i++)
		out[i] = in[length - 1 - i];
}


/* e: one block of AES-128, in the Security Manager's byte order. */
static void
SecurityEncrypt(const uint8 key[16], const uint8 plain[16], uint8 out[16])
{
	uint8 reversedKey[16];
	uint8 reversedPlain[16];
	uint8 cipher[16];

	Reverse(reversedKey, key, 16);
	Reverse(reversedPlain, plain, 16);

	AES_CTX context;
	AES_Setkey(&context, reversedKey, 16);
	AES_Encrypt(&context, reversedPlain, cipher);

	Reverse(out, cipher, 16);
	memset(&context, 0, sizeof(context));
}


/* c1: the value each side sends to commit to its random number before either
 * has revealed it. It mixes in both pairing packets and both addresses, so a
 * confirm from one pairing cannot be replayed into another.
 */
static void
SecurityConfirm(const uint8 key[16], const uint8 random[16],
	const uint8 request[7], const uint8 response[7], uint8 initiatorType,
	const uint8* initiatorAddress, uint8 responderType,
	const uint8* responderAddress, uint8 out[16])
{
	uint8 p1[16];
	p1[0] = initiatorType;
	p1[1] = responderType;
	memcpy(p1 + 2, request, 7);
	memcpy(p1 + 9, response, 7);

	uint8 block[16];
	for (int i = 0; i < 16; i++)
		block[i] = random[i] ^ p1[i];

	uint8 first[16];
	SecurityEncrypt(key, block, first);

	uint8 p2[16];
	memcpy(p2, responderAddress, 6);
	memcpy(p2 + 6, initiatorAddress, 6);
	memset(p2 + 12, 0, 4);

	for (int i = 0; i < 16; i++)
		block[i] = first[i] ^ p2[i];

	SecurityEncrypt(key, block, out);
}


/* s1: the key that encrypts this connection, made from both randoms. */
static void
SecuritySessionKey(const uint8 key[16], const uint8 responderRandom[16],
	const uint8 initiatorRandom[16], uint8 out[16])
{
	uint8 block[16];
	memcpy(block, initiatorRandom, 8);
	memcpy(block + 8, responderRandom, 8);

	SecurityEncrypt(key, block, out);
}


/* The specification publishes worked examples for all three. Checking them
 * here means a pairing that fails later failed on the air rather than in the
 * arithmetic - which is not something the device will ever tell us.
 */
static bool
CheckCrypto()
{
	bool ok = true;

	/* AES-128 itself, from the standard that defines it. */
	const uint8 aesKey[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
	const uint8 aesPlain[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
		0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
	const uint8 aesCipher[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04,
		0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };

	uint8 out[16];
	AES_CTX context;
	AES_Setkey(&context, aesKey, 16);
	AES_Encrypt(&context, aesPlain, out);
	if (memcmp(out, aesCipher, 16) != 0) {
		printf("[!] AES-128 does not match its own test vector\n");
		Dump("got", out, 16);
		ok = false;
	}

	/* c1, from the specification's example: a pairing between a device at
	 * a1a2a3a4a5a6 and one at b1b2b3b4b5b6, with a key of zero.
	 */
	const uint8 zeroKey[16] = {};
	const uint8 random[16] = { 0xe0, 0x2e, 0x70, 0xc6, 0x4e, 0x27, 0x88, 0x63,
		0x0e, 0x6f, 0xad, 0x56, 0x21, 0xd5, 0x83, 0x57 };
	const uint8 request[7] = { 0x01, 0x01, 0x00, 0x00, 0x10, 0x07, 0x07 };
	const uint8 response[7] = { 0x02, 0x03, 0x00, 0x00, 0x08, 0x00, 0x05 };
	const uint8 initiator[6] = { 0xa6, 0xa5, 0xa4, 0xa3, 0xa2, 0xa1 };
	const uint8 responder[6] = { 0xb6, 0xb5, 0xb4, 0xb3, 0xb2, 0xb1 };
	const uint8 expectedConfirm[16] = { 0x86, 0x3b, 0xf1, 0xbe, 0xc5, 0x4d,
		0xa7, 0xd2, 0xea, 0x88, 0x89, 0x87, 0xef, 0x3f, 0x1e, 0x1e };

	SecurityConfirm(zeroKey, random, request, response, 0x01, initiator, 0x00,
		responder, out);
	if (memcmp(out, expectedConfirm, 16) != 0) {
		printf("[!] the confirm function does not match its test vector\n");
		Dump("got", out, 16);
		ok = false;
	}

	/* s1, likewise. */
	const uint8 firstRandom[16] = { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
		0x11, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00 };
	const uint8 secondRandom[16] = { 0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa,
		0x99, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
	const uint8 expectedKey[16] = { 0x62, 0xa0, 0x6d, 0x79, 0xae, 0x16, 0x42,
		0x5b, 0x9b, 0xf4, 0xb0, 0xe8, 0xf0, 0xe1, 0x1f, 0x9a };

	SecuritySessionKey(zeroKey, firstRandom, secondRandom, out);
	if (memcmp(out, expectedKey, 16) != 0) {
		printf("[!] the session key function does not match its test vector\n");
		Dump("got", out, 16);
		ok = false;
	}

	return ok;
}


//	#pragma mark - the transport


/* Commands go down as an ioctl, and the driver writes what comes back into the
 * port this process owns.
 */
static status_t
SendCommand(uint16 opcode, const uint8* parameters, uint8 length)
{
	uint8 packet[3 + 255];
	packet[0] = opcode & 0xff;
	packet[1] = opcode >> 8;
	packet[2] = length;
	if (length > 0)
		memcpy(packet + 3, parameters, length);

	if (sVerbose)
		Dump("-> command", packet, 3 + length);

	if (ioctl(sDevice, ISSUE_BT_COMMAND, packet, 3 + length) != 0) {
		fprintf(stderr, "[!] the driver would not take a command: %s\n",
			strerror(errno));
		return errno != 0 ? (status_t)errno : B_ERROR;
	}

	return B_OK;
}


/* Send one L2CAP frame on this connection. Everything a mouse needs fits in a
 * single ACL packet, so this does not fragment. The flag says this is the first
 * fragment of a frame and may be flushed automatically, which is the only
 * combination a Low Energy link allows for a first fragment.
 */
static status_t
SendFrame(uint16 channel, const uint8* data, size_t length)
{
	uint8 packet[MAX_PACKET];
	uint16 handle = pack_acl_handle_flags(sConnection, HCI_ACL_PACKET_START, 0);
	packet[0] = handle & 0xff;
	packet[1] = handle >> 8;
	uint16 total = (uint16)(length + 4);
	packet[2] = total & 0xff;
	packet[3] = total >> 8;
	packet[4] = length & 0xff;
	packet[5] = (length >> 8) & 0xff;
	packet[6] = channel & 0xff;
	packet[7] = channel >> 8;
	memcpy(packet + 8, data, length);

	if (sVerbose)
		Dump("-> data", packet, 8 + length);

	if (ioctl(sDevice, ISSUE_BT_ACL, packet, 8 + length) != 0) {
		fprintf(stderr, "[!] the driver would not take a data packet: %s\n",
			strerror(errno));
		return errno != 0 ? (status_t)errno : B_ERROR;
	}

	return B_OK;
}


/* What the driver counted. When a request gets no answer, this says whether it
 * ever left: the driver counts what it offered to the bus, what the bus took,
 * and what came back, separately for commands and for data.
 */
static void
ShowStatistics(const char* when)
{
	bt_hci_statistics stats;
	memset(&stats, 0, sizeof(stats));

	if (ioctl(sDevice, GET_STATS, &stats, sizeof(stats)) != 0) {
		printf("  (the driver would not give up its counters: %s)\n",
			strerror(errno));
		return;
	}

	printf("  %s: sent %u offered/%u refused/%u completed/%u failed,"
		" read %u offered/%u refused/%u completed/%u failed,"
		" commands %u, events %u, data out %u, data in %u\n", when,
		stats.acceptedTX, stats.rejectedTX, stats.successfulTX, stats.errorTX,
		stats.acceptedRX, stats.rejectedRX, stats.successfulRX, stats.errorRX,
		stats.commandTX, stats.eventRX, stats.aclTX, stats.aclRX);
}


/* Take the next thing the kernel hands up, whatever it is. */
static status_t
Dequeue(queued_packet* packet, bigtime_t timeout)
{
	int32 code = 0;
	ssize_t received = read_port_etc(sPort, &code, packet->data,
		sizeof(packet->data), B_TIMEOUT, timeout < 0 ? 0 : timeout);

	if (received < 0)
		return (status_t)received;

	packet->length = (uint16)received;
	packet->type = GET_PORTCODE_TYPE(code) == BT_ACL ? 1 : 0;

	if (GET_PORTCODE_TYPE(code) != BT_EVENT
		&& GET_PORTCODE_TYPE(code) != BT_ACL) {
		/* Nothing else should arrive here; say so rather than misread it. */
		printf("  the kernel handed up a %u, which is neither event nor data\n",
			(unsigned)GET_PORTCODE_TYPE(code));
		return B_ERROR;
	}

	return B_OK;
}


//	#pragma mark - what arrives


struct hid_report_characteristic {
	uint16	valueHandle;
	uint16	configurationHandle;
	uint8	reportId;
	uint8	reportType;	/* 1 input, 2 output, 3 feature */
};

static hid_report_characteristic sReports[16];
static int sReportCount = 0;
static uint16 sReportMapHandle = 0;
static uint16 sProtocolModeHandle = 0;
static uint16 sBatteryLevelHandle = 0;

/* What the Security Manager and the connection produce as they go. */
static uint8 sPairingRequest[7];
static uint8 sPairingResponse[7];
static uint8 sInitiatorRandom[16];
static uint8 sResponderRandom[16];
static uint8 sResponderConfirm[16];
static uint8 sSessionKey[16];
static uint8 sLongTermKey[16];
static uint16 sKeyDiversifier = 0;
static uint8 sKeyRandom[8];
static bool sHaveLongTermKey = false;
static bool sEncrypted = false;
static bool sPairingFailed = false;
static uint8 sPairingFailure = 0;
static int sPairingStep = 0;
static bool sDisconnected = false;
static uint8 sDisconnectReason = 0;

static uint8 sLocalAddress[6];
static uint8 sLocalAddressType = 0x00;
static uint8 sRemoteAddress[6];
static uint8 sRemoteAddressType = 0x01;

/* One ATT request may be outstanding at a time - that is the protocol's own
 * rule - so a single slot for the answer is enough.
 */
static uint8 sAttResponse[MAX_PACKET];
static size_t sAttResponseLength = 0;
static bool sHaveAttResponse = false;

static int sNotifications = 0;


static const char*
AttErrorName(uint8 error)
{
	switch (error) {
		case 0x01: return "invalid handle";
		case 0x02: return "reading not permitted";
		case 0x03: return "writing not permitted";
		case 0x05: return "not authenticated";
		case 0x07: return "invalid offset";
		case 0x08: return "not authorised";
		case 0x0a: return "attribute not found";
		case 0x0c: return "encryption key too short";
		case 0x0d: return "wrong length";
		case 0x0f: return "not encrypted";
		case 0x11: return "out of resources";
	}
	return "error";
}


static const char*
PairingFailureName(uint8 reason)
{
	switch (reason) {
		case 0x01: return "the passkey was not entered";
		case 0x02: return "it will not pair like this";
		case 0x03: return "the confirm value did not match";
		case 0x04: return "pairing is not supported";
		case 0x05: return "the key would be too short";
		case 0x06: return "a command it does not know";
		case 0x07: return "something unspecified";
		case 0x08: return "too many attempts, try later";
		case 0x09: return "invalid parameters";
		case 0x0a: return "the transport keys do not match";
		case 0x0b: return "it is busy pairing on another transport";
		case 0x0c: return "cross-transport keys are not allowed";
		case 0x0d: return "the key was rejected";
	}
	return "?";
}


static void PairingStep(const uint8* data, size_t length);


static void
HandleAttribute(const uint8* data, size_t length)
{
	if (length < 1)
		return;

	uint8 opcode = data[0];

	if (opcode == ATT_HANDLE_VALUE_NOTIFICATION) {
		if (length < 3)
			return;
		uint16 handle = data[1] | (data[2] << 8);
		sNotifications++;

		/* Say what it is, and for a mouse report say what it means. The
		 * shape below is the usual one: a byte of buttons, then movement.
		 */
		printf("  report from handle %#04x:", handle);
		for (size_t i = 3; i < length; i++)
			printf(" %02x", data[i]);

		if (handle == sBatteryLevelHandle && length == 4)
			printf("   battery %u%%", data[3]);
		printf("\n");
		return;
	}

	if (sVerbose)
		Dump("<- attribute", data, length);

	memcpy(sAttResponse, data, length);
	sAttResponseLength = length;
	sHaveAttResponse = true;
}


static void
HandleData(const uint8* packet, size_t length)
{
	if (length < 8)
		return;

	uint16 frameLength = packet[4] | (packet[5] << 8);
	uint16 channel = packet[6] | (packet[7] << 8);
	const uint8* payload = packet + 8;

	if (frameLength + 8u > length)
		frameLength = (uint16)(length - 8);

	switch (channel) {
		case L2CAP_CID_ATTRIBUTE:
			HandleAttribute(payload, frameLength);
			break;

		case L2CAP_CID_SECURITY_MANAGER:
			PairingStep(payload, frameLength);
			break;

		case L2CAP_CID_LE_SIGNALLING:
			/* The only thing a device sends here unasked is a request to
			 * change the connection timing, which the controller answers by
			 * itself on newer parts. Noting it is enough.
			 */
			if (sVerbose)
				Dump("<- signalling", payload, frameLength);
			break;

		default:
			printf("  data on channel %#04x, which nothing here uses\n",
				channel);
			break;
	}
}


/* Events that can arrive at any time, rather than as an answer to something.
 * Returns true if it was one of those.
 */
static bool
HandleUnsolicitedEvent(const uint8* event, size_t length)
{
	uint8 code = event[0];

	if (code == HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS)
		return true;

	if (code == HCI_EVENT_ENCRYPTION_CHANGE && length >= 6) {
		if (event[2] == 0x00) {
			sEncrypted = event[5] != 0;
			printf("  the link is %s\n",
				sEncrypted ? "encrypted" : "no longer encrypted");
		} else {
			printf("  [!] encryption failed: %#02x\n", event[2]);
			sPairingFailed = true;
		}
		return true;
	}

	if (code == HCI_EVENT_DISCONNECTION_COMPLETE && length >= 7) {
		sDisconnected = true;
		sDisconnectReason = event[6];
		printf("  the device disconnected (%#02x)\n", sDisconnectReason);
		return true;
	}

	if (code == HCI_EVENT_LE_META && length >= 3) {
		uint8 subevent = event[2];
		if (subevent == LE_SUBEVENT_DATA_LENGTH_CHANGE
			|| subevent == LE_SUBEVENT_PHY_UPDATE
			|| subevent == LE_SUBEVENT_CONNECTION_UPDATE) {
			if (sVerbose)
				Dump("<- le event", event, length);
			return true;
		}
	}

	return false;
}


/* Wait for the answer to a command. Anything else that arrives on the way is
 * handled where it belongs, so nothing is lost by waiting here.
 */
static int
WaitForCommand(uint16 opcode, uint8* returnParameters, size_t* returnLength,
	bigtime_t timeout = 4000000)
{
	bigtime_t end = system_time() + timeout;

	while (system_time() < end) {
		queued_packet packet;
		if (Dequeue(&packet, end - system_time()) != B_OK)
			break;

		if (packet.type == 1) {
			HandleData(packet.data, packet.length);
			continue;
		}

		if (packet.length < 2)
			continue;

		if (sVerbose && packet.data[0] != HCI_EVENT_COMMAND_COMPLETE)
			Dump("<- event", packet.data, packet.length);

		if (packet.data[0] == HCI_EVENT_COMMAND_COMPLETE
			&& packet.length >= 5) {
			uint16 answered = packet.data[3] | (packet.data[4] << 8);
			if (answered != opcode)
				continue;

			size_t parameters = packet.length - 5;
			if (returnParameters != NULL && returnLength != NULL) {
				if (parameters > *returnLength)
					parameters = *returnLength;
				memcpy(returnParameters, packet.data + 5, parameters);
				*returnLength = parameters;
			}
			return parameters > 0 ? packet.data[5] : 0;
		}

		if (packet.data[0] == HCI_EVENT_COMMAND_STATUS
			&& packet.length >= 6) {
			uint16 answered = packet.data[4] | (packet.data[5] << 8);
			if (answered != opcode)
				continue;
			return packet.data[2];
		}

		HandleUnsolicitedEvent(packet.data, packet.length);
	}

	return -1;
}


static bool
Command(const char* what, uint16 opcode, const uint8* parameters, uint8 length,
	uint8* returnParameters = NULL, size_t* returnLength = NULL)
{
	if (what != NULL) {
		printf("%-36s", what);
		fflush(stdout);
	}

	if (SendCommand(opcode, parameters, length) != B_OK) {
		if (what != NULL)
			printf(" the radio would not take it\n");
		return false;
	}

	int status = WaitForCommand(opcode, returnParameters, returnLength);
	if (what != NULL) {
		if (status == 0)
			printf(" ok\n");
		else if (status < 0)
			printf(" no answer\n");
		else
			printf(" refused (%#02x)\n", status);
	}

	return status == 0;
}


//	#pragma mark - finding and connecting


struct found_device {
	uint8	address[6];
	uint8	addressType;
	char	name[48];
	bool	isHid;
};


static bool
ScanForMouse(int seconds, found_device* found)
{
	const uint8 parameters[7] = { 0x01, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };
	Command("setting the scan up", HCI_LE_SET_SCAN_PARAMETERS, parameters, 7);

	const uint8 on[2] = { 0x01, 0x00 };
	if (!Command("starting the scan", HCI_LE_SET_SCAN_ENABLE, on, 2))
		return false;

	printf("looking for a mouse for up to %d seconds...\n", seconds);

	bigtime_t end = system_time() + (bigtime_t)seconds * 1000000;
	bool have = false;
	memset(found, 0, sizeof(*found));

	while (system_time() < end && !have) {
		queued_packet packet;
		if (Dequeue(&packet, end - system_time()) != B_OK)
			break;
		if (packet.type != 0 || packet.length < 4)
			continue;
		if (packet.data[0] != HCI_EVENT_LE_META
			|| packet.data[2] != LE_SUBEVENT_ADVERTISING_REPORT)
			continue;

		const uint8* report = packet.data + 3;
		size_t length = packet.length - 3;
		if (length < 10)
			continue;

		uint8 addressType = report[2];
		const uint8* address = report + 3;
		uint8 dataLength = report[9];
		if (10u + dataLength > length)
			continue;
		const uint8* data = report + 10;

		bool isHid = false;
		char name[48];
		name[0] = '\0';

		size_t offset = 0;
		while (offset + 1 < dataLength) {
			uint8 fieldLength = data[offset];
			if (fieldLength == 0 || offset + fieldLength >= dataLength + 1u)
				break;
			uint8 type = data[offset + 1];
			const uint8* value = data + offset + 2;
			uint8 valueLength = fieldLength - 1;

			if (type == 0x02 || type == 0x03) {
				for (uint8 i = 0; i + 1 < valueLength; i += 2) {
					if ((value[i] | (value[i + 1] << 8)) == UUID_HID_SERVICE)
						isHid = true;
				}
			} else if (type == 0x19 && valueLength >= 2) {
				uint16 appearance = value[0] | (value[1] << 8);
				if ((appearance & 0xffc0) == 0x03c0)
					isHid = true;
			} else if (type == 0x08 || type == 0x09) {
				uint8 copy = valueLength < sizeof(name) - 1
					? valueLength : (uint8)(sizeof(name) - 1);
				memcpy(name, value, copy);
				name[copy] = '\0';
			}

			offset += fieldLength + 1;
		}

		if (isHid) {
			memcpy(found->address, address, 6);
			found->addressType = addressType;
			strncpy(found->name, name, sizeof(found->name) - 1);
			found->isHid = true;
			have = true;
			printf("  %s (%s) %s\n", AddressToString(address),
				addressType == 0 ? "public" : "random", name);
		}
	}

	const uint8 off[2] = { 0x00, 0x00 };
	Command("stopping the scan", HCI_LE_SET_SCAN_ENABLE, off, 2);

	return have;
}


static bool
Connect(const uint8* address, uint8 addressType)
{
	uint8 parameters[25];
	parameters[0] = 0x60;	/* scan interval */
	parameters[1] = 0x00;
	parameters[2] = 0x30;	/* scan window */
	parameters[3] = 0x00;
	parameters[4] = 0x00;	/* no filter list: this one device */
	parameters[5] = addressType;
	memcpy(parameters + 6, address, 6);
	parameters[12] = 0x00;	/* our own public address */
	parameters[13] = 0x18;	/* connection interval, 24 * 1.25ms = 30ms */
	parameters[14] = 0x00;
	parameters[15] = 0x28;	/* up to 40 * 1.25ms = 50ms */
	parameters[16] = 0x00;
	parameters[17] = 0x00;	/* no slave latency */
	parameters[18] = 0x00;
	parameters[19] = 0xf4;	/* give up after 500 * 10ms = 5 seconds */
	parameters[20] = 0x01;
	parameters[21] = 0x00;	/* no preference on connection event length */
	parameters[22] = 0x00;
	parameters[23] = 0x00;
	parameters[24] = 0x00;

	printf("connecting to %s (%s)\n", AddressToString(address),
		addressType == 0 ? "public" : "random");

	/* This one answers with a Command Status, and the connection itself
	 * arrives later as an LE event.
	 */
	if (SendCommand(HCI_LE_CREATE_CONNECTION, parameters, 25) != B_OK)
		return false;

	int status = WaitForCommand(HCI_LE_CREATE_CONNECTION, NULL, NULL);
	if (status != 0) {
		printf("[!] the controller would not start connecting (%#02x)\n",
			status < 0 ? 0 : status);
		return false;
	}

	bigtime_t end = system_time() + 20000000;
	while (system_time() < end) {
		queued_packet packet;
		if (Dequeue(&packet, end - system_time()) != B_OK)
			break;

		if (packet.type == 1) {
			HandleData(packet.data, packet.length);
			continue;
		}

		if (packet.length >= 3 && packet.data[0] == HCI_EVENT_LE_META
			&& (packet.data[2] == LE_SUBEVENT_CONNECTION_COMPLETE
				|| packet.data[2] == LE_SUBEVENT_ENHANCED_CONNECTION_COMPLETE)) {
			const uint8* body = packet.data + 3;
			if (body[0] != 0x00) {
				printf("[!] the connection failed: %#02x\n", body[0]);
				return false;
			}

			sConnection = body[1] | (body[2] << 8);
			uint16 interval = body[11] | (body[12] << 8);
			if (packet.data[2] == LE_SUBEVENT_ENHANCED_CONNECTION_COMPLETE)
				interval = body[23] | (body[24] << 8);

			printf("connected: handle %#04x, every %u ms\n", sConnection,
				(unsigned)(interval * 125 / 100));
			return true;
		}

		if (packet.length >= 2)
			HandleUnsolicitedEvent(packet.data, packet.length);
	}

	printf("[!] the connection never completed\n");
	SendCommand(HCI_LE_CREATE_CONNECTION_CANCEL, NULL, 0);
	WaitForCommand(HCI_LE_CREATE_CONNECTION_CANCEL, NULL, NULL);
	return false;
}


//	#pragma mark - the attribute protocol


/* Send an ATT request and wait for its answer. Returns the answer's opcode,
 * or 0 if nothing came, and prints the reason when the device refuses.
 */
static uint8
Request(const uint8* request, size_t length, const uint8** response,
	size_t* responseLength, bigtime_t timeout = 5000000)
{
	sHaveAttResponse = false;

	if (SendFrame(L2CAP_CID_ATTRIBUTE, request, length) != B_OK)
		return 0;

	bigtime_t end = system_time() + timeout;
	while (system_time() < end && !sHaveAttResponse) {
		queued_packet packet;
		if (Dequeue(&packet, end - system_time()) != B_OK)
			break;

		if (packet.type == 1)
			HandleData(packet.data, packet.length);
		else if (packet.length >= 2)
			HandleUnsolicitedEvent(packet.data, packet.length);

		if (sDisconnected)
			return 0;
	}

	if (!sHaveAttResponse)
		return 0;

	if (response != NULL)
		*response = sAttResponse;
	if (responseLength != NULL)
		*responseLength = sAttResponseLength;

	return sAttResponse[0];
}


static bool
ExchangeMtu(uint16 want)
{
	uint8 request[3];
	request[0] = ATT_EXCHANGE_MTU_REQUEST;
	request[1] = want & 0xff;
	request[2] = want >> 8;

	const uint8* response = NULL;
	size_t length = 0;
	uint8 opcode = 0;

	/* This is the first thing said on the link, so it is also where a data
	 * path that does not work shows up. Say what the driver saw, and ask
	 * again before giving up: a device that has just connected is sometimes
	 * not listening yet.
	 */
	ShowStatistics("before the first request");

	for (int attempt = 0; attempt < 3 && opcode == 0; attempt++) {
		opcode = Request(request, 3, &response, &length, 3000000);
		if (opcode == 0 && attempt == 0)
			ShowStatistics("after asking once");
	}

	if (opcode != ATT_EXCHANGE_MTU_RESPONSE || length < 3) {
		printf("  it would not agree a packet size; staying at %u\n", sAttMtu);
		ShowStatistics("after giving up");
		return false;
	}

	uint16 theirs = response[1] | (response[2] << 8);
	sAttMtu = theirs < want ? theirs : want;
	printf("  attribute packets up to %u bytes\n", sAttMtu);
	return true;
}


struct service_range {
	uint16	start;
	uint16	end;
	uint16	uuid;
};

static service_range sServices[24];
static int sServiceCount = 0;


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
	}
	return "";
}


static bool
DiscoverServices()
{
	uint16 start = 0x0001;
	sServiceCount = 0;

	while (start != 0x0000) {
		uint8 request[7];
		request[0] = ATT_READ_BY_GROUP_TYPE_REQUEST;
		request[1] = start & 0xff;
		request[2] = start >> 8;
		request[3] = 0xff;
		request[4] = 0xff;
		request[5] = GATT_PRIMARY_SERVICE & 0xff;
		request[6] = GATT_PRIMARY_SERVICE >> 8;

		const uint8* response;
		size_t length;
		uint8 opcode = Request(request, 7, &response, &length);

		if (opcode == ATT_ERROR_RESPONSE)
			break;	/* "attribute not found" ends the walk */
		if (opcode != ATT_READ_BY_GROUP_TYPE_RESPONSE || length < 6)
			return sServiceCount > 0;

		uint8 entry = response[1];
		if (entry < 6)
			return sServiceCount > 0;

		uint16 last = start;
		for (size_t offset = 2; offset + entry <= length; offset += entry) {
			const uint8* item = response + offset;
			uint16 from = item[0] | (item[1] << 8);
			uint16 to = item[2] | (item[3] << 8);
			/* Services with a full 128 bit identity are of no use here, but
			 * their range still has to be stepped over.
			 */
			uint16 uuid = entry == 6 ? (item[4] | (item[5] << 8)) : 0x0000;

			if (sServiceCount < (int)(sizeof(sServices) / sizeof(sServices[0]))) {
				sServices[sServiceCount].start = from;
				sServices[sServiceCount].end = to;
				sServices[sServiceCount].uuid = uuid;
				sServiceCount++;
			}

			printf("  service %04x-%04x  %04x %s\n", from, to, uuid,
				ServiceName(uuid));
			last = to;
		}

		if (last >= 0xffff)
			break;
		start = last + 1;
	}

	return sServiceCount > 0;
}


/* Walk the characteristics of one service. Each is announced by a declaration
 * whose value says what it permits, where its value lives, and what it is.
 */
static bool
DiscoverCharacteristics(uint16 from, uint16 to, bool print)
{
	uint16 start = from;

	while (start <= to) {
		uint8 request[7];
		request[0] = ATT_READ_BY_TYPE_REQUEST;
		request[1] = start & 0xff;
		request[2] = start >> 8;
		request[3] = to & 0xff;
		request[4] = to >> 8;
		request[5] = GATT_CHARACTERISTIC & 0xff;
		request[6] = GATT_CHARACTERISTIC >> 8;

		const uint8* response;
		size_t length;
		uint8 opcode = Request(request, 7, &response, &length);

		if (opcode == ATT_ERROR_RESPONSE)
			break;
		if (opcode != ATT_READ_BY_TYPE_RESPONSE || length < 4)
			break;

		uint8 entry = response[1];
		if (entry < 7)
			break;

		uint16 last = start;
		for (size_t offset = 2; offset + entry <= length; offset += entry) {
			const uint8* item = response + offset;
			uint16 declaration = item[0] | (item[1] << 8);
			uint8 properties = item[2];
			uint16 value = item[3] | (item[4] << 8);
			uint16 uuid = entry >= 9 ? (item[5] | (item[6] << 8)) : 0x0000;

			if (print) {
				printf("    %04x: value at %04x  %04x  %s%s%s%s\n",
					declaration, value, uuid,
					(properties & 0x02) ? "read " : "",
					(properties & 0x08) ? "write " : "",
					(properties & 0x10) ? "notify " : "",
					(properties & 0x04) ? "write-without-response" : "");
			}

			switch (uuid) {
				case UUID_REPORT_MAP:
					sReportMapHandle = value;
					break;
				case UUID_PROTOCOL_MODE:
					sProtocolModeHandle = value;
					break;
				case UUID_BATTERY_LEVEL:
					sBatteryLevelHandle = value;
					break;
				case UUID_REPORT:
					if (sReportCount
							< (int)(sizeof(sReports) / sizeof(sReports[0]))) {
						sReports[sReportCount].valueHandle = value;
						sReports[sReportCount].configurationHandle = 0;
						sReports[sReportCount].reportId = 0;
						sReports[sReportCount].reportType = 0;
						sReportCount++;
					}
					break;
			}

			last = declaration;
		}

		if (last >= to)
			break;
		start = last + 1;
	}

	return true;
}


/* For each report characteristic, find the two descriptors that matter: the
 * one that turns notifications on, and the one that says which report it is.
 */
static void
DiscoverReportDescriptors(uint16 serviceEnd)
{
	for (int i = 0; i < sReportCount; i++) {
		uint16 from = sReports[i].valueHandle + 1;
		uint16 to = (i + 1 < sReportCount)
			? (uint16)(sReports[i + 1].valueHandle - 1) : serviceEnd;
		if (to < from)
			continue;

		uint8 request[5];
		request[0] = ATT_FIND_INFORMATION_REQUEST;
		request[1] = from & 0xff;
		request[2] = from >> 8;
		request[3] = to & 0xff;
		request[4] = to >> 8;

		const uint8* response;
		size_t length;
		if (Request(request, 5, &response, &length)
				!= ATT_FIND_INFORMATION_RESPONSE || length < 4)
			continue;

		if (response[1] != 0x01)
			continue;	/* 128 bit descriptors: none of ours */

		for (size_t offset = 2; offset + 4 <= length; offset += 4) {
			uint16 handle = response[offset] | (response[offset + 1] << 8);
			uint16 uuid = response[offset + 2] | (response[offset + 3] << 8);

			if (uuid == GATT_CLIENT_CONFIGURATION)
				sReports[i].configurationHandle = handle;

			if (uuid == GATT_REPORT_REFERENCE) {
				uint8 read[3];
				read[0] = ATT_READ_REQUEST;
				read[1] = handle & 0xff;
				read[2] = handle >> 8;

				const uint8* value;
				size_t valueLength;
				if (Request(read, 3, &value, &valueLength)
						== ATT_READ_RESPONSE && valueLength >= 3) {
					sReports[i].reportId = value[1];
					sReports[i].reportType = value[2];
				}
			}
		}
	}
}


/* Read a whole attribute, however long it is: one read for the start and a
 * blob read for each piece after that.
 */
static size_t
ReadLongValue(uint16 handle, uint8* buffer, size_t size)
{
	uint8 request[5];
	request[0] = ATT_READ_REQUEST;
	request[1] = handle & 0xff;
	request[2] = handle >> 8;

	const uint8* response;
	size_t length;
	uint8 opcode = Request(request, 3, &response, &length);

	if (opcode == ATT_ERROR_RESPONSE && length >= 5) {
		printf("  reading %#04x: %s\n", handle, AttErrorName(response[4]));
		return 0;
	}
	if (opcode != ATT_READ_RESPONSE)
		return 0;

	size_t total = length - 1;
	if (total > size)
		total = size;
	memcpy(buffer, response + 1, total);

	/* A value shorter than the packet allows is the whole value. */
	while (length - 1 == (size_t)(sAttMtu - 1) && total < size) {
		request[0] = ATT_READ_BLOB_REQUEST;
		request[1] = handle & 0xff;
		request[2] = handle >> 8;
		request[3] = total & 0xff;
		request[4] = total >> 8;

		opcode = Request(request, 5, &response, &length);
		if (opcode != ATT_READ_BLOB_RESPONSE || length <= 1)
			break;

		size_t piece = length - 1;
		if (total + piece > size)
			piece = size - total;
		memcpy(buffer + total, response + 1, piece);
		total += piece;
	}

	return total;
}


static bool
WriteValue(uint16 handle, const uint8* value, size_t length)
{
	uint8 request[MAX_PACKET];
	request[0] = ATT_WRITE_REQUEST;
	request[1] = handle & 0xff;
	request[2] = handle >> 8;
	memcpy(request + 3, value, length);

	const uint8* response;
	size_t responseLength;
	uint8 opcode = Request(request, 3 + length, &response, &responseLength);

	if (opcode == ATT_ERROR_RESPONSE && responseLength >= 5) {
		printf("  writing %#04x: %s\n", handle, AttErrorName(response[4]));
		return false;
	}

	return opcode == ATT_WRITE_RESPONSE;
}


/* Enough of a HID report descriptor reader to say what the reports are: their
 * numbers, and how many bytes each takes. Nothing here interprets the fields.
 */
static void
DescribeReportMap(const uint8* map, size_t length)
{
	int reportId = 0;
	int reportSize = 0;
	int reportCount = 0;
	int bits = 0;
	const char* usage = "";

	printf("  the report map, %zu bytes:\n", length);
	for (size_t i = 0; i < length && i < 64; i++)
		printf("%s%02x", i % 24 == 0 ? "    " : " ", map[i]);
	if (length > 64)
		printf(" ...");
	printf("\n");

	size_t offset = 0;
	while (offset < length) {
		uint8 item = map[offset];
		uint8 size = item & 0x03;
		if (size == 3)
			size = 4;
		uint8 tag = item & 0xfc;

		uint32 value = 0;
		for (uint8 i = 0; i < size && offset + 1 + i < length; i++)
			value |= (uint32)map[offset + 1 + i] << (8 * i);

		switch (tag) {
			case 0x84:	/* report id */
				if (reportId != 0 && bits > 0) {
					printf("    report %d: %d bytes%s\n", reportId,
						(bits + 7) / 8, usage);
				}
				reportId = (int)value;
				bits = 0;
				usage = "";
				break;

			case 0x74:	/* report size */
				reportSize = (int)value;
				break;

			case 0x94:	/* report count */
				reportCount = (int)value;
				break;

			case 0x80:	/* input */
				bits += reportSize * reportCount;
				break;

			case 0xa0:	/* collection */
				break;

			case 0x04:	/* usage page */
				if (value == 0x01)
					usage = "  (a pointer or mouse)";
				else if (value == 0x0c)
					usage = "  (consumer controls)";
				break;
		}

		offset += 1 + size;
	}

	if (bits > 0)
		printf("    report %d: %d bytes%s\n", reportId, (bits + 7) / 8, usage);
}


//	#pragma mark - pairing


static void
FillRandom(uint8* buffer, size_t length)
{
	/* This is a bench tool and the value only has to be unpredictable to the
	 * device, not to an attacker; the stack will use a better source.
	 */
	static bool seeded = false;
	if (!seeded) {
		srandom((unsigned)(system_time() ^ (bigtime_t)find_thread(NULL)));
		seeded = true;
	}
	for (size_t i = 0; i < length; i++)
		buffer[i] = (uint8)(random() & 0xff);
}


static void
SendPairing(const uint8* data, size_t length)
{
	if (sVerbose)
		Dump("-> pairing", data, length);
	SendFrame(L2CAP_CID_SECURITY_MANAGER, data, length);
}


/* The Security Manager's side of the conversation, driven by what arrives.
 * Legacy "just works": the key both sides start from is zero, each commits to
 * a random number before revealing it, and out of the two randoms comes the
 * key that encrypts the link.
 */
static void
PairingStep(const uint8* data, size_t length)
{
	if (length < 1)
		return;

	if (sVerbose)
		Dump("<- pairing", data, length);

	switch (data[0]) {
		case SMP_PAIRING_RESPONSE:
		{
			if (length < 7)
				return;
			memcpy(sPairingResponse, data, 7);
			printf("  it will pair: input/output %#02x, authentication %#02x,"
				" keys up to %u bytes\n", data[1], data[3], data[4]);

			if ((data[3] & 0x08) != 0) {
				printf("  it asks for Secure Connections, which this does not"
					" do; trying the older way anyway\n");
			}

			FillRandom(sInitiatorRandom, 16);

			uint8 confirm[17];
			confirm[0] = SMP_PAIRING_CONFIRM;
			const uint8 zeroKey[16] = {};
			SecurityConfirm(zeroKey, sInitiatorRandom, sPairingRequest,
				sPairingResponse, sLocalAddressType, sLocalAddress,
				sRemoteAddressType, sRemoteAddress, confirm + 1);
			SendPairing(confirm, 17);
			sPairingStep = 1;
			break;
		}

		case SMP_PAIRING_CONFIRM:
		{
			if (length < 17)
				return;
			memcpy(sResponderConfirm, data + 1, 16);

			uint8 random[17];
			random[0] = SMP_PAIRING_RANDOM;
			memcpy(random + 1, sInitiatorRandom, 16);
			SendPairing(random, 17);
			sPairingStep = 2;
			break;
		}

		case SMP_PAIRING_RANDOM:
		{
			if (length < 17)
				return;
			memcpy(sResponderRandom, data + 1, 16);

			/* Check it committed to the number it has now revealed. If this
			 * does not match, something is between us and the device.
			 */
			const uint8 zeroKey[16] = {};
			uint8 expected[16];
			SecurityConfirm(zeroKey, sResponderRandom, sPairingRequest,
				sPairingResponse, sLocalAddressType, sLocalAddress,
				sRemoteAddressType, sRemoteAddress, expected);

			if (memcmp(expected, sResponderConfirm, 16) != 0) {
				printf("  [!] its confirm value does not match what it sent\n");
				uint8 failed[2] = { SMP_PAIRING_FAILED, 0x04 };
				SendPairing(failed, 2);
				sPairingFailed = true;
				return;
			}

			printf("  its confirm value checks out\n");
			SecuritySessionKey(zeroKey, sResponderRandom, sInitiatorRandom,
				sSessionKey);

			/* Turn encryption on with that key. A short term key is used with
			 * a diversifier and random of zero, which is what says "this is a
			 * key we just made" rather than one from a previous pairing.
			 */
			uint8 parameters[28];
			parameters[0] = sConnection & 0xff;
			parameters[1] = sConnection >> 8;
			memset(parameters + 2, 0, 10);
			memcpy(parameters + 12, sSessionKey, 16);

			if (SendCommand(HCI_LE_START_ENCRYPTION, parameters, 28) != B_OK)
				sPairingFailed = true;
			sPairingStep = 3;
			break;
		}

		case SMP_ENCRYPTION_INFORMATION:
			if (length < 17)
				return;
			memcpy(sLongTermKey, data + 1, 16);
			sHaveLongTermKey = true;
			printf("  it gave us a long term key\n");
			break;

		case SMP_MASTER_IDENTIFICATION:
			if (length < 11)
				return;
			sKeyDiversifier = data[1] | (data[2] << 8);
			memcpy(sKeyRandom, data + 3, 8);
			printf("  and the numbers that name it\n");
			break;

		case SMP_IDENTITY_INFORMATION:
		case SMP_IDENTITY_ADDRESS:
		case SMP_SIGNING_INFORMATION:
			/* Keys for resolving private addresses and for signing: neither
			 * is needed to read a mouse.
			 */
			break;

		case SMP_PAIRING_FAILED:
			sPairingFailed = true;
			sPairingFailure = length >= 2 ? data[1] : 0;
			printf("  [!] it refused to pair: %s (%#02x)\n",
				PairingFailureName(sPairingFailure), sPairingFailure);
			break;

		case SMP_SECURITY_REQUEST:
			printf("  it asks for the link to be secured\n");
			break;

		default:
			printf("  pairing message %#02x, which nothing here handles\n",
				data[0]);
			break;
	}
}


/* Wait while the pairing conversation runs. */
static bool
Pair()
{
	printf("pairing:\n");

	sPairingRequest[0] = SMP_PAIRING_REQUEST;
	sPairingRequest[1] = 0x03;	/* nothing to type on, nothing to show */
	sPairingRequest[2] = 0x00;	/* no out of band data */
	sPairingRequest[3] = 0x01;	/* bonding, no man-in-the-middle protection */
	sPairingRequest[4] = 0x10;	/* keys of the full sixteen bytes */
	sPairingRequest[5] = 0x00;	/* we distribute nothing */
	sPairingRequest[6] = 0x01;	/* it should give us its long term key */

	SendPairing(sPairingRequest, 7);

	bigtime_t end = system_time() + 30000000;
	while (system_time() < end && !sPairingFailed && !sDisconnected) {
		queued_packet packet;
		if (Dequeue(&packet, end - system_time()) != B_OK)
			break;

		if (packet.type == 1) {
			HandleData(packet.data, packet.length);
			continue;
		}

		if (packet.length < 2)
			continue;

		if (packet.data[0] == HCI_EVENT_COMMAND_STATUS
			&& packet.length >= 6) {
			uint16 answered = packet.data[4] | (packet.data[5] << 8);
			if (answered == HCI_LE_START_ENCRYPTION && packet.data[2] != 0) {
				printf("  [!] the controller refused to encrypt: %#02x\n",
					packet.data[2]);
				return false;
			}
			continue;
		}

		HandleUnsolicitedEvent(packet.data, packet.length);

		/* Once the link is encrypted the device sends its keys, and pairing
		 * is done when they have arrived - or shortly after, if it sends
		 * none.
		 */
		if (sEncrypted && sHaveLongTermKey)
			break;
		if (sEncrypted && end > system_time() + 3000000)
			end = system_time() + 3000000;
	}

	return sEncrypted;
}


static void
StoreKeys(const char* path)
{
	if (!sEncrypted)
		return;

	/* The key is what lets anything decrypt this mouse's traffic, so it is
	 * written where only its owner can read it, and never printed.
	 */
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		printf("  could not save the keys to %s: %s\n", path, strerror(errno));
		return;
	}

	char line[256];
	int length = snprintf(line, sizeof(line), "address %s\ntype %u\n",
		AddressToString(sRemoteAddress), sRemoteAddressType);
	write(fd, line, length);

	if (sHaveLongTermKey) {
		length = snprintf(line, sizeof(line), "diversifier %u\n",
			sKeyDiversifier);
		write(fd, line, length);

		char keyText[80];
		int at = 0;
		for (int i = 0; i < 16; i++)
			at += snprintf(keyText + at, sizeof(keyText) - at, "%02x",
				sLongTermKey[i]);
		length = snprintf(line, sizeof(line), "key %s\nrandom ", keyText);
		write(fd, line, length);
		at = 0;
		for (int i = 0; i < 8; i++)
			at += snprintf(keyText + at, sizeof(keyText) - at, "%02x",
				sKeyRandom[i]);
		length = snprintf(line, sizeof(line), "%s\n", keyText);
		write(fd, line, length);
	}

	close(fd);
	printf("  keys written to %s\n", path);
}


//	#pragma mark -


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char* path = "/dev/bluetooth/h2/h2generic/0";
	const char* address = NULL;
	const char* keyPath = NULL;
	uint8 addressType = 0x01;
	int seconds = 20;
	bool pair = true;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
			path = argv[++i];
		else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
			address = argv[++i];
		else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc)
			keyPath = argv[++i];
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (strcmp(argv[i], "-r") == 0)
			addressType = 0x00;
		else if (strcmp(argv[i], "-P") == 0)
			pair = false;
		else if (strcmp(argv[i], "-v") == 0)
			sVerbose = true;
		else {
			fprintf(stderr, "usage: %s [-d device] [-a address] [-r] [-P]"
				" [-s seconds] [-k keys] [-v]\n", argv[0]);
			return 1;
		}
	}

	char keyDefault[256];
	if (keyPath == NULL) {
		const char* home = getenv("HOME");
		snprintf(keyDefault, sizeof(keyDefault), "%s/config/settings/"
			"btlehid_keys", home != NULL ? home : "/boot/home");
		keyPath = keyDefault;
	}

	printf("checking the pairing arithmetic against its published"
		" examples... ");
	if (!CheckCrypto()) {
		fprintf(stderr, "[!] refusing to go on with broken crypto\n");
		return 1;
	}
	printf("all three match\n\n");

	/* Stand in for the Bluetooth server: the kernel hands events and data to
	 * whoever owns a port of this name, and two owners would be one too many.
	 */
	if (find_port(BT_USERLAND_PORT_NAME) >= 0) {
		fprintf(stderr, "[!] something already owns \"%s\" - stop the"
			" Bluetooth server first\n", BT_USERLAND_PORT_NAME);
		return 1;
	}

	sPort = create_port(128, BT_USERLAND_PORT_NAME);
	if (sPort < 0) {
		fprintf(stderr, "[!] cannot create the port: %s\n", strerror(sPort));
		return 1;
	}

	sDevice = open(path, O_RDWR);
	if (sDevice < 0) {
		fprintf(stderr, "[!] cannot open %s: %s\n", path, strerror(errno));
		delete_port(sPort);
		return 1;
	}

	/* This is what starts the driver reading: without it nothing arrives. */
	uint32 up = 0;
	if (ioctl(sDevice, BT_UP, &up, sizeof(up)) != 0) {
		fprintf(stderr, "[!] the driver would not come up: %s\n",
			strerror(errno));
		close(sDevice);
		delete_port(sPort);
		return 1;
	}

	if (!Command("resetting the controller", HCI_RESET, NULL, 0)) {
		fprintf(stderr, "\n[!] the controller does not answer. It probably has"
			" no firmware yet - run btfw.\n");
		exit(1);
	}

	const uint8 eventMask[8]
		= { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
	Command("unmasking the LE event", HCI_SET_EVENT_MASK, eventMask, 8);

	const uint8 leHost[2] = { 0x01, 0x00 };
	Command("saying the host speaks LE", HCI_WRITE_LE_HOST_SUPPORT, leHost, 2);

	/* Unmask every LE event the controller has, including the two that only
	 * matter once there is a connection.
	 */
	const uint8 leEventMask[8]
		= { 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 };
	Command("unmasking the LE subevents", HCI_LE_SET_EVENT_MASK, leEventMask, 8);

	uint8 answer[64];
	size_t answerLength = sizeof(answer);
	if (Command(NULL, 0x1009 /* read the local address */, NULL, 0, answer,
			&answerLength) && answerLength >= 7) {
		memcpy(sLocalAddress, answer + 1, 6);
		printf("this adapter is %s\n", AddressToString(sLocalAddress));
	}

	found_device found;
	if (address != NULL) {
		AddressFromString(address, found.address);
		found.addressType = addressType;
		found.name[0] = '\0';
	} else if (!ScanForMouse(20, &found)) {
		printf("\nnothing nearby advertises itself as a mouse or keyboard."
			" Put it in pairing mode and try again.\n");
		exit(1);
	}

	memcpy(sRemoteAddress, found.address, 6);
	sRemoteAddressType = found.addressType;

	printf("\n");
	if (!Connect(found.address, found.addressType))
		exit(1);

	printf("\n");
	ExchangeMtu(247);

	printf("what it offers:\n");
	if (!DiscoverServices()) {
		printf("[!] it lists no services\n");
		exit(1);
	}

	uint16 hidStart = 0;
	uint16 hidEnd = 0;
	for (int i = 0; i < sServiceCount; i++) {
		if (sServices[i].uuid == UUID_HID_SERVICE) {
			hidStart = sServices[i].start;
			hidEnd = sServices[i].end;
		}
	}

	if (hidStart == 0) {
		printf("[!] it has no human interface service, so it is not a mouse\n");
		exit(1);
	}

	printf("\nthe human interface service (%04x-%04x):\n", hidStart, hidEnd);
	DiscoverCharacteristics(hidStart, hidEnd, true);

	/* The battery service is worth having too: it is the one other thing a
	 * mouse says about itself that anyone wants to see.
	 */
	for (int i = 0; i < sServiceCount; i++) {
		if (sServices[i].uuid == UUID_BATTERY_SERVICE)
			DiscoverCharacteristics(sServices[i].start, sServices[i].end, false);
	}

	printf("  %d report characteristic%s, report map at %#04x\n", sReportCount,
		sReportCount == 1 ? "" : "s", sReportMapHandle);

	if (pair) {
		printf("\n");
		if (!Pair()) {
			printf("\n[!] pairing did not finish%s\n", sPairingFailed
				? "" : " and nothing said why");
			exit(1);
		}
		StoreKeys(keyPath);
	}

	printf("\nreading what needs an encrypted link:\n");

	uint8 map[512];
	size_t mapLength = ReadLongValue(sReportMapHandle, map, sizeof(map));
	if (mapLength > 0)
		DescribeReportMap(map, mapLength);

	DiscoverReportDescriptors(hidEnd);

	if (sProtocolModeHandle != 0) {
		const uint8 reportMode = 0x01;
		uint8 request[4];
		request[0] = 0x52;	/* write without a response */
		request[1] = sProtocolModeHandle & 0xff;
		request[2] = sProtocolModeHandle >> 8;
		request[3] = reportMode;
		SendFrame(L2CAP_CID_ATTRIBUTE, request, 4);
		printf("  asked it for report mode\n");
	}

	printf("\nsubscribing to its reports:\n");
	int subscribed = 0;
	for (int i = 0; i < sReportCount; i++) {
		if (sReports[i].configurationHandle == 0)
			continue;
		/* Only input reports are sent to us; the others are ours to send. */
		if (sReports[i].reportType != 0 && sReports[i].reportType != 0x01)
			continue;

		const uint8 notifications[2] = { 0x01, 0x00 };
		if (WriteValue(sReports[i].configurationHandle, notifications, 2)) {
			printf("  report %u at handle %#04x: on\n", sReports[i].reportId,
				sReports[i].valueHandle);
			subscribed++;
		}
	}

	if (sBatteryLevelHandle != 0) {
		uint8 level[8];
		if (ReadLongValue(sBatteryLevelHandle, level, sizeof(level)) >= 1)
			printf("  battery: %u%%\n", level[0]);
	}

	if (subscribed == 0) {
		printf("[!] nothing to listen to\n");
		exit(1);
	}

	printf("\nmove the mouse - listening for %d seconds\n", seconds);

	bigtime_t end = system_time() + (bigtime_t)seconds * 1000000;
	while (system_time() < end && !sDisconnected) {
		queued_packet packet;
		if (Dequeue(&packet, end - system_time()) != B_OK)
			continue;

		if (packet.type == 1)
			HandleData(packet.data, packet.length);
		else if (packet.length >= 2)
			HandleUnsolicitedEvent(packet.data, packet.length);
	}

	printf("\n%d report%s arrived\n", sNotifications,
		sNotifications == 1 ? "" : "s");

	if (!sDisconnected) {
		uint8 parameters[3];
		parameters[0] = sConnection & 0xff;
		parameters[1] = sConnection >> 8;
		parameters[2] = 0x13;	/* the user is done with it */
		Command("disconnecting", HCI_DISCONNECT, parameters, 3);
	}

	close(sDevice);
	delete_port(sPort);

	return sNotifications > 0 ? 0 : 1;
}
