/*
 * Copyright 2007-2009 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

#include <Entry.h>
#include <Deskbar.h>
#include <Directory.h>
#include <Message.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>
#include <Window.h>

#include <TypeConstants.h>
#include <syslog.h>

#include <bluetoothserver_p.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/bluetooth.h>

#include "BluetoothServer.h"
#include "Debug.h"
#include "DeskbarReplicant.h"
#include "LocalDeviceImpl.h"
#include "SDPServer.h"
#include "SDPClient.h"


status_t
DispatchEvent(struct hci_event_header* header, int32 code, size_t size)
{
	// we only handle events
	if (GET_PORTCODE_TYPE(code)!= BT_EVENT) {
		TRACE_BT("BluetoothServer: Wrong type frame code\n");
		return B_OK;
	}

	// fetch the LocalDevice who belongs this event
	LocalDeviceImpl* lDeviceImplementation = ((BluetoothServer*)be_app)->
		LocateLocalDeviceImpl(GET_PORTCODE_HID(code));

	if (lDeviceImplementation == NULL) {
		TRACE_BT("BluetoothServer: LocalDevice could not be fetched\n");
		return B_OK;
	}

	lDeviceImplementation->HandleEvent(header);

	return B_OK;
}


BluetoothServer::BluetoothServer()
	:
	BApplication(BLUETOOTH_SIGNATURE)
{
	fDeviceManager = new DeviceManager();
	fLocalDevicesList.MakeEmpty();
	fWatchersList.MakeEmpty();

	fEventListener2 = new BluetoothPortListener(BT_USERLAND_PORT_NAME,
		(BluetoothPortListener::port_listener_func)&DispatchEvent);
	fSDPServer = new SDPServer();
}


bool BluetoothServer::QuitRequested(void)
{
	LocalDeviceImpl* lDeviceImpl = NULL;
	while ((lDeviceImpl = (LocalDeviceImpl*)
		fLocalDevicesList.RemoveItemAt(0)) != NULL)
		delete lDeviceImpl;

	_RemoveDeskbarIcon();

	fSDPServer->Stop();
	delete fSDPServer;

	delete fEventListener2;
	TRACE_BT("Shutting down bluetooth_server.\n");

	return BApplication::QuitRequested();
}


void BluetoothServer::ArgvReceived(int32 argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "--finish") == 0)
			PostMessage(B_QUIT_REQUESTED);
	}
}


void BluetoothServer::ReadyToRun(void)
{
	fDeviceManager->StartMonitoringDevice("bluetooth/h2");
	fDeviceManager->StartMonitoringDevice("bluetooth/h3");
	fDeviceManager->StartMonitoringDevice("bluetooth/h4");
	fDeviceManager->StartMonitoringDevice("bluetooth/h5");

	if (fEventListener2->Launch() != B_OK)
		TRACE_BT("General: Bluetooth event listener failed\n");
	else
		TRACE_BT("General: Bluetooth event listener Ready\n");

	_InstallDeskbarIcon();

	status_t status = fSDPServer->Start();
	if (status != B_OK)
		TRACE_BT("BluetoothServer: Failed launching the SDP server thread\n");

	// Drives the HCI command queue and LE watchdogs.
	SetPulseRate(500000);
}


void BluetoothServer::Pulse(void)
{
	for (int32 index = 0; index < fLocalDevicesList.CountItems(); index++) {
		LocalDeviceImpl* device
			= (LocalDeviceImpl*)fLocalDevicesList.ItemAt(index);
		if (device != NULL)
			device->Pulse();
	}
}


void BluetoothServer::AppActivated(bool act)
{
	printf("Activated %d\n",act);
}


void BluetoothServer::MessageReceived(BMessage* message)
{
	BMessage reply;
	status_t status = B_WOULD_BLOCK; // mark somehow to do not reply anything

	switch (message->what) {
		case BT_MSG_ADD_DEVICE:
		{
			BString str;
			message->FindString("name", &str);

			TRACE_BT("BluetoothServer: Requested LocalDevice %s\n", str.String());

			BPath path(str.String());

			LocalDeviceImpl* lDeviceImpl
				= LocalDeviceImpl::CreateTransportAccessor(&path);

			if (lDeviceImpl->GetID() >= 0) {
				fLocalDevicesList.AddItem(lDeviceImpl);

				TRACE_BT("LocalDevice %s id=%" B_PRId32 " added\n", str.String(),
					lDeviceImpl->GetID());
			} else {
				TRACE_BT("BluetoothServer: Adding LocalDevice hci id invalid\n");
			}

			status = B_WOULD_BLOCK;
			/* TODO: This should be by user request only! */
			if (lDeviceImpl->Launch() == B_OK)
				lDeviceImpl->StartPairingSetup();
		}
		break;

		case BT_MSG_REMOVE_DEVICE:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl != NULL) {
				fLocalDevicesList.RemoveItem(lDeviceImpl);
				delete lDeviceImpl;
			}
		}
		break;

		case BT_MSG_COUNT_LOCAL_DEVICES:
			status = HandleLocalDevicesCount(message, &reply);
			break;

		case BT_MSG_ACQUIRE_LOCAL_DEVICE:
			status = HandleAcquireLocalDevice(message, &reply);
			break;

		case BT_MSG_HANDLE_SIMPLE_REQUEST:
			status = HandleSimpleRequest(message, &reply);
			break;

		case BT_MSG_LE_SCAN_START:
		{
			LocalDeviceImpl* device = LocateDelegateFromMessage(message);
			BMessenger listener;
			status = device != NULL
				&& message->FindMessenger("listener", &listener) == B_OK
				? device->StartLEScan(listener) : B_BAD_VALUE;
			break;
		}

		case BT_MSG_LE_SCAN_STOP:
		{
			// With a "listener", only that client's scan interest ends;
			// without one every listener is stopped (legacy behavior).
			LocalDeviceImpl* device = LocateDelegateFromMessage(message);
			BMessenger listener;
			bool hasListener
				= message->FindMessenger("listener", &listener) == B_OK;
			status = device != NULL
				? device->StopLEScan(hasListener ? &listener : NULL)
				: B_BAD_VALUE;
			break;
		}

		case BT_MSG_LE_CONNECT:
		{
			LocalDeviceImpl* device = LocateDelegateFromMessage(message);
			BMessenger listener;
			const void* address = NULL;
			ssize_t addressSize = 0;
			uint8 addressType;
			status = device != NULL
				&& message->FindMessenger("listener", &listener) == B_OK
				&& message->FindData("address", B_RAW_TYPE, &address,
					&addressSize) == B_OK
				&& addressSize == 6
				&& message->FindUInt8("address_type", &addressType) == B_OK
				? device->StartLEConnection(listener, (const uint8*)address,
					addressType) : B_BAD_VALUE;
			break;
		}

		case BT_MSG_LE_CONNECT_CANCEL:
		{
			LocalDeviceImpl* device = LocateDelegateFromMessage(message);
			BMessenger listener;
			bool hasListener
				= message->FindMessenger("listener", &listener) == B_OK;
			status = device != NULL ? device->CancelLEConnection(
				hasListener ? &listener : NULL) : B_BAD_VALUE;
			break;
		}

		case BT_MSG_LE_DISCONNECT:
		{
			// "force" lets the preferences end a link another client owns,
			// for example after forgetting a connected mouse.
			LocalDeviceImpl* device = LocateDelegateFromMessage(message);
			BMessenger listener;
			bool hasListener
				= message->FindMessenger("listener", &listener) == B_OK
					&& !message->GetBool("force", false);
			status = device != NULL ? device->DisconnectLEConnection(
				hasListener ? &listener : NULL) : B_BAD_VALUE;
			break;
		}

		case BT_MSG_LE_START_ENCRYPTION:
		{
			LocalDeviceImpl* device = LocateDelegateFromMessage(message);
			const void* key = NULL;
			ssize_t keySize = 0;
			if (device == NULL) {
				status = B_BAD_VALUE;
				break;
			}
			if (message->FindData("short_term_key", B_RAW_TYPE,
					&key, &keySize) == B_OK) {
				status = keySize == 16
					? device->StartLEEncryption((const uint8*)key, NULL, 0)
					: B_BAD_VALUE;
				break;
			}
			const void* randomNumber = NULL;
			ssize_t randomSize = 0;
			uint16 encryptedDiversifier;
			status = message->FindData("long_term_key", B_RAW_TYPE,
					&key, &keySize) == B_OK && keySize == 16
				&& message->FindData("random_number", B_RAW_TYPE,
					&randomNumber, &randomSize) == B_OK && randomSize == 8
				&& message->FindUInt16("encrypted_diversifier",
					&encryptedDiversifier) == B_OK
				? device->StartLEEncryption((const uint8*)key,
					(const uint8*)randomNumber, encryptedDiversifier)
				: B_BAD_VALUE;
			break;
		}

		case BT_MSG_GET_PROPERTY:
			status = HandleGetProperty(message, &reply);
			break;

		// Handle if the bluetooth preferences is running?
		case B_SOME_APP_LAUNCHED:
		{
			const char* signature;

			if (message->FindString("be:signature", &signature) == B_OK) {
				printf("input_server : %s\n", signature);
				if (strcmp(signature, "application/x-vnd.Be-TSKB") == 0) {

				}
			}
		}
		return;

		case BT_REQ_CREATE_CONN:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			lDeviceImpl->CreateConnection(message);
			break;
		}
		case BT_MSG_PAIR_CONFIRM_RESULT:
		{
			LocalDeviceImpl* device = LocateDelegateFromMessage(message);
			const bdaddr_t* address;
			ssize_t length;
			int32 which = 0;
			if (device != NULL
				&& message->FindData("bdaddr", B_ANY_TYPE,
					(const void**)&address, &length) == B_OK
				&& length == sizeof(bdaddr_t)) {
				message->FindInt32("which", &which);
				device->ConfirmPairing(*address, which == 1);
			}
			break;
		}

		case BT_REQ_CANCEL_CONN:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			lDeviceImpl->CancelConnection(message);
		}
		break;

		case BT_REQ_DISCONNECT:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			lDeviceImpl->Disconnect(message);
		}
		break;

		case BT_REQ_REMOVE_DEVICE:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			lDeviceImpl->Disconnect(message);

			const bdaddr_t* bdaddr;
			ssize_t addr_size;
			message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &addr_size);

			ServerRemoteDevice* rd = lDeviceImpl->RemoteDeviceByAddr(*bdaddr);
			lDeviceImpl->RemoveRemoteDevice(rd);
		}
		break;

		case BT_START_WATCHING_CONNECTIONS:
		{
			BMessenger* messenger = new BMessenger();
			if (message->FindMessenger("messenger", messenger) == B_OK)
				fWatchersList.AddItem(messenger);
		}
		break;

		case BT_STOP_WATCHING_CONNECTIONS:
		{
			BMessenger* messenger = new BMessenger();
			if (message->FindMessenger("messenger", messenger) == B_OK)
				fWatchersList.RemoveItem(messenger);
		}
		break;

		case BT_MSG_GET_REMOTE_DEVICES:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			RemoteDevicesList* rdList = lDeviceImpl->GetRemoteDevicesList();

			for (int32 i = 0; i < rdList->CountItems(); i++) {
				ServerRemoteDevice* rd = rdList->ItemAt(i);
				BMessage device;
				bdaddr_t bdaddr = rd->bdaddr;
				device.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));
				device.AddString("name", rd->friendly_name);
				device.AddData("class_of_device", B_RAW_TYPE, rd->classOfDevice,
					sizeof(rd->classOfDevice));
				device.AddUInt16("clock_offset", rd->clock_offset);
				device.AddUInt8("pscan_rep_mode", rd->pscan_rep_mode);

				reply.AddMessage("remote", &device);
			}

			message->SendReply(&reply);
		}
		break;

		case BT_REQ_CONN_STATE:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			const bdaddr_t* bdaddr;
			ssize_t addr_size;
			ServerRemoteDevice* rd = NULL;
			if (message->FindData("bdaddr", B_ANY_TYPE,
					(const void**)&bdaddr, &addr_size) == B_OK
				&& addr_size == sizeof(bdaddr_t))
				rd = lDeviceImpl->RemoteDeviceByAddr(*bdaddr);
			reply.AddUInt8("conn state", static_cast<uint8>(rd != NULL
				? rd->conn_state : RemoteDevice::DISCONNECTED));

			message->SendReply(&reply);
		}
		break;

		default:
			BApplication::MessageReceived(message);
			break;
	}

	// Can we reply right now?
	// TODO: review this condition
	if (status != B_WOULD_BLOCK) {
		reply.AddInt32("status", status);
		message->SendReply(&reply);
//		printf("Sending reply message for->\n");
//		message->PrintToStream();
	}
}


void
BluetoothServer::NotifyWatchers(BMessage* notice)
{
	for (int32 i = 0; i < fWatchersList.CountItems(); i++) {
		BMessenger* messenger = fWatchersList.ItemAt(i);
		if (!messenger->IsValid()) {
			fWatchersList.RemoveItemAt(i);
			i--;
			continue;
		}
		messenger->SendMessage(notice);
	}
}


void
BluetoothServer::DiscoverServices(ServerRemoteDevice* rd)
{
	TRACE_BT("BluetoothServer: Discovering Services for %s\n", rd->friendly_name.String());
	SDPClient sdpClient(rd);
	sdpClient.Start();
	if (sdpClient.RequestServiceRecords() == B_OK)
		TRACE_BT("SDP: Service records loaded successfully\n");
	else
		TRACE_BT("SDP: Error service records could not be loaded\n");

	// rd->services.PrintToStream();
}


void
BluetoothServer::NotifyServices(ServerRemoteDevice* rd)
{
	TRACE_BT("BluetoothServer: Notifying Services for %s\n", rd->friendly_name.String());
	SDPClient sdpClient(rd);

	if (sdpClient.NotifyProfiles() == B_OK)
		TRACE_BT("SDP: Notified the dedicated profile successfully\n");
	else
		TRACE_BT("SDP: Error could not notify the dedicated profile\n");

}


#if 0
#pragma mark -
#endif

LocalDeviceImpl*
BluetoothServer::LocateDelegateFromMessage(BMessage* message)
{
	hci_id hid;

	if (message->FindInt32("hci_id", &hid) != B_OK)
		return NULL;

	return LocateLocalDeviceImpl(hid);
}


LocalDeviceImpl*
BluetoothServer::LocateLocalDeviceImpl(hci_id hid)
{
	// Try to find out when a ID was specified
	int index;

	for (index = 0; index < fLocalDevicesList.CountItems(); index++) {
		LocalDeviceImpl* lDeviceImpl = fLocalDevicesList.ItemAt(index);
		if (lDeviceImpl->GetID() == hid)
			return lDeviceImpl;
	}

	return NULL;
}


#if 0
#pragma - Messages reply
#endif

status_t
BluetoothServer::HandleLocalDevicesCount(BMessage* message, BMessage* reply)
{
	TRACE_BT("BluetoothServer: count requested\n");

	return reply->AddInt32("count", fLocalDevicesList.CountItems());
}


status_t
BluetoothServer::HandleAcquireLocalDevice(BMessage* message, BMessage* reply)
{
	hci_id hid;
	ssize_t size;
	bdaddr_t bdaddr;
	LocalDeviceImpl* lDeviceImpl = NULL;
	static int32 lastIndex = 0;

	if (message->FindInt32("hci_id", &hid) == B_OK)	{
		TRACE_BT("BluetoothServer: GetLocalDevice requested with id\n");
		lDeviceImpl = LocateDelegateFromMessage(message);

	} else if (message->FindData("bdaddr", B_ANY_TYPE,
		(const void**)&bdaddr, &size) == B_OK) {

		// Try to find out when the user specified the address
		TRACE_BT("BluetoothServer: GetLocalDevice requested with bdaddr\n");
		for (lastIndex = 0; lastIndex < fLocalDevicesList.CountItems();
			lastIndex ++) {
			// TODO: Only possible if the property is available
			// bdaddr_t local;
			// lDeviceImpl = fLocalDevicesList.ItemAt(lastIndex);
			// if ((lDeviceImpl->GetAddress(&local, message) == B_OK)
			// 	&& bacmp(&local, &bdaddr)) {
			// 	break;
			// }
		}

	} else {
		// Careless, any device not performing operations will be fine
		TRACE_BT("BluetoothServer: GetLocalDevice plain request\n");
		// from last assigned till end
		for (int index = lastIndex + 1;
			index < fLocalDevicesList.CountItems();	index++) {
			lDeviceImpl= fLocalDevicesList.ItemAt(index);
			if (lDeviceImpl != NULL && lDeviceImpl->Available()) {
				printf("Requested local device %" B_PRId32 "\n",
					lDeviceImpl->GetID());
				TRACE_BT("BluetoothServer: Device available: %" B_PRId32 "\n", lDeviceImpl->GetID());
				lastIndex = index;
				break;
			}
		}

		// from starting till last assigned if not yet found
		if (lDeviceImpl == NULL) {
			for (int index = 0; index <= lastIndex ; index ++) {
				lDeviceImpl = fLocalDevicesList.ItemAt(index);
				if (lDeviceImpl != NULL && lDeviceImpl->Available()) {
					printf("Requested local device %" B_PRId32 "\n",
						lDeviceImpl->GetID());
					TRACE_BT("BluetoothServer: Device available: %" B_PRId32 "\n", lDeviceImpl->GetID());
					lastIndex = index;
					break;
				}
			}
		}
	}

	if (lastIndex <= fLocalDevicesList.CountItems() && lDeviceImpl != NULL
		&& lDeviceImpl->Available()) {

		hid = lDeviceImpl->GetID();
		lDeviceImpl->Acquire();

		TRACE_BT("BluetoothServer: Device acquired %" B_PRId32 "\n", hid);
		return reply->AddInt32("hci_id", hid);
	}

	return B_ERROR;

}


status_t
BluetoothServer::HandleSimpleRequest(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		return B_ERROR;
	}

	const char* propertyRequested;

	// Find out if there is a property being requested,
	if (message->FindString("property", &propertyRequested) == B_OK) {
		// Check if the property has been already retrieved
		if (lDeviceImpl->IsPropertyAvailable(propertyRequested)) {
			// Dump everything
			reply->AddMessage("properties", lDeviceImpl->GetPropertiesMessage());
			return B_OK;
		}
	}

	// we are gonna need issue the command ...
	if (lDeviceImpl->ProcessSimpleRequest(DetachCurrentMessage()) == B_OK)
		return B_WOULD_BLOCK;
	else {
		lDeviceImpl->Unregister();
		return B_ERROR;
	}

}


status_t
BluetoothServer::HandleGetProperty(BMessage* message, BMessage* reply)
{
	// User side will look for the reply in a result field and will
	// not care about status fields, therefore we return OK in all cases

	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		return B_ERROR;
	}

	const char* propertyRequested;

	// Find out if there is a property being requested,
	if (message->FindString("property", &propertyRequested) == B_OK) {

		TRACE_BT("BluetoothServer: Searching %s property...\n", propertyRequested);

		// Check if the property has been already retrieved
		if (lDeviceImpl->IsPropertyAvailable(propertyRequested)) {

			// 1 bytes requests
			if (strcmp(propertyRequested, "hci_version") == 0
				|| strcmp(propertyRequested, "lmp_version") == 0
				|| strcmp(propertyRequested, "sco_mtu") == 0) {

				uint8 result = lDeviceImpl->GetPropertiesMessage()->
					FindInt8(propertyRequested);
				reply->AddInt32("result", result);

			// 2 bytes requests
			} else if (strcmp(propertyRequested, "hci_revision") == 0
					|| strcmp(propertyRequested, "lmp_subversion") == 0
					|| strcmp(propertyRequested, "manufacturer") == 0
					|| strcmp(propertyRequested, "acl_mtu") == 0
					|| strcmp(propertyRequested, "acl_max_pkt") == 0
					|| strcmp(propertyRequested, "sco_max_pkt") == 0
					|| strcmp(propertyRequested, "packet_type") == 0 ) {

				uint16 result = lDeviceImpl->GetPropertiesMessage()->
					FindInt16(propertyRequested);
				reply->AddInt32("result", result);

			// 1 bit requests
			} else if (strcmp(propertyRequested, "role_switch_capable") == 0
					|| strcmp(propertyRequested, "encrypt_capable") == 0) {

				bool result = lDeviceImpl->GetPropertiesMessage()->
					FindBool(propertyRequested);

				reply->AddInt32("result", result);



			} else {
				TRACE_BT("BluetoothServer: Property %s could not be satisfied\n", propertyRequested);
			}
		}
	}

	return B_OK;
}


#if 0
#pragma mark -
#endif


void
BluetoothServer::ShowWindow(BWindow* pWindow)
{
	pWindow->Lock();
	if (pWindow->IsHidden())
		pWindow->Show();
	else
		pWindow->Activate();
	pWindow->Unlock();
}


void
BluetoothServer::_InstallDeskbarIcon()
{
	app_info appInfo;
	be_app->GetAppInfo(&appInfo);

	BDeskbar deskbar;

	if (deskbar.HasItem(kDeskbarItemName)) {
		_RemoveDeskbarIcon();
	}

	// The BluetoothStatus applet shows devices and batteries; when it is
	// installed, it replaces this simpler replicant.
	entry_ref applet;
	if (be_roster->FindApp("application/x-vnd.Haiku-BluetoothStatus",
			&applet) == B_OK)
		return;

	status_t res = deskbar.AddItem(&appInfo.ref);
	if (res != B_OK)
		TRACE_BT("Failed adding deskbar icon: %" B_PRId32 "\n", res);
}


void
BluetoothServer::_RemoveDeskbarIcon()
{
	BDeskbar deskbar;
	status_t res = deskbar.RemoveItem(kDeskbarItemName);
	if (res != B_OK)
		TRACE_BT("Failed removing Deskbar icon: %" B_PRId32 ": \n", res);
}


#if 0
#pragma mark -
#endif

int
main(int /*argc*/, char** /*argv*/)
{
	BluetoothServer* bluetoothServer = new BluetoothServer;

	bluetoothServer->Run();
	delete bluetoothServer;

	return 0;
}
