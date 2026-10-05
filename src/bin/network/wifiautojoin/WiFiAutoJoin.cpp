/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <string.h>
#include <vector>

#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <OS.h>

#include "../../../shared/net/WirelessNetworkList.h"


static bool
IsAssociated(BNetworkDevice& device)
{
	wireless_network network;
	uint32 cookie = 0;
	return device.HasLink()
		&& device.GetNextAssociatedNetwork(cookie, network) == B_OK;
}


static std::vector<wireless_network>
SavedNetworks()
{
	std::vector<wireless_network> saved;
	BNetworkRoster& roster = BNetworkRoster::Default();
	int32 count = roster.CountPersistentNetworks();
	for (int32 i = 0; i < count; i++) {
		wireless_network network;
		// Older libbnetapi versions do not advance the cookie.
		uint32 cookie = i;
		if (roster.GetNextPersistentNetwork(&cookie, network) == B_OK)
			saved.push_back(network);
	}
	return saved;
}


static bool
IsSaved(const wireless_network& candidate,
	const std::vector<wireless_network>& saved)
{
	// Saved networks are matched by name only: a mesh's access points may
	// differ in their advertised security details.
	for (const wireless_network& network : saved) {
		if (WirelessNetworkNameEquals(candidate.name, network.name))
			return true;
	}
	return false;
}


int
main()
{
	// Run as a short-lived startup client, separate from Deskbar's UI thread.
	// Early firmware or net_server startup can make the first scan unavailable.
	for (int32 attempt = 0; attempt < 8; attempt++) {
		if (attempt > 0)
			snooze(5 * 1000000);
		std::vector<wireless_network> saved = SavedNetworks();
		fprintf(stderr, "wifiautojoin: attempt %ld, %zu saved networks\n",
			(long)attempt + 1, saved.size());
		if (saved.empty())
			continue;

		BNetworkInterface interface;
		uint32 cookie = 0;
		BNetworkRoster& roster = BNetworkRoster::Default();
		while (roster.GetNextInterface(&cookie, interface) == B_OK) {
			BNetworkDevice device(interface.Name());
			if (!device.IsWireless())
				continue;
			if (IsAssociated(device))
				return 0;
			fprintf(stderr, "wifiautojoin: scanning %s\n", interface.Name());
			status_t scanStatus = device.Scan(false, false);
			fprintf(stderr, "wifiautojoin: scan returned %ld\n",
				(long)scanStatus);
			if (scanStatus != B_OK)
				continue;

			// Scan is asynchronous. Read its cache after the radio has had time
			// to collect advertisements, while leaving UI threads free.
			snooze(6 * 1000000);
			if (IsAssociated(device))
				return 0;
			wireless_network* networks = NULL;
			uint32 count = 0;
			status_t status = device.GetNetworks(networks, count);
			std::vector<WirelessNetworkGroup> visible;
			if (status == B_OK)
				visible = GroupWirelessNetworks(networks, count);
			LogWirelessScan("autojoin", interface.Name(), networks, count,
				visible);
			fprintf(stderr, "wifiautojoin: %lu visible networks (status %ld)\n",
				(unsigned long)visible.size(), (long)status);
			delete[] networks;
			// Groups are sorted by signal: the first saved one is the best
			for (const WirelessNetworkGroup& group : visible) {
				const wireless_network& network = group.network;
				fprintf(stderr, "wifiautojoin: candidate %s, %ld dBm, %ld "
					"access points\n", network.name,
					(long)WirelessSignalDBm(network),
					(long)group.accessPoints);
				if (!IsSaved(network, saved))
					continue;
				if (IsAssociated(device))
					return 0;
				// Join by name so the driver picks the access point
				WiFiDebugLog("autojoin", "joining saved network \"%s\"",
					network.name);
				status = device.JoinNetwork(network.name);
				WiFiDebugLog("autojoin", "join request: %s",
					strerror(status));
				if (status != B_OK)
					return 1;
				for (int32 second = 0; second < 20; second++) {
					snooze(1000000);
					if (IsAssociated(device)) {
						printf("wifiautojoin: connected to %s\n", network.name);
						WiFiDebugLog("autojoin", "connected to \"%s\"",
							network.name);
						return 0;
					}
				}
				fprintf(stderr, "wifiautojoin: join timed out\n");
				WiFiDebugLog("autojoin", "joining \"%s\" timed out",
					network.name);
				return 1;
			}
		}
	}
	fprintf(stderr, "wifiautojoin: no saved network available\n");
	return 1;
}
