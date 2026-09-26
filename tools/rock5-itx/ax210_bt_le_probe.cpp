/* Exercise Haiku's LE advertising path without recording nearby addresses. */
#include <bluetooth/LocalDevice.h>
#include <bluetoothserver_p.h>

#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <stdio.h>


class LEListener : public BLooper {
public:
	LEListener()
		: BLooper("AX210 LE scan listener"),
		  fReady(create_sem(0, "AX210 LE scan ready")),
		  fReports(0),
		  fStatus(B_ERROR)
	{
	}

	~LEListener()
	{
		delete_sem(fReady);
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case BT_MSG_LE_SCAN_STARTED:
				fStatus = B_OK;
				release_sem(fReady);
				break;
			case BT_MSG_LE_SCAN_ERROR:
				fStatus = B_ERROR;
				release_sem(fReady);
				break;
			case BT_MSG_LE_ADVERTISEMENT:
				atomic_add(&fReports, 1);
				break;
			default:
				BLooper::MessageReceived(message);
				break;
		}
	}

	status_t WaitForStart()
	{
		status_t result = acquire_sem_etc(fReady, 1, B_RELATIVE_TIMEOUT,
			10 * 1000 * 1000);
		return result == B_OK ? fStatus : result;
	}

	int32 Reports() { return atomic_add(&fReports, 0); }

private:
	sem_id fReady;
	int32 fReports;
	status_t fStatus;
};


int main()
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
	LEListener* listener = new LEListener();
	listener->Run();
	BMessage start(BT_MSG_LE_SCAN_START), reply;
	start.AddInt32("hci_id", device->ID());
	start.AddMessenger("listener", BMessenger(listener));
	status_t result = server.SendMessage(&start, &reply,
		5 * 1000 * 1000, 5 * 1000 * 1000);
	int32 status = B_ERROR;
	if (result == B_OK)
		reply.FindInt32("status", &status);
	printf("le_scan_request=%ld\n", (long)status);
	if (status == B_OK)
		result = listener->WaitForStart();
	else
		result = status;
	printf("le_scan_start=%ld\n", (long)result);
	if (result == B_OK)
		snooze(6 * 1000 * 1000);
	BMessage stop(BT_MSG_LE_SCAN_STOP), stopReply;
	stop.AddInt32("hci_id", device->ID());
	server.SendMessage(&stop, &stopReply,
		5 * 1000 * 1000, 5 * 1000 * 1000);
	int32 stopStatus = B_ERROR;
	stopReply.FindInt32("status", &stopStatus);
	printf("le_scan_stop=%ld advertising_reports=%ld\n",
		(long)stopStatus, (long)listener->Reports());
	if (listener->Lock())
		listener->Quit();
	return result == B_OK && stopStatus == B_OK ? 0 : 1;
}
