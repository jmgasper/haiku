/*
 * Drive net80211 directly, without net_server or wpa_supplicant in the way.
 *
 * Joining through ifconfig goes to net_server, which always hands the job to
 * wpa_supplicant - so when nothing happens there is no telling whether the
 * driver never got the request or got it and failed. This asks the stack
 * itself, which makes the driver the only thing left that can be wrong.
 *
 * Usage: wifijoin <device> [<ssid>]
 *   with an ssid: set it, which starts authentication
 *   without:      report what the stack currently thinks
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <SupportDefs.h>

#include <compat/sys/cdefs.h>
#include <compat/sys/ioccom.h>
#include <net80211/ieee80211_ioctl.h>


static int
request(int socket, const char* device, int set, uint16_t type, void* data,
	uint16_t length, int16_t value, uint16_t* got)
{
	struct ieee80211req ireq;

	memset(&ireq, 0, sizeof(ireq));
	strlcpy(ireq.i_name, device, IFNAMSIZ);
	ireq.i_type = type;
	ireq.i_val = value;
	ireq.i_len = length;
	ireq.i_data = data;

	if (ioctl(socket, set ? SIOCS80211 : SIOCG80211, &ireq, sizeof(ireq)) < 0)
		return errno;

	if (got != NULL)
		*got = ireq.i_len;

	return 0;
}


/* Several of these are plain values carried in i_val rather than in a
 * buffer, so they need reading back differently.
 */
static int
value_of(int socket, const char* device, uint16_t type)
{
	struct ieee80211req ireq;

	memset(&ireq, 0, sizeof(ireq));
	strlcpy(ireq.i_name, device, IFNAMSIZ);
	ireq.i_type = type;

	if (ioctl(socket, SIOCG80211, &ireq, sizeof(ireq)) < 0)
		return -1;

	return ireq.i_val;
}


static void
report(int socket, const char* device)
{
	char ssid[IEEE80211_NWID_LEN + 1];
	uint8_t bssid[IEEE80211_ADDR_LEN];
	uint16_t length = sizeof(ssid) - 1;
	int error;

	memset(ssid, 0, sizeof(ssid));
	error = request(socket, device, 0, IEEE80211_IOC_SSID, ssid, length, 0,
		&length);
	if (error != 0)
		printf("  ssid: cannot read it (%s)\n", strerror(error));
	else {
		ssid[length] = '\0';
		printf("  ssid: \"%s\"\n", length > 0 ? ssid : "(none set)");
	}

	{
		uint16_t got = 0;
		int privacy = -1, wpa = -1;

		if (request(socket, device, 0, IEEE80211_IOC_PRIVACY, &privacy,
				sizeof(privacy), 0, &got) == 0)
			privacy = value_of(socket, device, IEEE80211_IOC_PRIVACY);
		wpa = value_of(socket, device, IEEE80211_IOC_WPA);
		printf("  privacy: %d, wpa mode: %d\n", privacy, wpa);
	}

	memset(bssid, 0, sizeof(bssid));
	error = request(socket, device, 0, IEEE80211_IOC_BSSID, bssid,
		sizeof(bssid), 0, NULL);
	if (error != 0)
		printf("  bssid: cannot read it (%s)\n", strerror(error));
	else {
		printf("  bssid: %02x:%02x:%02x:%02x:%02x:%02x\n", bssid[0], bssid[1],
			bssid[2], bssid[3], bssid[4], bssid[5]);
	}
}


int
main(int argc, char** argv)
{
	const char* device;
	int socket;
	int held;
	int waited;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <device> [<ssid>]\n", argv[0]);
		return 1;
	}

	device = argv[1];

	/* Hold the device open for as long as this runs. Haiku builds the vap
	 * when the device is opened and tears it down again when the last
	 * handle closes, so anything configured on a vap nobody is holding is
	 * thrown away within seconds - which is why the name and the crypto
	 * flags kept coming back unset.
	 */
	held = open(device, O_RDWR);
	if (held < 0)
		printf("could not hold the device open: %s\n", strerror(errno));
	else
		printf("holding the device open\n");

	socket = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (socket < 0) {
		fprintf(stderr, "no socket: %s\n", strerror(errno));
		return 1;
	}

	if (argc > 2) {
		const char* ssid = argv[2];
		size_t length = strlen(ssid);
		int error;

		/* Say who we are willing to talk to before naming the network, or
		 * the stack keeps whatever it was last told and looks for that.
		 */
		error = request(socket, device, 1, IEEE80211_IOC_ROAMING,
			NULL, 0, IEEE80211_ROAMING_AUTO, NULL);
		if (error != 0)
			printf("roaming mode refused: %s\n", strerror(error));

		error = request(socket, device, 1, IEEE80211_IOC_SSID,
			(void*)ssid, (uint16_t)length, 0, NULL);
		if (error != 0) {
			fprintf(stderr, "setting the name failed: %s\n", strerror(error));
			close(socket);
			return 1;
		}

		printf("asked for \"%s\"\n", ssid);

		/* Asking for a scan outright cancels whatever is running and
		 * starts another, and that path wedges the whole stack. Only do
		 * it when told to; otherwise leave the vap's own scan to find
		 * the network, which it is already doing.
		 */
		if (argc > 4 && strcmp(argv[4], "scan") == 0) {
			struct ieee80211_scan_req req;

			/* Look at what has already been heard rather than asking
			 * for a fresh sweep. Starting one means cancelling the
			 * one in flight, and that path takes the whole stack
			 * down. This one joins straight from the scan cache.
			 *
			 * The absence of NOJOIN is the point: the flags of the
			 * last request stick to the vap, and everything else on
			 * the system asks for scans that must not join, so the
			 * stack had been told never to join anything.
			 */
			memset(&req, 0, sizeof(req));
			req.sr_flags = IEEE80211_IOC_SCAN_ACTIVE
				| IEEE80211_IOC_SCAN_CHECK;
			req.sr_duration = IEEE80211_IOC_SCAN_FOREVER;
			req.sr_mindwell = 500;
			req.sr_maxdwell = 1500;
			req.sr_nssid = 1;
			req.sr_ssid[0].len = (int)length;
			memcpy(req.sr_ssid[0].ssid, ssid, length);

			error = request(socket, device, 1, IEEE80211_IOC_SCAN_REQ,
				&req, (uint16_t)sizeof(req), 0, NULL);
			if (error != 0)
				printf("the scan request was refused: %s\n",
					strerror(error));
			else
				printf("asked it to look and join\n");
		}
	}

	/* Configure it while it is down. Every one of these settings returns
	 * ENETRESET, and the stack acts on that by putting a *running* vap
	 * through ieee80211_init - which is the thing that ends with the
	 * machine stopped. Down, it just stores the setting.
	 */
	if (request(socket, device, 1, IEEE80211_IOC_HAIKU_COMPAT_WLAN_DOWN,
			NULL, 0, 0, NULL) != 0)
		printf("could not put it down first\n");
	else
		printf("put it down to be configured\n");

	if (argc > 3 && (strcmp(argv[3], "wpa") == 0
			|| strcmp(argv[3], "priv") == 0)) {
		int error;

		/* After the name, not before: setting the name resets the vap
		 * and the crypto flags do not survive it, so the stack went on
		 * refusing every encrypted network on privacy grounds.
		 *
		 * What wpa_supplicant would say on our behalf. Without it the
		 * stack refuses every encrypted network out of hand, which looks
		 * from outside exactly like a scan that found nothing.
		 */
		error = request(socket, device, 1, IEEE80211_IOC_PRIVACY, NULL, 0,
			1, NULL);
		if (error != 0)
			printf("privacy refused: %s\n", strerror(error));

		if (strcmp(argv[3], "wpa") == 0) {
			error = request(socket, device, 1, IEEE80211_IOC_WPA, NULL, 0,
				2, NULL);
			if (error != 0)
				printf("wpa mode refused: %s\n", strerror(error));
			else
				printf("told the stack this network is WPA2\n");
		} else
			printf("privacy only, no WPA mode\n");
	}

	/* Now bring it up, already knowing what it is looking for. */
	if (request(socket, device, 1, IEEE80211_IOC_HAIKU_COMPAT_WLAN_UP,
			NULL, 0, 0, NULL) != 0)
		printf("could not bring it back up\n");
	else
		printf("brought it up configured\n");

	/* Watch it settle rather than looking once and leaving: the whole
	 * point is to still be holding the device while it associates.
	 */
	for (waited = 0; waited < 60; waited += 5) {
		printf("after %ds:\n", waited);
		report(socket, device);
		sleep(5);
	}

	close(socket);
	if (held >= 0)
		close(held);

	return 0;
}
