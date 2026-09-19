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

	if (argc < 2) {
		fprintf(stderr, "usage: %s <device> [<ssid>]\n", argv[0]);
		return 1;
	}

	device = argv[1];
	socket = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (socket < 0) {
		fprintf(stderr, "no socket: %s\n", strerror(errno));
		return 1;
	}

	if (argc > 3 && strcmp(argv[3], "wpa") == 0) {
		int error;

		/* What wpa_supplicant would say on our behalf. Without it the
		 * stack refuses every encrypted network out of hand, which looks
		 * from outside exactly like a scan that found nothing.
		 */
		error = request(socket, device, 1, IEEE80211_IOC_PRIVACY, NULL, 0,
			1, NULL);
		if (error != 0)
			printf("privacy refused: %s\n", strerror(error));

		error = request(socket, device, 1, IEEE80211_IOC_WPA, NULL, 0, 2,
			NULL);
		if (error != 0)
			printf("wpa mode refused: %s\n", strerror(error));
		else
			printf("told the stack this network is WPA2\n");
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

		/* Setting the name alone changes nothing: every scan the stack
		 * was running came from ifconfig and carried "nojoin", so it
		 * looked around for ever and never tried to join anything. Ask
		 * for a scan that is allowed to pick and join.
		 */
		{
			struct ieee80211_scan_req req;

			/* One is always already running, and the stack refuses a
			 * second. Stop it first or the request never lands.
			 */
			request(socket, device, 1, IEEE80211_IOC_SCAN_CANCEL,
				NULL, 0, 0, NULL);
			usleep(300000);

			memset(&req, 0, sizeof(req));
			req.sr_flags = IEEE80211_IOC_SCAN_ACTIVE
				| IEEE80211_IOC_SCAN_FLUSH;
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

	printf("the stack says:\n");
	report(socket, device);

	close(socket);
	return 0;
}
