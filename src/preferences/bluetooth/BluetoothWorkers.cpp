/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "BluetoothWorkers.h"

#include <OS.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/RemoteDevice.h>

#include <CommandManager.h>
#include <LEPairingSession.h>
#include <bluetoothserver_p.h>

#include <new>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "DeviceModel.h"
#include "defs.h"


static const bigtime_t kSendTimeout = 2000000;
static const bigtime_t kReplyTimeout = 5000000;
static const bigtime_t kNameReplyTimeout = 15000000;
static const int32 kPairBusyAttempts = 2;
	// PairLEDevice itself retries a busy link for about 15 seconds.
static const bigtime_t kPairBusyDelay = 2000000;


// Runs a job object on its own thread and deletes it afterwards.
class WorkerJob {
public:
	virtual						~WorkerJob() {}
	virtual	void				Run() = 0;

			status_t			Start(const char* name);

private:
	static	int32				_Entry(void* data);
};


status_t
WorkerJob::Start(const char* name)
{
	thread_id thread = spawn_thread(_Entry, name, B_NORMAL_PRIORITY, this);
	if (thread < 0) {
		delete this;
		return thread;
	}
	resume_thread(thread);
	return B_OK;
}


int32
WorkerJob::_Entry(void* data)
{
	WorkerJob* job = (WorkerJob*)data;
	job->Run();
	delete job;
	return 0;
}


static status_t
SendToServer(BMessage& request, BMessage& reply,
	bigtime_t replyTimeout = kReplyTimeout)
{
	BMessenger server(BLUETOOTH_SIGNATURE);
	if (!server.IsValid())
		return B_NAME_NOT_FOUND;
	return server.SendMessage(&request, &reply, kSendTimeout, replyTimeout);
}


static status_t
GetLocalProperty(int32 hciID, const char* property, uint32& value)
{
	BMessage request(BT_MSG_GET_PROPERTY);
	request.AddInt32("hci_id", hciID);
	request.AddString("property", property);
	BMessage reply;
	status_t status = SendToServer(request, reply);
	if (status == B_OK)
		status = reply.FindInt32("result", (int32*)&value);
	return status;
}


//	#pragma mark - generic request


class ServerRequestJob : public WorkerJob {
public:
	ServerRequestJob(const BMessage& request, const BMessenger& target,
		const BMessage& result)
		:
		fRequest(request),
		fTarget(target),
		fResult(result)
	{
	}

	virtual void Run()
	{
		BMessage reply;
		status_t status = SendToServer(fRequest, reply);
		if (status == B_OK) {
			int32 replyStatus;
			if (reply.FindInt32("status", &replyStatus) == B_OK)
				status = replyStatus;
		}
		fResult.AddInt32("status", status);
		fTarget.SendMessage(&fResult);
	}

private:
	BMessage		fRequest;
	BMessenger		fTarget;
	BMessage		fResult;
};


status_t
SendServerRequestAsync(const BMessage& request, const BMessenger& target,
	const BMessage& result)
{
	WorkerJob* job = new(std::nothrow) ServerRequestJob(request, target,
		result);
	if (job == NULL)
		return B_NO_MEMORY;
	return job->Start("Bluetooth server request");
}


//	#pragma mark - adapters


class ProbeAdaptersJob : public WorkerJob {
public:
	ProbeAdaptersJob(const BMessenger& target, int32 generation)
		:
		fTarget(target),
		fGeneration(generation)
	{
	}

	virtual void Run()
	{
		BMessage result(kMsgAdaptersProbed);
		result.AddInt32("generation", fGeneration);

		// LocalDevice objects cannot be deleted by applications; the few
		// created here live as long as the app.
		std::vector<int32> seen;
		uint32 count = LocalDevice::GetLocalDeviceCount();
		for (uint32 i = 0; i < count; i++) {
			LocalDevice* device = LocalDevice::GetLocalDevice();
			if (device == NULL)
				continue;
			bool duplicate = false;
			for (size_t j = 0; j < seen.size(); j++)
				duplicate |= seen[j] == device->ID();
			if (duplicate)
				continue;
			seen.push_back(device->ID());

			bdaddr_t address = device->GetBluetoothAddress();
			result.AddPointer("device", device);
			result.AddInt32("id", device->ID());
			result.AddString("name", device->GetFriendlyName());
			result.AddData("address", B_RAW_TYPE, address.b, 6);
		}
		fTarget.SendMessage(&result);
	}

private:
	BMessenger		fTarget;
	int32			fGeneration;
};


status_t
ProbeAdapters(const BMessenger& target, int32 generation)
{
	WorkerJob* job = new(std::nothrow) ProbeAdaptersJob(target, generation);
	if (job == NULL)
		return B_NO_MEMORY;
	return job->Start("Bluetooth adapter probe");
}


//	#pragma mark - paired devices


class LoadPairedJob : public WorkerJob {
public:
	LoadPairedJob(const BMessenger& target, int32 generation, int32 hciID)
		:
		fTarget(target),
		fGeneration(generation),
		fHCIID(hciID)
	{
	}

	virtual void Run()
	{
		BMessage result(kMsgPairedLoaded);
		result.AddInt32("generation", fGeneration);

		if (fHCIID >= 0)
			_LoadClassic(result);

		std::vector<LEBondedPeer> peers;
		status_t status = ListLEBondedPeers(peers);
		result.AddInt32("le_status", status);
		for (size_t i = 0; i < peers.size(); i++) {
			BMessage device;
			device.AddData("address", B_RAW_TYPE, peers[i].peerAddress, 6);
			device.AddUInt8("address_type", peers[i].peerAddressType);
			device.AddBool("mouse", peers[i].mouse);
			device.AddData("local_address", B_RAW_TYPE,
				peers[i].localAddress, 6);
			device.AddUInt8("local_type", peers[i].localAddressType);
			result.AddMessage("le", &device);
		}

		fTarget.SendMessage(&result);
	}

private:
	void _LoadClassic(BMessage& result)
	{
		BMessage request(BT_MSG_GET_REMOTE_DEVICES);
		request.AddInt32("hci_id", fHCIID);
		BMessage devices;
		status_t status = SendToServer(request, devices);
		result.AddInt32("classic_status", status);
		if (status != B_OK)
			return;

		BMessage remote;
		for (int32 i = 0; devices.FindMessage("remote", i, &remote) == B_OK;
				i++) {
			const bdaddr_t* address;
			const uint8* record;
			ssize_t size;
			if (remote.FindData("bdaddr", B_ANY_TYPE, (const void**)&address,
					&size) != B_OK || size != sizeof(bdaddr_t)
				|| remote.FindData("class_of_device", B_ANY_TYPE,
					(const void**)&record, &size) != B_OK || size != 3)
				continue;

			BMessage stateRequest(BT_REQ_CONN_STATE);
			stateRequest.AddInt32("hci_id", fHCIID);
			stateRequest.AddData("bdaddr", B_ANY_TYPE, address,
				sizeof(bdaddr_t));
			BMessage stateReply;
			uint8 state = Bluetooth::RemoteDevice::DISCONNECTED;
			if (SendToServer(stateRequest, stateReply) == B_OK)
				stateReply.FindUInt8("conn state", &state);

			BMessage device;
			device.AddData("address", B_RAW_TYPE, address->b, 6);
			device.AddString("name", remote.GetString("name", ""));
			device.AddUInt32("class",
				record[0] | (record[1] << 8) | (record[2] << 16));
			device.AddInt32("state", state);
			device.AddUInt8("page_repetition_mode",
				remote.GetUInt8("pscan_rep_mode", 0));
			device.AddUInt16("clock_offset", remote.GetUInt16("clock_offset", 0));
			result.AddMessage("classic", &device);
		}
	}

	BMessenger		fTarget;
	int32			fGeneration;
	int32			fHCIID;
};


status_t
LoadPairedDevices(const BMessenger& target, int32 generation, int32 hciID)
{
	WorkerJob* job = new(std::nothrow) LoadPairedJob(target, generation,
		hciID);
	if (job == NULL)
		return B_NO_MEMORY;
	return job->Start("Bluetooth paired devices");
}


//	#pragma mark - remote name


class NameLookupJob : public WorkerJob {
public:
	NameLookupJob(const BMessenger& target, int32 hciID,
		const uint8 address[6], uint8 pageRepetitionMode, uint16 clockOffset)
		:
		fTarget(target),
		fHCIID(hciID),
		fPageRepetitionMode(pageRepetitionMode),
		fClockOffset(clockOffset)
	{
		memcpy(fAddress, address, 6);
	}

	virtual void Run()
	{
		BMessage result(kMsgNameResult);
		result.AddUInt64("key", KeyForAddress(fAddress));

		bdaddr_t address;
		memcpy(address.b, fAddress, 6);
		size_t size;
		void* command = buildRemoteNameRequest(address, fPageRepetitionMode,
			fClockOffset, &size);
		if (command != NULL) {
			BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
			request.AddInt32("hci_id", fHCIID);
			request.AddData("raw command", B_ANY_TYPE, command, size);
			request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
			request.AddInt16("opcodeExpected",
				PACK_OPCODE(OGF_LINK_CONTROL, OCF_REMOTE_NAME_REQUEST));
			request.AddInt16("eventExpected",
				HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE);
			free(command);

			BMessage reply;
			uint8 status;
			BString name;
			if (SendToServer(request, reply, kNameReplyTimeout) == B_OK
				&& reply.FindUInt8("status", &status) == B_OK
				&& status == BT_OK
				&& reply.FindString("friendlyname", &name) == B_OK)
				result.AddString("name", name);
		}
		fTarget.SendMessage(&result);
	}

private:
	BMessenger		fTarget;
	int32			fHCIID;
	uint8			fAddress[6];
	uint8			fPageRepetitionMode;
	uint16			fClockOffset;
};


status_t
LookupRemoteName(const BMessenger& target, int32 hciID,
	const uint8 address[6], uint8 pageRepetitionMode, uint16 clockOffset)
{
	WorkerJob* job = new(std::nothrow) NameLookupJob(target, hciID, address,
		pageRepetitionMode, clockOffset);
	if (job == NULL)
		return B_NO_MEMORY;
	return job->Start("Bluetooth remote name");
}


//	#pragma mark - Classic connection


class ClassicConnectJob : public WorkerJob {
public:
	ClassicConnectJob(const BMessenger& target, int32 hciID,
		const uint8 address[6], const BString& name, uint32 deviceClass,
		uint8 pageRepetitionMode, uint16 clockOffset)
		:
		fTarget(target),
		fHCIID(hciID),
		fName(name),
		fDeviceClass(deviceClass),
		fPageRepetitionMode(pageRepetitionMode),
		fClockOffset(clockOffset)
	{
		memcpy(fAddress, address, 6);
	}

	virtual void Run()
	{
		// Mirrors RemoteDevice::Connect(), but passes the best known name
		// so the server stores it with the paired device.
		uint32 packetType = 0;
		uint32 roleSwitch = 0;
		GetLocalProperty(fHCIID, "packet_type", packetType);
		GetLocalProperty(fHCIID, "role_switch_capable", roleSwitch);

		bdaddr_t address;
		memcpy(address.b, fAddress, 6);

		BMessage request(BT_REQ_CREATE_CONN);
		request.AddInt32("hci_id", fHCIID);
		request.AddData("bdaddr", B_ANY_TYPE, &address, sizeof(bdaddr_t));
		request.AddString("name", fName);
		request.AddUInt32("record", fDeviceClass);
		request.AddUInt16("packet type", (uint16)packetType);
		request.AddUInt8("pscan_rep_mode", fPageRepetitionMode);
		request.AddUInt8("pscan_mode", 0);
		request.AddUInt16("clock_offset", fClockOffset);
		request.AddUInt8("role_switch", (uint8)roleSwitch);

		BMessenger server(BLUETOOTH_SIGNATURE);
		status_t status = server.IsValid()
			? server.SendMessage(&request, (BHandler*)NULL, kSendTimeout)
			: B_NAME_NOT_FOUND;

		BMessage result(kMsgClassicConnectSent);
		result.AddUInt64("key", KeyForAddress(fAddress));
		result.AddInt32("status", status);
		fTarget.SendMessage(&result);
	}

private:
	BMessenger		fTarget;
	int32			fHCIID;
	uint8			fAddress[6];
	BString			fName;
	uint32			fDeviceClass;
	uint8			fPageRepetitionMode;
	uint16			fClockOffset;
};


status_t
ConnectClassic(const BMessenger& target, int32 hciID, const uint8 address[6],
	const BString& name, uint32 deviceClass, uint8 pageRepetitionMode,
	uint16 clockOffset)
{
	WorkerJob* job = new(std::nothrow) ClassicConnectJob(target, hciID,
		address, name, deviceClass, pageRepetitionMode, clockOffset);
	if (job == NULL)
		return B_NO_MEMORY;
	return job->Start("Bluetooth Classic connect");
}


status_t
SendClassicRequest(uint32 what, int32 hciID, const uint8 address[6])
{
	BMessenger server(BLUETOOTH_SIGNATURE);
	if (!server.IsValid())
		return B_NAME_NOT_FOUND;

	bdaddr_t bdaddr;
	memcpy(bdaddr.b, address, 6);
	BMessage request(what);
	request.AddInt32("hci_id", hciID);
	request.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));
	if (what != BT_REQ_CANCEL_CONN)
		request.AddUInt8("reason", BT_REMOTE_USER_ENDED_CONNECTION);
	return server.SendMessage(&request, (BHandler*)NULL, kSendTimeout);
}


//	#pragma mark - LE pairing


class PairLEJob : public WorkerJob {
public:
	PairLEJob(const BMessenger& target, int32 hciID,
		const uint8 localAddress[6], const uint8 peerAddress[6],
		uint8 peerAddressType)
		:
		fTarget(target),
		fHCIID(hciID),
		fPeerAddressType(peerAddressType)
	{
		memcpy(fLocalAddress, localAddress, 6);
		memcpy(fPeerAddress, peerAddress, 6);
	}

	virtual void Run()
	{
		Bluetooth::LEPairingResult result;
		for (int32 attempt = 1; ; attempt++) {
			result = Bluetooth::PairLEDevice(fHCIID, fLocalAddress,
				fPeerAddress, fPeerAddressType, &fTarget);
			// The server runs one LE operation at a time; the input_server
			// mouse add-on may be scanning for or connecting to a bonded
			// mouse right now.
			if (result.status != B_BUSY || attempt >= kPairBusyAttempts
				|| !fTarget.IsValid())
				break;
			BMessage busy(kMsgLEPairBusy);
			busy.AddInt32("attempt", attempt + 1);
			busy.AddInt32("attempts", kPairBusyAttempts);
			fTarget.SendMessage(&busy);
			snooze(kPairBusyDelay);
		}

		BMessage done(kMsgLEPairDone);
		done.AddUInt64("key", KeyForAddress(fPeerAddress));
		done.AddInt32("status", result.status);
		done.AddInt32("stage", result.stage);
		done.AddBool("encrypted", result.encrypted);
		done.AddBool("bonded", result.bonded);
		done.AddBool("reused_bond", result.reusedBond);
		done.AddBool("hid_mouse_ready", result.hidMouseReady);
		done.AddInt32("hid_status", result.hidStatus);
		result.detail[sizeof(result.detail) - 1] = '\0';
		done.AddString("detail", result.detail);
		fTarget.SendMessage(&done);
	}

private:
	BMessenger		fTarget;
	int32			fHCIID;
	uint8			fLocalAddress[6];
	uint8			fPeerAddress[6];
	uint8			fPeerAddressType;
};


status_t
PairLEDeviceAsync(const BMessenger& target, int32 hciID,
	const uint8 localAddress[6], const uint8 peerAddress[6],
	uint8 peerAddressType)
{
	WorkerJob* job = new(std::nothrow) PairLEJob(target, hciID, localAddress,
		peerAddress, peerAddressType);
	if (job == NULL)
		return B_NO_MEMORY;
	return job->Start("Bluetooth LE pairing");
}
