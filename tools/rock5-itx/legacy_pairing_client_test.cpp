/* Scripted LE legacy Just Works peer; no Bluetooth controller is contacted. */
#include <LELegacyPairingClient.h>
#include <LELegacyPairingCrypto.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>


static const uint8 kLocalAddress[6] = { 1, 2, 3, 4, 5, 6 };
static const uint8 kPeerAddress[6] = { 7, 8, 9, 10, 11, 12 };
static const uint8 kPeerRandom[16] = {
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10
};


struct PeerState {
	int descriptor;
	bool tamper;
	bool pass;
	uint8 shortTermKey[16];
};


static bool
RunPeer(PeerState* state)
{
	const uint8 tk[16] = {};
	uint8 request[7];
	if (recv(state->descriptor, request, sizeof(request), 0) != 7)
		return false;
	if (memcmp(request, "\x01\x03\x00\x01\x10\x00\x03", 7) != 0)
		return false;
	// A peripheral may send a Security Request that crosses our request;
	// the client must skip it. The responder here also asks for MITM, which
	// Just Works cannot give but which the peer, not us, must reject.
	const uint8 securityRequest[2] = { 0x0b, 0x05 };
	if (send(state->descriptor, securityRequest, sizeof(securityRequest), 0)
			!= 2)
		return false;
	const uint8 response[7] = { 2, 3, 0, 0x05, 16, 0, 3 };
	if (send(state->descriptor, response, sizeof(response), 0) != 7)
		return false;
	uint8 peerConfirm[17] = { 3 };
	if (!Bluetooth::LELegacyConfirm(tk, kPeerRandom, request, response,
		0, kLocalAddress, 1, kPeerAddress, peerConfirm + 1))
		return false;
	if (state->tamper)
		peerConfirm[1] ^= 1;
	uint8 localConfirm[17];
	if (recv(state->descriptor, localConfirm, sizeof(localConfirm), 0) != 17
		|| localConfirm[0] != 3)
		return false;
	if (send(state->descriptor, peerConfirm, sizeof(peerConfirm), 0) != 17)
		return false;
	uint8 localRandom[17];
	if (recv(state->descriptor, localRandom, sizeof(localRandom), 0) != 17
		|| localRandom[0] != 4)
		return false;
	uint8 expected[16];
	if (!Bluetooth::LELegacyConfirm(tk, localRandom + 1, request,
		response, 0, kLocalAddress, 1, kPeerAddress, expected)
		|| memcmp(expected, localConfirm + 1, 16) != 0)
		return false;
	uint8 peerRandomPacket[17] = { 4 };
	memcpy(peerRandomPacket + 1, kPeerRandom, 16);
	if (send(state->descriptor, peerRandomPacket,
		sizeof(peerRandomPacket), 0) != 17)
		return false;
	if (state->tamper) {
		uint8 failed[2];
		return recv(state->descriptor, failed, sizeof(failed), 0) == 2
			&& failed[0] == 5 && failed[1] == 4;
	}
	return Bluetooth::LELegacyShortTermKey(tk, kPeerRandom,
		localRandom + 1, state->shortTermKey);
}


static void*
Peer(void* argument)
{
	PeerState* state = (PeerState*)argument;
	state->pass = RunPeer(state);
	close(state->descriptor);
	return NULL;
}


static bool
RunCase(bool tamper, bool useHighDescriptor = false,
	bool* highDescriptorUsed = NULL)
{
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0)
		return false;
	if (highDescriptorUsed != NULL)
		*highDescriptorUsed = false;
	if (useHighDescriptor) {
		int high = fcntl(sockets[0], F_DUPFD, FD_SETSIZE + 16);
		if (high >= 0) {
			close(sockets[0]);
			sockets[0] = high;
			if (highDescriptorUsed != NULL)
				*highDescriptorUsed = true;
		}
	}
	PeerState state = { sockets[1], tamper, false, {} };
	pthread_t thread;
	if (pthread_create(&thread, NULL, Peer, &state) != 0) {
		close(sockets[0]);
		close(sockets[1]);
		return false;
	}
	uint8 key[16];
	uint8 distribution = 0;
	uint8 keySize = 0;
	status_t status = Bluetooth::LELegacyPairJustWorks(sockets[0],
		kLocalAddress, 0, kPeerAddress, 1, key, &distribution, &keySize);
	close(sockets[0]);
	pthread_join(thread, NULL);
	return state.pass && (tamper ? status == B_BAD_DATA
		: status == B_OK && distribution == 3 && keySize == 16
			&& memcmp(key, state.shortTermKey, 16) == 0);
}


enum KeyCase {
	KEYS_IN_ORDER,
	KEYS_REORDERED,
	KEYS_MALFORMED
};


static bool
RunKeyDistribution(KeyCase keyCase)
{
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0)
		return false;
	uint8 encryption[17] = { 6 };
	uint8 identification[11] = { 7, 0x34, 0x12 };
	uint8 identity[17] = { 8 };
	uint8 address[8] = { 9, 1, 7, 8, 9, 10, 11, 0xc0 };
	for (uint8 i = 0; i < 16; i++) {
		encryption[i + 1] = i + 1;
		identity[i + 1] = 0x80 + i;
	}
	for (uint8 i = 0; i < 8; i++)
		identification[i + 3] = 0x20 + i;
	switch (keyCase) {
		case KEYS_IN_ORDER:
			send(sockets[1], encryption, sizeof(encryption), 0);
			send(sockets[1], identification, sizeof(identification), 0);
			send(sockets[1], identity, sizeof(identity), 0);
			send(sockets[1], address, sizeof(address), 0);
			break;
		case KEYS_REORDERED:
		{
			// Identity first, and a stray Security Request in between.
			const uint8 securityRequest[2] = { 0x0b, 0x01 };
			send(sockets[1], identity, sizeof(identity), 0);
			send(sockets[1], address, sizeof(address), 0);
			send(sockets[1], securityRequest, sizeof(securityRequest), 0);
			send(sockets[1], encryption, sizeof(encryption), 0);
			send(sockets[1], identification, sizeof(identification), 0);
			break;
		}
		case KEYS_MALFORMED:
			send(sockets[1], identity, 5, 0);
			break;
	}
	Bluetooth::LELegacyBondKey key = {};
	status_t status = Bluetooth::LELegacyReceiveResponderKeys(sockets[0],
		3, 12, key);
	bool pass;
	if (keyCase == KEYS_MALFORMED) {
		uint8 failed[2] = {};
		pass = status == B_BAD_DATA
			&& recv(sockets[1], failed, sizeof(failed), 0) == 2
			&& failed[0] == 5 && failed[1] == 0x0a
			&& !key.hasLongTermKey && !key.hasIdentity;
	} else {
		uint8 expectedKey[16];
		memcpy(expectedKey, encryption + 1, 16);
		memset(expectedKey + 12, 0, 4);
		pass = status == B_OK && key.hasLongTermKey && key.hasIdentity
			&& key.keySize == 12 && key.encryptedDiversifier == 0x1234
			&& memcmp(key.longTermKey, expectedKey, 16) == 0
			&& memcmp(key.randomNumber, identification + 3, 8) == 0
			&& memcmp(key.identityResolvingKey, identity + 1, 16) == 0
			&& key.identityAddressType == 1
			&& memcmp(key.identityAddress, address + 2, 6) == 0;
	}
	close(sockets[0]);
	close(sockets[1]);
	return pass;
}


int
main()
{
	bool success = RunCase(false);
	bool tamper = RunCase(true);
	bool highDescriptorUsed = false;
	bool highDescriptor = RunCase(false, true, &highDescriptorUsed);
	bool keys = RunKeyDistribution(KEYS_IN_ORDER);
	bool order = RunKeyDistribution(KEYS_REORDERED);
	bool malformed = RunKeyDistribution(KEYS_MALFORMED);
	printf("smp_legacy_exchange=%s smp_confirm_rejection=%s "
		"smp_bond_keys=%s smp_key_reorder=%s smp_key_malformed_rejection=%s "
		"smp_high_fd=%s\n",
		success ? "pass" : "fail", tamper ? "pass" : "fail",
		keys ? "pass" : "fail", order ? "pass" : "fail",
		malformed ? "pass" : "fail",
		!highDescriptor ? "fail" : highDescriptorUsed ? "pass" : "unavailable");
	return success && tamper && highDescriptor && keys && order && malformed
		? 0 : 1;
}
