/* Inspect HE capability elements returned by Haiku's wireless scan ioctl.
 * Distributed under the terms of the MIT License.
 */

#include <errno.h>
#include <SupportDefs.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <unistd.h>

extern "C" {
#include <compat/sys/cdefs.h>
#include <compat/sys/ioccom.h>
#include <net80211/ieee80211_ioctl.h>
}

int
main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s interface ssid\n", argv[0]);
		return 2;
	}
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	uint8_t buffer[65535];
	struct ieee80211req request = {};
	strlcpy(request.i_name, argv[1], sizeof(request.i_name));
	request.i_type = IEEE80211_IOC_SCAN_RESULTS;
	request.i_len = sizeof(buffer);
	request.i_data = buffer;
	if (ioctl(fd, SIOCG80211, &request, sizeof(request)) < 0) {
		perror("SIOCG80211 scan results");
		close(fd);
		return 1;
	}
	close(fd);
	int matches = 0;
	for (size_t offset = 0; offset + sizeof(ieee80211req_scan_result)
			<= request.i_len;) {
		const ieee80211req_scan_result* result =
			(const ieee80211req_scan_result*)(buffer + offset);
		if (result->isr_len < sizeof(*result)
			|| offset + result->isr_len > request.i_len)
			break;
		const size_t ssidOffset = result->isr_ie_off;
		const size_t ieOffset = ssidOffset + result->isr_ssid_len
			+ result->isr_meshid_len;
		if (ieOffset + result->isr_ie_len > result->isr_len) {
			offset += result->isr_len;
			continue;
		}
		const char* ssid = argv[2];
		if (strlen(ssid) == result->isr_ssid_len
			&& memcmp(buffer + offset + ssidOffset, ssid,
				result->isr_ssid_len) == 0) {
			bool he = false;
			const uint8_t* heIe = NULL;
			size_t heIeLen = 0;
			const uint8_t* ie = buffer + offset + ieOffset;
			for (size_t pos = 0; pos + 2 <= result->isr_ie_len;) {
				const size_t len = ie[pos + 1];
				if (pos + 2 + len > result->isr_ie_len)
					break;
				if (ie[pos] == 255 && len >= 1 && ie[pos + 2] == 35) {
					he = true;
					heIe = ie + pos + 3;
					heIeLen = len - 1;
				}
				pos += len + 2;
			}
			printf("ssid=%s freq=%u rssi=%d HE=%s\n", ssid,
				result->isr_freq, result->isr_rssi, he ? "yes" : "no");
			if (heIe != NULL) {
				printf("HE capabilities: ");
				for (size_t i = 0; i < heIeLen; i++)
					printf("%02x", heIe[i]);
				printf("\n");
			}
			matches++;
		}
		offset += result->isr_len;
	}
	printf("matching APs=%d\n", matches);
	return matches > 0 ? 0 : 1;
}
