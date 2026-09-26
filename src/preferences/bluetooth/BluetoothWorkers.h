/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef BLUETOOTH_WORKERS_H
#define BLUETOOTH_WORKERS_H


// Everything here talks to bluetooth_server from a short-lived thread and
// reports back through a BMessenger, so the calling window never blocks and
// a closed window only makes the final SendMessage fail.


#include <Message.h>
#include <Messenger.h>
#include <String.h>

#include <bluetooth/LocalDevice.h>


// Sends request to the server with bounded timeouts. The result message is
// posted to target with an added "status" (the reply's status, or the
// messaging error).
status_t	SendServerRequestAsync(const BMessage& request,
				const BMessenger& target, const BMessage& result);

// kMsgAdaptersProbed: "generation", and per adapter "device" (pointer to a
// LocalDevice that stays valid for the life of the app), "id", "name",
// "address" (6 raw bytes).
status_t	ProbeAdapters(const BMessenger& target, int32 generation);

// kMsgPairedLoaded: "generation", "classic" messages (address, name,
// class, state), "le" messages (address, address_type, mouse,
// local_address, local_type). hciID < 0 skips the Classic list.
status_t	LoadPairedDevices(const BMessenger& target, int32 generation,
				int32 hciID);

// kMsgNameResult: "key", "name" (only on success).
status_t	LookupRemoteName(const BMessenger& target, int32 hciID,
				const uint8 address[6], uint8 pageRepetitionMode,
				uint16 clockOffset);

// kMsgClassicConnectSent: "key", "status". Sends the Classic create
// connection request that starts pairing; the outcome arrives as
// BT_MSG_CONN_COMPLETED / BT_MSG_CONN_FAILED to connection watchers.
status_t	ConnectClassic(const BMessenger& target, int32 hciID,
				const uint8 address[6], const BString& name, uint32 deviceClass,
				uint8 pageRepetitionMode, uint16 clockOffset);

// Fire-and-forget Classic requests (BT_REQ_DISCONNECT, BT_REQ_REMOVE_DEVICE,
// BT_REQ_CANCEL_CONN) built without a RemoteDevice.
status_t	SendClassicRequest(uint32 what, int32 hciID, const uint8 address[6]);

// Pairs with an LE peer. The kit posts LE_PAIRING_PROGRESS_MESSAGE to
// target. While the server reports B_BUSY the worker retries a few times and
// posts kMsgLEPairBusy ("attempt", "attempts"). Finally kMsgLEPairDone with
// the LEPairingResult fields and "key".
status_t	PairLEDeviceAsync(const BMessenger& target, int32 hciID,
				const uint8 localAddress[6], const uint8 peerAddress[6],
				uint8 peerAddressType);


#endif	// BLUETOOTH_WORKERS_H
