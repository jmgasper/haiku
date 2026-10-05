/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * bt_le: command line access to Bluetooth Low Energy scanning, pairing and
 * the stored bonds, mainly for diagnosing pairing problems.
 */

#include <Application.h>
#include <Autolock.h>
#include <Locker.h>
#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>

#include <bluetooth/LocalDevice.h>

#include <LEAttributeClient.h>
#include <LEBondStore.h>
#include <LELog.h>
#include <LEPairingSession.h>
#include <bluetoothserver_p.h>

#include <algorithm>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>


using namespace Bluetooth;


static void
Usage()
{
	fprintf(stderr,
		"usage: bt_le scan [seconds]\n"
		"       bt_le pair <address> [public|random]\n"
		"       bt_le services <address> [public|random]\n"
		"       bt_le list\n"
		"       bt_le forget <address> [public|random]\n"
		"       bt_le log [0-3]\n"
		"Addresses are written as AA:BB:CC:DD:EE:FF. Pair a device while it\n"
		"is in pairing mode; the log is %s.\n", LELogFilePath());
}


static bool
ParseAddress(const char* text, uint8 address[6])
{
	unsigned int bytes[6];
	if (sscanf(text, "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2],
			&bytes[3], &bytes[4], &bytes[5]) != 6)
		return false;
	// Text is most significant first; the wire order is the reverse.
	for (int i = 0; i < 6; i++) {
		if (bytes[i] > 0xff)
			return false;
		address[5 - i] = bytes[i];
	}
	return true;
}


static LocalDevice*
GetAdapter(uint8 localAddress[6])
{
	LocalDevice* local = LocalDevice::GetLocalDevice();
	if (local == NULL) {
		fprintf(stderr, "No Bluetooth adapter (is bluetooth_server running?)\n");
		return NULL;
	}
	bdaddr_t address = local->GetBluetoothAddress();
	memcpy(localAddress, address.b, 6);
	return local;
}


struct Advertiser {
	uint8 address[6];
	uint8 type;
	int8 rssi;
	bool connectable;
	std::string name;
	uint16 appearance;
	bool hid;
	uint32 reports;
};


class ScanListener : public BLooper {
public:
	ScanListener()
		:
		BLooper("bt_le scan"),
		fStarted(false),
		fError(false)
	{
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case BT_MSG_LE_SCAN_STARTED:
				fStarted = true;
				break;
			case BT_MSG_LE_SCAN_ERROR:
				fError = true;
				break;
			case BT_MSG_LE_ADVERTISEMENT:
				_Add(message);
				break;
			default:
				BLooper::MessageReceived(message);
		}
	}

	std::vector<Advertiser> Results()
	{
		BAutolock lock(this);
		std::vector<Advertiser> results;
		for (auto& entry : fDevices)
			results.push_back(entry.second);
		return results;
	}

	bool fStarted;
	bool fError;

private:
	void _Add(BMessage* message)
	{
		const uint8* address;
		const uint8* data;
		ssize_t addressLength, dataLength = 0;
		uint8 type = 0, eventType = 0;
		int8 rssi = 0;
		if (message->FindData("address", B_RAW_TYPE, (const void**)&address,
				&addressLength) != B_OK || addressLength != 6)
			return;
		message->FindUInt8("address_type", &type);
		message->FindUInt8("event_type", &eventType);
		message->FindInt8("rssi", &rssi);
		if (message->FindData("data", B_RAW_TYPE, (const void**)&data,
				&dataLength) != B_OK)
			dataLength = 0;

		std::string key((const char*)address, 6);
		key += (char)type;
		Advertiser& device = fDevices[key];
		if (device.reports == 0) {
			memcpy(device.address, address, 6);
			device.type = type;
			device.connectable = false;
			device.appearance = 0;
			device.hid = false;
		}
		device.reports++;
		device.rssi = rssi;
		if (eventType == 0 || eventType == 1)
			device.connectable = true;

		for (ssize_t offset = 0; offset + 1 < dataLength;) {
			uint8 length = data[offset];
			if (length == 0 || offset + 1 + length > dataLength)
				break;
			uint8 field = data[offset + 1];
			const uint8* value = data + offset + 2;
			size_t valueLength = length - 1;
			if (field == 0x09 || (field == 0x08 && device.name.empty()))
				device.name.assign((const char*)value, valueLength);
			else if (field == 0x19 && valueLength >= 2)
				device.appearance = value[0] | (value[1] << 8);
			else if (field == 0x02 || field == 0x03) {
				for (size_t i = 0; i + 1 < valueLength; i += 2) {
					if ((value[i] | (value[i + 1] << 8)) == 0x1812)
						device.hid = true;
				}
			}
			offset += 1 + length;
		}
	}

	std::map<std::string, Advertiser> fDevices;
};


static int
Scan(int seconds)
{
	uint8 local[6];
	LocalDevice* adapter = GetAdapter(local);
	if (adapter == NULL)
		return 1;
	BMessenger server(BLUETOOTH_SIGNATURE);
	ScanListener* listener = new ScanListener();
	listener->Run();
	BMessenger listenerMessenger(listener);

	BMessage request(BT_MSG_LE_SCAN_START), reply;
	request.AddInt32("hci_id", adapter->ID());
	request.AddMessenger("listener", listenerMessenger);
	status_t status = server.SendMessage(&request, &reply, 5000000, 5000000);
	int32 result = B_ERROR;
	if (status == B_OK)
		reply.FindInt32("status", &result);
	if (status != B_OK || result != B_OK) {
		fprintf(stderr, "Could not start the LE scan: %s\n",
			strerror(status != B_OK ? status : result));
		return 1;
	}
	printf("Scanning for %d seconds...\n", seconds);
	snooze((bigtime_t)seconds * 1000000);

	BMessage stop(BT_MSG_LE_SCAN_STOP);
	stop.AddInt32("hci_id", adapter->ID());
	stop.AddMessenger("listener", listenerMessenger);
	server.SendMessage(&stop, &reply, 5000000, 5000000);
	snooze(200000);

	std::vector<Advertiser> results = listener->Results();
	if (listener->Lock())
		listener->Quit();
	std::sort(results.begin(), results.end(),
		[](const Advertiser& a, const Advertiser& b) { return a.rssi > b.rssi; });
	printf("%-17s %-6s %5s  %-4s %-8s %s\n", "address", "type", "rssi", "conn",
		"hint", "name");
	for (const Advertiser& device : results) {
		char text[18];
		const char* hint = "";
		if (device.appearance == 0x03c2)
			hint = "mouse";
		else if (device.appearance == 0x03c1)
			hint = "keyboard";
		else if (device.hid)
			hint = "HID";
		printf("%-17s %-6s %5d  %-4s %-8s %s\n",
			LEAddressString(device.address, text),
			device.type == 0 ? "public" : "random", device.rssi,
			device.connectable ? "yes" : "no", hint, device.name.c_str());
	}
	if (listener->fError)
		fprintf(stderr, "The scan reported an error; see the log.\n");
	return 0;
}


class ProgressPrinter : public BLooper {
public:
	ProgressPrinter() : BLooper("bt_le progress") {}

	void MessageReceived(BMessage* message) override
	{
		if (message->what != LE_PAIRING_PROGRESS_MESSAGE) {
			BLooper::MessageReceived(message);
			return;
		}
		static const char* kStages[] = { "connect", "bond lookup",
			"pairing", "security channel", "pairing request",
			"pairing response", "confirm", "random", "encryption",
			"key distribution", "saving bond", "HID discovery", "complete",
			"cleanup" };
		int32 stage = 0;
		message->FindInt32("stage", &stage);
		const char* detail = message->FindString("detail");
		printf("  [%s]%s%s\n", stage >= 0 && stage < (int32)B_COUNT_OF(kStages)
			? kStages[stage] : "?", detail != NULL ? " " : "",
			detail != NULL ? detail : "");
		fflush(stdout);
	}
};


static int
Pair(const uint8 peer[6], uint8 type)
{
	uint8 local[6];
	LocalDevice* adapter = GetAdapter(local);
	if (adapter == NULL)
		return 1;
	char text[18];
	printf("Pairing %s (%s address). Keep the device in pairing mode.\n",
		LEAddressString(peer, text), type == 0 ? "public" : "random");
	ProgressPrinter* printer = new ProgressPrinter();
	printer->Run();
	BMessenger progress(printer);
	LEPairingResult result = PairLEDevice(adapter->ID(), local, peer, type,
		&progress);
	snooze(100000);
	if (printer->Lock())
		printer->Quit();
	if (result.status != B_OK) {
		printf("Pairing failed: %s\n", result.detail[0] != '\0'
			? result.detail : strerror(result.status));
		printf("Details: %s\n", LELogFilePath());
		return 1;
	}
	printf("Paired%s. Encrypted: %s, bonded: %s.\n",
		result.hidMouseReady ? " as a mouse" : "",
		result.encrypted ? "yes" : "no", result.bonded ? "yes" : "no");
	if (!result.hidMouseReady && result.detail[0] != '\0')
		printf("Note: %s\n", result.detail);
	return 0;
}


class LinkListener : public BLooper {
public:
	LinkListener()
		:
		BLooper("bt_le link"),
		fSemaphore(create_sem(0, "bt_le link"))
	{
	}

	~LinkListener() override
	{
		delete_sem(fSemaphore);
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case BT_MSG_LE_CONNECTED:
			case BT_MSG_LE_CONNECT_FAILED:
			case BT_MSG_LE_DISCONNECTED:
			{
				int32 status = B_ERROR;
				message->FindInt32("status", &status);
				fLast = message->what;
				fStatus = status;
				release_sem(fSemaphore);
				break;
			}
			default:
				BLooper::MessageReceived(message);
		}
	}

	status_t Wait(bigtime_t timeout)
	{
		status_t status = acquire_sem_etc(fSemaphore, 1, B_RELATIVE_TIMEOUT,
			timeout);
		if (status != B_OK)
			return status;
		return fLast == BT_MSG_LE_CONNECTED ? B_OK
			: (fStatus != B_OK ? fStatus : B_ERROR);
	}

private:
	sem_id fSemaphore;
	uint32 fLast;
	int32 fStatus;
};


static int
Services(const uint8 peer[6], uint8 type)
{
	uint8 local[6];
	LocalDevice* adapter = GetAdapter(local);
	if (adapter == NULL)
		return 1;
	BMessenger server(BLUETOOTH_SIGNATURE);
	LinkListener* listener = new LinkListener();
	listener->Run();
	BMessenger listenerMessenger(listener);
	char text[18];
	printf("Connecting to %s (%s, no pairing)...\n", LEAddressString(peer, text),
		type == 0 ? "public" : "random");

	BMessage request(BT_MSG_LE_CONNECT), reply;
	request.AddInt32("hci_id", adapter->ID());
	request.AddMessenger("listener", listenerMessenger);
	request.AddData("address", B_RAW_TYPE, peer, 6);
	request.AddUInt8("address_type", type);
	int32 result = B_ERROR;
	if (server.SendMessage(&request, &reply, 5000000, 5000000) == B_OK)
		reply.FindInt32("status", &result);
	if (result != B_OK) {
		fprintf(stderr, "Connection request refused: %s\n", strerror(result));
		return 1;
	}
	status_t status = listener->Wait(20000000);
	int exitCode = 1;
	if (status != B_OK) {
		fprintf(stderr, "Not connected: %s\n", strerror(status));
		BMessage cancel(BT_MSG_LE_CONNECT_CANCEL);
		cancel.AddInt32("hci_id", adapter->ID());
		cancel.AddMessenger("listener", listenerMessenger);
		server.SendMessage(&cancel, &reply, 5000000, 5000000);
		snooze(1000000);
	} else {
		printf("Connected.\n");
		bdaddr_t address;
		memcpy(address.b, peer, 6);
		LEAttributeClient client;
		status = client.Connect(address);
		std::vector<LEPrimaryService> services;
		if (status == B_OK)
			status = client.DiscoverPrimaryServices(services);
		if (status != B_OK)
			fprintf(stderr, "Service discovery failed: %s\n", strerror(status));
		else {
			for (const LEPrimaryService& service : services) {
				printf("  service %#06x handles %#06x-%#06x\n", service.uuid16,
					service.startHandle, service.endHandle);
			}
			printf("%zu primary services.\n", services.size());
			exitCode = 0;
		}
		client.Disconnect();
		BMessage disconnect(BT_MSG_LE_DISCONNECT);
		disconnect.AddInt32("hci_id", adapter->ID());
		disconnect.AddMessenger("listener", listenerMessenger);
		server.SendMessage(&disconnect, &reply, 5000000, 5000000);
		listener->Wait(3000000);
	}
	if (listener->Lock())
		listener->Quit();
	return exitCode;
}


static int
List()
{
	char directory[1024];
	if (DefaultLEBondDirectory(directory, sizeof(directory)) != B_OK)
		return 1;
	std::vector<LEHIDMouseDevice> mice;
	status_t status = ListLEHIDMice(directory, mice);
	if (status != B_OK && status != B_ENTRY_NOT_FOUND) {
		fprintf(stderr, "Cannot read %s: %s\n", directory, strerror(status));
		return 1;
	}
	if (mice.empty())
		printf("No paired Low Energy mice.\n");
	for (const LEHIDMouseDevice& mouse : mice) {
		char peer[18], local[18];
		printf("%s (%s) on adapter %s\n", LEAddressString(mouse.peerAddress,
			peer), mouse.peerAddressType == 0 ? "public" : "random",
			LEAddressString(mouse.localAddress, local));
	}
	return 0;
}


static int
Forget(const uint8 peer[6], uint8 type)
{
	uint8 local[6];
	LocalDevice* adapter = GetAdapter(local);
	if (adapter == NULL)
		return 1;
	char directory[1024];
	if (DefaultLEBondDirectory(directory, sizeof(directory)) != B_OK)
		return 1;
	status_t status = RemoveLEBond(directory, local, 0, peer, type);
	char text[18];
	if (status != B_OK) {
		fprintf(stderr, "Could not forget %s: %s\n", LEAddressString(peer, text),
			strerror(status));
		return 1;
	}
	printf("Forgot %s.\n", LEAddressString(peer, text));
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		Usage();
		return 1;
	}
	BApplication application("application/x-vnd.Haiku-bt_le");
	const char* command = argv[1];
	if (strcmp(command, "scan") == 0)
		return Scan(argc > 2 ? atoi(argv[2]) : 10);
	if (strcmp(command, "list") == 0)
		return List();
	if (strcmp(command, "log") == 0) {
		if (argc > 2) {
			status_t status = LESetLogLevel(atoi(argv[2]));
			if (status != B_OK) {
				fprintf(stderr, "Cannot set the level: %s\n", strerror(status));
				return 1;
			}
		}
		printf("level %" B_PRId32 " (0 errors, 1 steps, 2 debug + kernel, "
			"3 packets)\nlog: %s\n", LELogLevel(), LELogFilePath());
		return 0;
	}
	if ((strcmp(command, "pair") == 0 || strcmp(command, "forget") == 0
			|| strcmp(command, "services") == 0)
		&& argc > 2) {
		uint8 address[6];
		if (!ParseAddress(argv[2], address)) {
			Usage();
			return 1;
		}
		// Most mice use a random static address (top two bits set).
		uint8 type = (address[5] & 0xc0) == 0xc0 ? 1 : 0;
		if (argc > 3)
			type = strcmp(argv[3], "public") == 0 ? 0 : 1;
		if (command[0] == 's')
			return Services(address, type);
		return command[0] == 'p' ? Pair(address, type) : Forget(address, type);
	}
	Usage();
	return 1;
}
