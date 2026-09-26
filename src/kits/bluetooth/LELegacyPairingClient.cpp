/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <LELegacyPairingClient.h>

#include <LELegacyPairingCrypto.h>
#include <LELog.h>
#include <LEPairingSession.h>

#include <Message.h>
#include <Messenger.h>

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <Errors.h>


namespace Bluetooth {

static const uint8 kPairingRequest = 0x01;
static const uint8 kPairingResponse = 0x02;
static const uint8 kPairingConfirm = 0x03;
static const uint8 kPairingRandom = 0x04;
static const uint8 kPairingFailed = 0x05;
static const uint8 kEncryptionInformation = 0x06;
static const uint8 kCentralIdentification = 0x07;
static const uint8 kIdentityInformation = 0x08;
static const uint8 kIdentityAddressInformation = 0x09;
static const uint8 kSigningInformation = 0x0a;
static const uint8 kSecurityRequest = 0x0b;
static const uint8 kReasonConfirmFailed = 0x04;
static const uint8 kReasonAuthenticationRequirements = 0x03;
static const uint8 kReasonInvalidParameters = 0x0a;


static void
ReportStage(const BMessenger* progress, LEPairingStage stage)
{
	if (progress != NULL && progress->IsValid()) {
		BMessage message(LE_PAIRING_PROGRESS_MESSAGE);
		message.AddInt32("stage", stage);
		progress->SendMessage(&message);
	}
}


#define LOG(level, format, args...) LELog(level, "smp", format, ##args)


static const char*
SMPCodeName(uint8 code)
{
	static const char* kNames[] = { "?", "Pairing Request", "Pairing Response",
		"Pairing Confirm", "Pairing Random", "Pairing Failed",
		"Encryption Information", "Central Identification",
		"Identity Information", "Identity Address Information",
		"Signing Information", "Security Request", "Pairing Public Key",
		"Pairing DHKey Check", "Keypress Notification" };
	return code < B_COUNT_OF(kNames) ? kNames[code] : "unknown";
}


static const char*
SMPReasonName(uint8 reason)
{
	static const char* kNames[] = { "?", "Passkey Entry Failed",
		"OOB Not Available", "Authentication Requirements",
		"Confirm Value Failed", "Pairing Not Supported",
		"Encryption Key Size", "Command Not Supported", "Unspecified Reason",
		"Repeated Attempts", "Invalid Parameters", "DHKey Check Failed",
		"Numeric Comparison Failed", "BR/EDR pairing in progress",
		"Cross-transport Key Derivation not allowed", "Key Rejected" };
	return reason < B_COUNT_OF(kNames) ? kNames[reason] : "unknown";
}


static void
SetDetail(char* detail, size_t size, const char* format, ...)
{
	if (detail == NULL || size == 0)
		return;
	va_list args;
	va_start(args, format);
	vsnprintf(detail, size, format, args);
	va_end(args);
	LOG(LE_LOG_ERROR, "%s", detail);
}


static void
LogPairingFeatures(const char* direction, const uint8* features)
{
	LOG(LE_LOG_INFO, "%s %s: IO capability %u, OOB %u, AuthReq %#04x "
		"(bonding %u, MITM %u, SC %u, keypress %u, CT2 %u), max key %u, "
		"initiator keys %#x, responder keys %#x", direction,
		SMPCodeName(features[0]), features[1], features[2], features[3],
		features[3] & 0x03, (features[3] >> 2) & 1, (features[3] >> 3) & 1,
		(features[3] >> 4) & 1, (features[3] >> 5) & 1, features[4],
		features[5], features[6]);
}


static void
ClearSecret(void* data, size_t length)
{
	volatile uint8* bytes = (volatile uint8*)data;
	while (length-- > 0)
		*bytes++ = 0;
}


static status_t
SendPacket(int descriptor, const uint8* packet, size_t length)
{
	ssize_t sent;
	do {
		sent = send(descriptor, packet, length, 0);
	} while (sent < 0 && errno == EINTR);
	if (sent == (ssize_t)length) {
		LOG(LE_LOG_DEBUG, "-> %s (%zu bytes)", SMPCodeName(packet[0]), length);
		return B_OK;
	}
	status_t error = sent < 0 ? errno : B_IO_ERROR;
	LOG(LE_LOG_ERROR, "sending %s failed: %s", SMPCodeName(packet[0]),
		strerror(error));
	return error;
}


static void
SendFailure(int descriptor, uint8 reason)
{
	const uint8 packet[2] = { kPairingFailed, reason };
	SendPacket(descriptor, packet, sizeof(packet));
}


static status_t
ReceivePacket(int descriptor, uint8* packet, size_t capacity,
	size_t& length, const timespec& deadline)
{
	for (;;) {
		timespec now;
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			return B_ERROR;
		long seconds = deadline.tv_sec - now.tv_sec;
		long microseconds = (deadline.tv_nsec - now.tv_nsec) / 1000;
		if (microseconds < 0) {
			seconds--;
			microseconds += 1000000;
		}
		if (seconds < 0 || (seconds == 0 && microseconds == 0))
			return B_TIMED_OUT;
		pollfd readable = { descriptor, POLLIN, 0 };
		int milliseconds = seconds * 1000 + (microseconds + 999) / 1000;
		int ready = poll(&readable, 1, milliseconds);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return errno;
		if (ready == 0)
			return B_TIMED_OUT;
		ssize_t received = recv(descriptor, packet, capacity, 0);
		if (received < 0 && errno == EINTR)
			continue;
		if (received < 0) {
			status_t error = errno;
			LOG(LE_LOG_ERROR, "receive failed: %s (link lost?)",
				strerror(error));
			return error == ENOTCONN ? B_DEV_NOT_READY : error;
		}
		if (received == 0)
			return B_DEV_NOT_READY;
		length = (size_t)received;
		// A Security Request is the responder asking us to do what we are
		// already doing; it can cross our Pairing Request on the air.
		if (packet[0] == kSecurityRequest) {
			LOG(LE_LOG_INFO, "<- Security Request (AuthReq %#04x); ignored "
				"during pairing", length > 1 ? packet[1] : 0);
			continue;
		}
		if (packet[0] == kPairingFailed && length >= 2) {
			LOG(LE_LOG_ERROR, "<- Pairing Failed: reason %#x (%s)", packet[1],
				SMPReasonName(packet[1]));
		} else {
			LOG(LE_LOG_DEBUG, "<- %s (%zu bytes)", SMPCodeName(packet[0]),
				length);
		}
		return B_OK;
	}
}


static bool
EqualSecret(const uint8* first, const uint8* second, size_t length)
{
	uint8 difference = 0;
	for (size_t i = 0; i < length; i++)
		difference |= first[i] ^ second[i];
	return difference == 0;
}


status_t
LELegacyPairJustWorks(int descriptor, const uint8 initiatorAddress[6],
	uint8 initiatorAddressType, const uint8 responderAddress[6],
	uint8 responderAddressType, uint8 shortTermKey[16],
	uint8* responderKeyDistribution, uint8* negotiatedKeySize,
	const BMessenger* progress, char* detail, size_t detailSize)
{
	if (detail != NULL && detailSize > 0)
		detail[0] = '\0';
	if (descriptor < 0 || initiatorAddress == NULL || responderAddress == NULL
		|| shortTermKey == NULL || initiatorAddressType > 1
		|| responderAddressType > 1)
		return B_BAD_VALUE;

	memset(shortTermKey, 0, 16);
	if (responderKeyDistribution != NULL)
		*responderKeyDistribution = 0;
	if (negotiatedKeySize != NULL)
		*negotiatedKeySize = 0;
	// NoInputNoOutput, no OOB, bonding, 128-bit key, no initiator key,
	// request the responder's encryption and identity keys for reconnection.
	const uint8 request[7] = { kPairingRequest, 0x03, 0, 0x01, 16, 0, 3 };
	uint8 response[64] = {};
	uint8 initiatorRandom[16] = {};
	uint8 responderRandom[16] = {};
	uint8 expected[16] = {};
	uint8 temporaryKey[16] = {};
	uint8 packet[17] = {};
	size_t length = 0;
	status_t status = B_ERROR;
	timespec deadline;
	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
		goto done;
	// Core Vol 3 Part H 3.4: the SMP transaction timeout is 30 seconds.
	deadline.tv_sec += 30;
	char initiatorText[18], responderText[18];
	LOG(LE_LOG_INFO, "legacy Just Works pairing: initiator %s (type %u), "
		"responder %s (type %u)", LEAddressString(initiatorAddress,
			initiatorText), initiatorAddressType,
		LEAddressString(responderAddress, responderText),
		responderAddressType);
	LogPairingFeatures("->", request);
	status = SendPacket(descriptor, request, sizeof(request));
	if (status != B_OK) {
		SetDetail(detail, detailSize, "Could not send the pairing request "
			"(%s).", strerror(status));
		goto done;
	}
	ReportStage(progress, LE_PAIRING_SMP_RESPONSE);
	status = ReceivePacket(descriptor, response, sizeof(response), length,
		deadline);
	if (status != B_OK) {
		SetDetail(detail, detailSize, status == B_TIMED_OUT
			? "The device did not answer the pairing request within 30 "
				"seconds. It may not be in pairing mode."
			: "No pairing response: %s.", strerror(status));
		goto done;
	}
	if (length >= 2 && response[0] == kPairingFailed) {
		SetDetail(detail, detailSize, "The device refused pairing: %s "
			"(reason %#x).", SMPReasonName(response[1]), response[1]);
		status = B_PERMISSION_DENIED;
		goto done;
	}
	if (length != 7 || response[0] != kPairingResponse) {
		SetDetail(detail, detailSize, "Unexpected %s (%zu bytes) instead of a "
			"pairing response.", SMPCodeName(response[0]), length);
		SendFailure(descriptor, kReasonInvalidParameters);
		status = B_BAD_DATA;
		goto done;
	}
	LogPairingFeatures("<-", response);
	if (response[1] > 4 || response[4] < 7 || response[4] > 16
		|| (response[3] & 0x03) > 1 || response[5] != 0
		|| (response[6] & ~request[6]) != 0) {
		SetDetail(detail, detailSize, "The pairing response has invalid "
			"parameters (IO %u, AuthReq %#x, key size %u, keys %#x/%#x).",
			response[1], response[3], response[4], response[5], response[6]);
		SendFailure(descriptor, kReasonInvalidParameters);
		status = B_BAD_DATA;
		goto done;
	}
	if ((response[3] & 0x04) != 0) {
		// Without a display or keyboard on either side, only Just Works is
		// possible; the responder decides whether that is acceptable.
		LOG(LE_LOG_INFO, "responder asks for MITM protection; neither side "
			"can provide it, continuing with Just Works");
	}
	if ((response[3] & 0x08) != 0) {
		LOG(LE_LOG_INFO, "responder supports Secure Connections; this host "
			"did not request it, so legacy pairing is used");
	}
	if (getentropy(initiatorRandom, sizeof(initiatorRandom)) != 0) {
		SetDetail(detail, detailSize, "No random numbers available.");
		status = B_ERROR;
		goto done;
	}
	if (!LELegacyConfirm(temporaryKey, initiatorRandom, request, response,
		initiatorAddressType, initiatorAddress, responderAddressType,
		responderAddress, packet + 1)) {
		SetDetail(detail, detailSize, "Could not compute the confirm value.");
		status = B_ERROR;
		goto done;
	}
	packet[0] = kPairingConfirm;
	ReportStage(progress, LE_PAIRING_SMP_CONFIRM);
	status = SendPacket(descriptor, packet, sizeof(packet));
	if (status != B_OK) {
		SetDetail(detail, detailSize, "Could not send the pairing confirm "
			"(%s).", strerror(status));
		goto done;
	}
	status = ReceivePacket(descriptor, response + 7, sizeof(response) - 7,
		length, deadline);
	if (status != B_OK) {
		SetDetail(detail, detailSize, "No confirm value from the device: %s.",
			strerror(status));
		goto done;
	}
	if (length >= 2 && response[7] == kPairingFailed) {
		SetDetail(detail, detailSize, "The device aborted pairing after our "
			"confirm: %s (reason %#x).", SMPReasonName(response[8]),
			response[8]);
		status = B_PERMISSION_DENIED;
		goto done;
	}
	if (length != 17 || response[7] != kPairingConfirm) {
		SetDetail(detail, detailSize, "Unexpected %s (%zu bytes) instead of "
			"the device's confirm value.", SMPCodeName(response[7]), length);
		SendFailure(descriptor, kReasonInvalidParameters);
		status = B_BAD_DATA;
		goto done;
	}
	packet[0] = kPairingRandom;
	memcpy(packet + 1, initiatorRandom, sizeof(initiatorRandom));
	ReportStage(progress, LE_PAIRING_SMP_RANDOM);
	status = SendPacket(descriptor, packet, sizeof(packet));
	if (status != B_OK) {
		SetDetail(detail, detailSize, "Could not send the pairing random "
			"(%s).", strerror(status));
		goto done;
	}
	status = ReceivePacket(descriptor, packet, sizeof(packet), length,
		deadline);
	if (status != B_OK) {
		SetDetail(detail, detailSize, "No random value from the device: %s.",
			strerror(status));
		goto done;
	}
	if (length >= 2 && packet[0] == kPairingFailed) {
		SetDetail(detail, detailSize, "The device rejected our confirm value: "
			"%s (reason %#x). The address or pairing features may not match "
			"what the device used.", SMPReasonName(packet[1]), packet[1]);
		status = B_PERMISSION_DENIED;
		goto done;
	}
	if (length != 17 || packet[0] != kPairingRandom) {
		SetDetail(detail, detailSize, "Unexpected %s (%zu bytes) instead of "
			"the device's random value.", SMPCodeName(packet[0]), length);
		SendFailure(descriptor, kReasonInvalidParameters);
		status = B_BAD_DATA;
		goto done;
	}
	memcpy(responderRandom, packet + 1, sizeof(responderRandom));
	if (!LELegacyConfirm(temporaryKey, responderRandom, request, response,
		initiatorAddressType, initiatorAddress, responderAddressType,
		responderAddress, expected)) {
		SetDetail(detail, detailSize, "Could not verify the device's confirm "
			"value.");
		status = B_ERROR;
		goto done;
	}
	if (!EqualSecret(expected, response + 8, 16)) {
		SetDetail(detail, detailSize, "The device's confirm value does not "
			"match its random value (wrong address or type?).");
		SendFailure(descriptor, kReasonConfirmFailed);
		status = B_BAD_DATA;
		goto done;
	}
	if (!LELegacyShortTermKey(temporaryKey, responderRandom, initiatorRandom,
		shortTermKey)) {
		SetDetail(detail, detailSize, "Could not derive the short-term key.");
		status = B_ERROR;
		goto done;
	}
	for (uint8 i = response[4]; i < 16; i++)
		shortTermKey[i] = 0;
	if (responderKeyDistribution != NULL)
		*responderKeyDistribution = response[6];
	if (negotiatedKeySize != NULL)
		*negotiatedKeySize = response[4];
	LOG(LE_LOG_INFO, "confirm values verified; short-term key ready (%u-byte "
		"key, responder will send keys %#x)", response[4], response[6]);
	status = B_OK;

done:
	if (status != B_OK)
		ClearSecret(shortTermKey, 16);
	ClearSecret(initiatorRandom, sizeof(initiatorRandom));
	ClearSecret(responderRandom, sizeof(responderRandom));
	ClearSecret(expected, sizeof(expected));
	ClearSecret(temporaryKey, sizeof(temporaryKey));
	ClearSecret(packet, sizeof(packet));
	return status;
}


status_t
LELegacyReceiveResponderKeys(int descriptor, uint8 expectedDistribution,
	uint8 negotiatedKeySize, LELegacyBondKey& output, char* detail,
	size_t detailSize)
{
	if (detail != NULL && detailSize > 0)
		detail[0] = '\0';
	ClearSecret(&output, sizeof(output));
	if (descriptor < 0
		|| (expectedDistribution & ~0x03) != 0
		|| negotiatedKeySize < 7 || negotiatedKeySize > 16)
		return B_BAD_VALUE;
	output.keySize = negotiatedKeySize;
	if (expectedDistribution == 0) {
		LOG(LE_LOG_INFO, "device distributes no keys; nothing to store");
		return B_OK;
	}

	timespec deadline;
	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
		return B_ERROR;
	deadline.tv_sec += 30;
	uint8 packet[32];
	size_t length = 0;
	status_t status = B_OK;
	bool haveEncryption = false, haveCentral = false;
	bool haveIdentity = false, haveAddress = false;
	const bool wantEncryption = (expectedDistribution & 0x01) != 0;
	const bool wantIdentity = (expectedDistribution & 0x02) != 0;
	LOG(LE_LOG_INFO, "waiting for the device's keys (%s%s)",
		wantEncryption ? "LTK " : "", wantIdentity ? "IRK" : "");

	// Core Vol 3 Part H 3.6.1 fixes the order, but accept any order and
	// ignore keys that were not requested rather than fail the bond.
	while ((wantEncryption && !(haveEncryption && haveCentral))
		|| (wantIdentity && !(haveIdentity && haveAddress))) {
		status = ReceivePacket(descriptor, packet, sizeof(packet), length,
			deadline);
		if (status != B_OK) {
			SetDetail(detail, detailSize, status == B_TIMED_OUT
				? "The device did not send its keys within 30 seconds."
				: "Receiving the device's keys failed: %s.", strerror(status));
			goto done;
		}
		switch (packet[0]) {
			case kPairingFailed:
				SetDetail(detail, detailSize, "The device aborted key "
					"distribution: %s (reason %#x).",
					length >= 2 ? SMPReasonName(packet[1]) : "?",
					length >= 2 ? packet[1] : 0);
				status = B_PERMISSION_DENIED;
				goto done;
			case kEncryptionInformation:
				if (length != 17)
					goto invalid;
				memcpy(output.longTermKey, packet + 1, 16);
				haveEncryption = true;
				break;
			case kCentralIdentification:
				if (length != 11)
					goto invalid;
				output.encryptedDiversifier = packet[1]
					| ((uint16)packet[2] << 8);
				memcpy(output.randomNumber, packet + 3, 8);
				haveCentral = true;
				break;
			case kIdentityInformation:
				if (length != 17)
					goto invalid;
				memcpy(output.identityResolvingKey, packet + 1, 16);
				haveIdentity = true;
				break;
			case kIdentityAddressInformation:
			{
				if (length != 8 || packet[1] > 1)
					goto invalid;
				output.identityAddressType = packet[1];
				memcpy(output.identityAddress, packet + 2, 6);
				haveAddress = true;
				char text[18];
				LOG(LE_LOG_INFO, "device identity address %s (type %u)",
					LEAddressString(output.identityAddress, text), packet[1]);
				break;
			}
			case kSigningInformation:
				LOG(LE_LOG_INFO, "ignoring unrequested signing key");
				break;
			default:
				LOG(LE_LOG_ERROR, "unexpected %s during key distribution",
					SMPCodeName(packet[0]));
				goto invalid;
		}
	}
	if (haveEncryption && haveCentral) {
		for (uint8 i = negotiatedKeySize; i < 16; i++)
			output.longTermKey[i] = 0;
		output.hasLongTermKey = true;
	}
	output.hasIdentity = haveIdentity && haveAddress;
	LOG(LE_LOG_INFO, "keys received: LTK %s, identity %s",
		output.hasLongTermKey ? "yes" : "no", output.hasIdentity ? "yes" : "no");
	status = B_OK;
	goto done;

invalid:
	SetDetail(detail, detailSize, "The device sent a malformed %s (%zu "
		"bytes).", SMPCodeName(packet[0]), length);
	SendFailure(descriptor, kReasonInvalidParameters);
	status = B_BAD_DATA;
done:
	ClearSecret(packet, sizeof(packet));
	if (status != B_OK)
		ClearSecret(&output, sizeof(output));
	return status;
}

} // namespace Bluetooth
