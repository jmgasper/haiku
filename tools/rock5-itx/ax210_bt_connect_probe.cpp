/* Controlled Bluetooth Classic connection probe for an explicitly named peer.
 * Distributed under the terms of the MIT License.
 */

#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/RemoteDevice.h>
#include <bluetooth/bdaddrUtils.h>

#include <OS.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

static sem_id sInquiryDone;
static Bluetooth::RemoteDevice* sPeer;
static const char* sTargetAddress;

class PeerListener : public Bluetooth::DiscoveryListener {
public:
	void DeviceDiscovered(Bluetooth::RemoteDevice* device,
		Bluetooth::DeviceClass) override
	{
		BString address = Bluetooth::bdaddrUtils::ToString(
			device->GetBluetoothAddress());
		if (strcasecmp(address.String(), sTargetAddress) == 0)
			sPeer = device;
	}

	void InquiryResponse(int result) override
	{
		printf("inquiry_result=%d target_found=%s\n", result,
			sPeer != NULL ? "yes" : "no");
		fflush(stdout);
		release_sem(sInquiryDone);
	}
};

int
main(int argc, char** argv)
{
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s target-bdaddr [hold-seconds]\n", argv[0]);
		return 2;
	}
	char* end = NULL;
	long holdSeconds = argc == 3 ? strtol(argv[2], &end, 10) : 0;
	if (holdSeconds < 0 || holdSeconds > 30
		|| (argc == 3 && (end == argv[2] || *end != 0)))
		return 2;
	sTargetAddress = argv[1];
	sInquiryDone = create_sem(0, "AX210 peer inquiry done");
	if (sInquiryDone < 0)
		return 1;
	alarm(60);
	Bluetooth::LocalDevice* local = Bluetooth::LocalDevice::GetLocalDevice();
	if (local == NULL) {
		fprintf(stderr, "no Bluetooth controller\n");
		return 1;
	}
	PeerListener* listener = new PeerListener();
	Bluetooth::DiscoveryAgent* agent = local->GetDiscoveryAgent();
	status_t status = agent->StartInquiry(Bluetooth::DiscoveryAgent::GIAC,
		listener, 6);
	if (status == B_OK)
		status = acquire_sem_etc(sInquiryDone, 1, B_RELATIVE_TIMEOUT,
			15 * 1000 * 1000);
	if (status != B_OK || sPeer == NULL) {
		printf("peer_unavailable status=%ld\n", (long)status);
		return 1;
	}
	status = sPeer->Connect();
	printf("connect_request=%ld\n", (long)status);
	fflush(stdout);
	if (status == B_OK) {
		for (int i = 0; i < 15; i++) {
			if (sPeer->GetConnectionState()
				== Bluetooth::RemoteDevice::CONNECTED) {
				printf("connection=established\n");
				fflush(stdout);
				if (holdSeconds > 0) {
					snooze(holdSeconds * 1000 * 1000);
					printf("connection_after_hold=%s\n",
						sPeer->GetConnectionState()
							== Bluetooth::RemoteDevice::CONNECTED
						? "established" : "disconnected");
				}
				sPeer->Disconnect(false);
				return 0;
			}
			snooze(1000 * 1000);
		}
	}
	printf("connection=not_established\n");
	return 1;
}
