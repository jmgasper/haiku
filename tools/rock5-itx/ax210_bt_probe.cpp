/* Bluetooth Classic controller and inquiry probe for the ROCK 5 ITX. */
#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/LocalDevice.h>

#include <OS.h>
#include <stdio.h>
#include <unistd.h>

static sem_id sInquiryDone;
static int32 sDevices;

class ProbeListener : public Bluetooth::DiscoveryListener {
public:
	void InquiryStarted(status_t status) override
	{
		printf("inquiry_start_status=%ld\n", (long)status);
		fflush(stdout);
	}

	void DeviceDiscovered(Bluetooth::RemoteDevice*, Bluetooth::DeviceClass) override
	{
		atomic_add(&sDevices, 1);
	}

	void InquiryResponse(int result) override
	{
		printf("inquiry_result=%d devices=%ld\n", result, (long)sDevices);
		fflush(stdout);
		release_sem(sInquiryDone);
	}
};

int main()
{
	sInquiryDone = create_sem(0, "AX210 inquiry complete");
	if (sInquiryDone < 0)
		return 1;
	alarm(35);
	Bluetooth::LocalDevice* device = Bluetooth::LocalDevice::GetLocalDevice();
	if (device == NULL) {
		fprintf(stderr, "no Bluetooth controller\n");
		return 1;
	}
	ProbeListener* listener = new ProbeListener();
	Bluetooth::DiscoveryAgent* agent = device->GetDiscoveryAgent();
	status_t status = agent->StartInquiry(Bluetooth::DiscoveryAgent::GIAC,
		listener, 6);
	printf("start_inquiry=%ld\n", (long)status);
	fflush(stdout);
	if (status == B_OK)
		status = acquire_sem_etc(sInquiryDone, 1, B_RELATIVE_TIMEOUT,
			15 * 1000 * 1000);
	printf("wait_result=%ld devices=%ld\n", (long)status, (long)sDevices);
	fflush(stdout);
	if (listener->Lock())
		listener->Quit();
	delete_sem(sInquiryDone);
	return status == B_OK ? 0 : 1;
}
