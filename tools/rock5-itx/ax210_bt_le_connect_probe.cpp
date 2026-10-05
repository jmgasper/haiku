/* Exercise LE connection setup/cancellation against a nonexistent random peer. */
#include <bluetooth/LocalDevice.h>
#include <bluetoothserver_p.h>

#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <stdio.h>


class ConnectionListener : public BLooper {
public:
	ConnectionListener()
		: BLooper("LE connection probe listener"),
		  fReady(create_sem(0, "LE connection probe ready")),
		  fWhat(0),
		  fStatus(B_ERROR)
	{
	}

	~ConnectionListener()
	{
		delete_sem(fReady);
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case BT_MSG_LE_CONNECTING:
			case BT_MSG_LE_CONNECTED:
			case BT_MSG_LE_CONNECT_FAILED:
			case BT_MSG_LE_DISCONNECTED:
				fWhat = message->what;
				message->FindInt32("status", &fStatus);
				release_sem(fReady);
				break;
			default:
				BLooper::MessageReceived(message);
				break;
		}
	}

	status_t WaitForEvent(bigtime_t timeout)
	{
		return acquire_sem_etc(fReady, 1, B_RELATIVE_TIMEOUT, timeout);
	}

	uint32 Event() const { return fWhat; }
	status_t Status() const { return fStatus; }

private:
	sem_id fReady;
	uint32 fWhat;
	status_t fStatus;
};


static status_t
SendRequest(BMessenger& server, uint32 what, int32 hciID)
{
	BMessage request(what), reply;
	request.AddInt32("hci_id", hciID);
	status_t result = server.SendMessage(&request, &reply,
		5 * 1000 * 1000, 5 * 1000 * 1000);
	int32 status = B_ERROR;
	if (result == B_OK)
		reply.FindInt32("status", &status);
	return result == B_OK ? status : result;
}


int
main()
{
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
	ConnectionListener* listener = new ConnectionListener();
	listener->Run();
	// Static random address 0xC00000000001 is not a household peer.
	const uint8 address[6] = { 1, 0, 0, 0, 0, 0xc0 };
	BMessage request(BT_MSG_LE_CONNECT), reply;
	request.AddInt32("hci_id", device->ID());
	request.AddMessenger("listener", BMessenger(listener));
	request.AddData("address", B_RAW_TYPE, address, sizeof(address));
	request.AddUInt8("address_type", 1);
	status_t result = server.SendMessage(&request, &reply,
		5 * 1000 * 1000, 5 * 1000 * 1000);
	int32 status = B_ERROR;
	if (result == B_OK)
		reply.FindInt32("status", &status);
	printf("le_connect_request=%ld\n", (long)status);
	if (result == B_OK && status == B_OK)
		result = listener->WaitForEvent(10 * 1000 * 1000);
	if (result == B_OK) {
		printf("le_connection_event=%#lx status=%ld\n",
			(unsigned long)listener->Event(), (long)listener->Status());
		if (listener->Event() == BT_MSG_LE_CONNECTING) {
			status = SendRequest(server, BT_MSG_LE_CONNECT_CANCEL, device->ID());
			printf("le_connect_cancel=%ld\n", (long)status);
			result = status;
			if (result == B_OK)
				result = listener->WaitForEvent(15 * 1000 * 1000);
			if (result == B_OK) {
				printf("le_cancel_event=%#lx status=%ld\n",
					(unsigned long)listener->Event(), (long)listener->Status());
				if (listener->Event() != BT_MSG_LE_CONNECT_FAILED
					|| listener->Status() != B_CANCELED)
					result = B_ERROR;
			}
		} else if (listener->Event() == BT_MSG_LE_CONNECTED) {
			status = SendRequest(server, BT_MSG_LE_DISCONNECT, device->ID());
			printf("le_unexpected_link_disconnect=%ld\n", (long)status);
			result = B_ERROR;
		}
	} else
		printf("le_connection_wait=%ld\n", (long)result);
	if (listener->Lock())
		listener->Quit();
	return result == B_OK ? 0 : 1;
}
