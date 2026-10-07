/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Exercise malformed join requests on a live OpenBSD-backed interface.
// Every request must be rejected before changing the current association.
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
	if (argc != 2) {
		fprintf(stderr, "usage: %s interface\n", argv[0]);
		return 2;
	}
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	alignas(ieee80211_haiku_join_req) uint8 buffer[
		sizeof(ieee80211_haiku_join_req) + 64];
	const char* names[] = {"empty", "short header", "oversized SSID",
		"oversized key", "truncated key", "invalid operation"};
	for (int test = 0; test < 6; test++) {
		memset(buffer, 0, sizeof(buffer));
		auto* join = reinterpret_cast<ieee80211_haiku_join_req*>(buffer);
		ieee80211req request = {};
		strlcpy(request.i_name, argv[1], sizeof(request.i_name));
		request.i_type = IEEE80211_IOC_HAIKU_JOIN;
		request.i_len = sizeof(buffer);
		request.i_data = buffer;
		int operation = SIOCS80211;
		switch (test) {
			case 0: request.i_len = 0; break;
			case 1: request.i_len = sizeof(*join) - 1; break;
			case 2: join->i_nwid_len = sizeof(join->i_nwid) + 1; break;
			case 3: join->i_key_len = 33; break;
			case 4:
				join->i_key_len = 32;
				request.i_len = sizeof(*join) + 31;
				break;
			case 5: operation = SIOCG80211; break;
		}
		errno = 0;
		int result = ioctl(fd, operation, &request, sizeof(request));
		if (result != -1 || errno != B_BAD_VALUE) {
			fprintf(stderr, "FAIL %s result=%d error=%s (%d)\n",
				names[test], result, strerror(errno), errno);
			close(fd);
			return 1;
		}
		printf("PASS %s rejected\n", names[test]);
	}
	close(fd);
	return 0;
}
