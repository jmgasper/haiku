/*
 * What net80211 has counted for the access point we are joined to: frames
 * each way, and every reason it threw one away. When the link is up and
 * nothing gets through, this says which side is losing them.
 *
 * Usage: wifistats <device>
 * Build (on the workstation, next to wifijoin): g++ -o wifistats wifistats.cpp
 *   -Iinc/freebsd_network -Iinc/freebsd_wlan -lnetwork
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
request(int socket, const char* device, uint16_t type, void* data,
	uint16_t length)
{
	struct ieee80211req ireq;

	memset(&ireq, 0, sizeof(ireq));
	strlcpy(ireq.i_name, device, IFNAMSIZ);
	ireq.i_type = type;
	ireq.i_len = length;
	ireq.i_data = data;

	if (ioctl(socket, SIOCG80211, &ireq, sizeof(ireq)) < 0)
		return errno;
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <device>\n", argv[0]);
		return 1;
	}

	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ieee80211req_sta_stats stats;
	memset(&stats, 0, sizeof(stats));

	int error = request(s, argv[1], IEEE80211_IOC_BSSID, stats.is_u.macaddr,
		IEEE80211_ADDR_LEN);
	if (error == 0) {
		error = request(s, argv[1], IEEE80211_IOC_STA_STATS, &stats,
			sizeof(stats));
	}
	close(s);
	if (error != 0) {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(error));
		return 1;
	}

	const struct ieee80211_nodestats& n = stats.is_stats;
	const uint8_t* a = stats.is_u.macaddr;
	printf("%02x:%02x:%02x:%02x:%02x:%02x\n", a[0], a[1], a[2], a[3], a[4], a[5]);
#define SHOW(field) printf("  %-18s %u\n", #field, (unsigned)n.field)
	SHOW(ns_rx_data); SHOW(ns_rx_mgmt); SHOW(ns_rx_ucast); SHOW(ns_rx_mcast);
	SHOW(ns_rx_dup); SHOW(ns_rx_noprivacy); SHOW(ns_rx_wepfail);
	SHOW(ns_rx_demicfail); SHOW(ns_rx_decap); SHOW(ns_rx_defrag);
	SHOW(ns_rx_decryptcrc); SHOW(ns_rx_unauth); SHOW(ns_rx_unencrypted);
	SHOW(ns_rx_drop);
	SHOW(ns_tx_data); SHOW(ns_tx_mgmt); SHOW(ns_tx_ucast); SHOW(ns_tx_mcast);
	printf("  %-18s %llu\n", "ns_tx_bytes", (unsigned long long)n.ns_tx_bytes);
	printf("  %-18s %llu\n", "ns_rx_bytes", (unsigned long long)n.ns_rx_bytes);
	SHOW(ns_tx_assoc); SHOW(ns_tx_deauth); SHOW(ns_rx_deauth);
	return 0;
}
