/* Does the Bluetooth adapter work?
 *
 * Asks the stack for the local adapter, prints what it says about itself, and
 * then runs an inquiry - the radio's own search for anything nearby that is
 * discoverable. Finding the adapter proves the driver and the stack are
 * talking to the hardware; finding another device proves the radio transmits
 * and receives.
 *
 * usage: bttest [seconds to look for]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <OS.h>

#include <bluetooth/LocalDevice.h>
#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/RemoteDevice.h>
#include <bluetooth/DeviceClass.h>

using namespace Bluetooth;


class Listener : public DiscoveryListener {
public:
	int fFound = 0;
	bool fDone = false;
	int fResult = -1;

	void DeviceDiscovered(RemoteDevice* device, DeviceClass cod) override
	{
		fFound++;
		bdaddr_t address = device->GetBluetoothAddress();
		BString name = device->GetFriendlyName(false);
		printf("  found %02x:%02x:%02x:%02x:%02x:%02x  %s\n",
			address.b[5], address.b[4], address.b[3],
			address.b[2], address.b[1], address.b[0],
			name.Length() > 0 ? name.String() : "(no name yet)");
	}

	void InquiryStarted(status_t status) override
	{
		printf("looking for devices nearby: %s\n",
			status == B_OK ? "started" : strerror(status));
		if (status != B_OK)
			fDone = true;
	}

	void InquiryResponse(int type) override
	{
		fResult = type;
		fDone = true;
	}
};


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	bigtime_t seconds = argc > 1 ? atoi(argv[1]) : 12;

	BApplication app("application/x-vnd.x399-bttest");

	printf("adapters the stack knows about: %" B_PRIu32 "\n",
		LocalDevice::GetLocalDeviceCount());

	LocalDevice* local = LocalDevice::GetLocalDevice();
	if (local == NULL) {
		fprintf(stderr, "[!] no local adapter: the stack has not claimed one\n");
		return 1;
	}

	bdaddr_t address = local->GetBluetoothAddress();
	printf("adapter %d: %02x:%02x:%02x:%02x:%02x:%02x\n", (int)local->ID(),
		address.b[5], address.b[4], address.b[3],
		address.b[2], address.b[1], address.b[0]);

	BString name = local->GetFriendlyName();
	printf("  name:  %s\n", name.Length() > 0 ? name.String() : "(none set)");

	DeviceClass cod = local->GetDeviceClass();
	printf("  class: %#" B_PRIx32 "\n", cod.Record());

	/* Each of these asks the stack a fresh question, and any one of them can
	 * be the one that never comes back, so say which is in flight.
	 */
	printf("  ... asking who made it\n");
	BString manufacturer = local->GetProperty("manufacturer");
	if (manufacturer.Length() > 0)
		printf("  made by: %s\n", manufacturer.String());

	printf("  ... asking its HCI version\n");
	uint32 version = 0;
	if (local->GetProperty("hciversion", &version) == B_OK)
		printf("  HCI version: %" B_PRIu32 "\n", version);

	printf("  ... asking its LMP version\n");
	uint32 lmp = 0;
	if (local->GetProperty("lmpversion", &lmp) == B_OK)
		printf("  LMP version: %" B_PRIu32 "\n", lmp);

	printf("  ... asking for a discovery agent\n");
	DiscoveryAgent* agent = local->GetDiscoveryAgent();
	if (agent == NULL) {
		fprintf(stderr, "[!] the adapter will not give out a discovery agent\n");
		return 1;
	}

	/* The listener is a looper, and it starts itself when it is made - so it
	 * is built on the heap and left to tidy itself away, the way a running
	 * looper must be.
	 */
	Listener* listener = new Listener();

	printf("looking for anything nearby for %" B_PRId64 " seconds...\n",
		(int64)seconds);
	status_t status = agent->StartInquiry(DiscoveryAgent::GIAC, listener,
		seconds);
	if (status != B_OK) {
		fprintf(stderr, "[!] asking for an inquiry: %s\n", strerror(status));
		return 1;
	}

	bigtime_t end = system_time() + (seconds + 6) * 1000000;
	while (!listener->fDone && system_time() < end)
		snooze(200000);

	if (!listener->fDone) {
		printf("the inquiry never came back\n");
		agent->CancelInquiry(listener);
		return 1;
	}

	const char* how = "?";
	switch (listener->fResult) {
		case DiscoveryListener::INQUIRY_COMPLETED: how = "finished"; break;
		case DiscoveryListener::INQUIRY_TERMINATED: how = "stopped early"; break;
		case DiscoveryListener::INQUIRY_ERROR: how = "failed"; break;
	}
	printf("inquiry %s, %d device%s found\n", how, listener->fFound,
		listener->fFound == 1 ? "" : "s");

	return listener->fResult == DiscoveryListener::INQUIRY_ERROR ? 1 : 0;
}
