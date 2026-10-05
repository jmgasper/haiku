/* Discover the HID service on an explicitly selected LE peer. MIT License. */
#include <LEAttributeClient.h>
#include <bluetooth/LocalDevice.h>
#include <bluetoothserver_p.h>

#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <stdio.h>
#include <string.h>


class LinkListener : public BLooper {
public:
	LinkListener()
		:
		BLooper("LE ATT probe listener"),
		fReady(create_sem(0, "LE ATT probe ready")),
		fEvent(0),
		fStatus(B_ERROR)
	{
	}

	~LinkListener() { delete_sem(fReady); }
	void MessageReceived(BMessage* message) override
	{
		if (message->what == BT_MSG_LE_CONNECTING
			|| message->what == BT_MSG_LE_CONNECTED
			|| message->what == BT_MSG_LE_CONNECT_FAILED
			|| message->what == BT_MSG_LE_DISCONNECTED) {
			fEvent = message->what;
			message->FindInt32("status", &fStatus);
			release_sem(fReady);
		} else
			BLooper::MessageReceived(message);
	}
	status_t Wait() { return acquire_sem_etc(fReady, 1, B_RELATIVE_TIMEOUT,
		25 * 1000000); }
	uint32 Event() const { return fEvent; }
	status_t Status() const { return fStatus; }
private:
	sem_id fReady;
	uint32 fEvent;
	status_t fStatus;
};


static status_t
SendRequest(BMessenger& server, uint32 what, int32 hciID)
{
	BMessage request(what), reply;
	request.AddInt32("hci_id", hciID);
	status_t status = server.SendMessage(&request, &reply,
		5 * 1000000, 5 * 1000000);
	int32 operationStatus = B_ERROR;
	if (status == B_OK && reply.FindInt32("status", &operationStatus) == B_OK)
		return operationStatus;
	return status == B_OK ? B_BAD_DATA : status;
}


static bool
ParseAddress(const char* text, bdaddr_t& address)
{
	unsigned int byte[6];
	int used = 0;
	if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%n", &byte[5], &byte[4],
			&byte[3], &byte[2], &byte[1], &byte[0], &used) != 6
		|| text[used] != '\0')
		return false;
	for (int i = 0; i < 6; i++)
		address.b[i] = byte[i];
	return true;
}


int
main(int argc, char** argv)
{
	if (argc != 3 || (strcmp(argv[2], "public") != 0
		&& strcmp(argv[2], "random") != 0)) {
		fprintf(stderr, "usage: %s XX:XX:XX:XX:XX:XX public|random\n",
			argv[0]);
		return 2;
	}
	bdaddr_t address;
	if (!ParseAddress(argv[1], address)) {
		fprintf(stderr, "invalid Bluetooth address\n");
		return 2;
	}
	Bluetooth::LocalDevice* device = Bluetooth::LocalDevice::GetLocalDevice();
	if (device == NULL) {
		fprintf(stderr, "no Bluetooth controller\n");
		return 1;
	}
	BMessenger server(BLUETOOTH_SIGNATURE);
	if (!server.IsValid()) {
		fprintf(stderr, "bluetooth_server is unavailable\n");
		return 1;
	}
	LinkListener* listener = new LinkListener();
	listener->Run();
	BMessage request(BT_MSG_LE_CONNECT), reply;
	request.AddInt32("hci_id", device->ID());
	request.AddMessenger("listener", BMessenger(listener));
	request.AddData("address", B_RAW_TYPE, address.b, sizeof(address.b));
	request.AddUInt8("address_type", strcmp(argv[2], "random") == 0 ? 1 : 0);
	status_t result = server.SendMessage(&request, &reply,
		5 * 1000000, 5 * 1000000);
	int32 operationStatus = B_ERROR;
	if (result == B_OK && reply.FindInt32("status", &operationStatus) == B_OK)
		result = operationStatus;
	else if (result == B_OK)
		result = B_BAD_DATA;
	if (result != B_OK) {
		fprintf(stderr, "LE connection request failed: %ld\n", (long)result);
		if (listener->Lock())
			listener->Quit();
		return 1;
	}

	bool connected = false;
	for (int i = 0; i < 2; i++) {
		result = listener->Wait();
		if (result != B_OK)
			break;
		if (listener->Event() == BT_MSG_LE_CONNECTED) {
			connected = true;
			break;
		}
		if (listener->Event() == BT_MSG_LE_CONNECT_FAILED
			|| listener->Event() == BT_MSG_LE_DISCONNECTED) {
			result = listener->Status();
			break;
		}
	}
	if (!connected) {
		SendRequest(server, BT_MSG_LE_CONNECT_CANCEL, device->ID());
		fprintf(stderr, "LE link unavailable: %ld\n", (long)result);
		if (listener->Lock())
			listener->Quit();
		return 1;
	}

	Bluetooth::LEAttributeClient client;
	result = client.Connect(address);
	if (result == B_OK) {
		std::vector<Bluetooth::LEPrimaryService> services;
		result = client.DiscoverPrimaryServices(services);
		printf("service_discovery=%ld count=%zu att_error=%#x\n",
			(long)result, services.size(), client.LastATTError());
		for (const auto& service : services) {
			printf("service=%#04x handles=%#04x-%#04x\n", service.uuid16,
				service.startHandle, service.endHandle);
			if (service.uuid16 != 0x1812 || result != B_OK)
				continue;
			std::vector<Bluetooth::LECharacteristic> characteristics;
			result = client.DiscoverCharacteristics(service.startHandle,
				service.endHandle, characteristics);
			printf("hid_characteristics=%ld count=%zu att_error=%#x\n",
				(long)result, characteristics.size(), client.LastATTError());
			for (const auto& characteristic : characteristics)
				printf("characteristic=%#04x value=%#04x properties=%#x\n",
					characteristic.uuid16, characteristic.valueHandle,
					characteristic.properties);
		}
	} else
		fprintf(stderr, "ATT socket failed: %ld\n", (long)result);
	client.Disconnect();
	SendRequest(server, BT_MSG_LE_DISCONNECT, device->ID());
	if (listener->Lock())
		listener->Quit();
	return result == B_OK ? 0 : 1;
}
