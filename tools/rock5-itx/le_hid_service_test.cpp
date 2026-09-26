/* Scripted HID-over-GATT discovery and report subscription. MIT License. */
#include <LEHIDService.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <thread>
#include <vector>


static bool
Exchange(int socket, const std::vector<uint8>& expected,
	const std::vector<uint8>& response)
{
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(socket, &readable);
	timeval timeout = { 6, 0 };
	if (select(socket + 1, &readable, NULL, NULL, &timeout) != 1)
		return false;
	uint8 request[32];
	ssize_t length = recv(socket, request, sizeof(request), 0);
	if (length != (ssize_t)expected.size()
		|| !std::equal(expected.begin(), expected.end(), request))
		return false;
	return send(socket, response.data(), response.size(), 0)
		== (ssize_t)response.size();
}


int
main()
{
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) != 0)
		return 1;
	Bluetooth::LEAttributeClient client;
	client.AdoptConnectedSocket(sockets[0]);
	std::atomic<bool> peerOK(true);
	std::thread peer([&]() {
		const std::vector<uint8> descriptors = { 5, 1,
			6, 0, 8, 0x29, 7, 0, 2, 0x29 };
		peerOK = Exchange(sockets[1], {0x10,1,0,0xff,0xff,0,0x28},
			{0x11,6,1,0,20,0,0x12,0x18})
			&& Exchange(sockets[1], {0x10,21,0,0xff,0xff,0,0x28},
				{1,0x10,21,0,0x0a})
			&& Exchange(sockets[1], {8,1,0,20,0,3,0x28},
				{9,7,2,0,2,3,0,0x4b,0x2a,
					4,0,0x10,5,0,0x4d,0x2a})
			&& Exchange(sockets[1], {8,5,0,20,0,3,0x28},
				{1,8,5,0,0x0a})
			&& Exchange(sockets[1], {0x0a,3,0},
				{0x0b,0x05,0x01,0x09,0x02,0xa1,0x01,0xc0})
			&& Exchange(sockets[1], {4,6,0,20,0}, descriptors)
			&& Exchange(sockets[1], {0x0a,6,0}, {0x0b,1,1})
			&& Exchange(sockets[1], {4,6,0,20,0}, descriptors)
			&& Exchange(sockets[1], {0x12,7,0,1,0}, {0x13});
		if (peerOK)
			peerOK = send(sockets[1], "\x1b\x05\x00\x01\x02", 5, 0) == 5;
		close(sockets[1]);
	});

	Bluetooth::LEHIDService hid;
	bool okay = hid.Discover(client) == B_OK
		&& hid.ReportMap().size() == 7
		&& hid.InputReports().size() == 1
		&& hid.FindInputReport(5) != NULL
		&& hid.FindInputReport(5)->reportID == 1
		&& hid.EnableInputReports(client) == B_OK;
	uint16 notificationHandle = 0;
	std::vector<uint8> report;
	if (okay)
		okay = client.ReadNotification(notificationHandle, report) == B_OK
			&& notificationHandle == 5 && report.size() == 2
			&& report[0] == 1 && report[1] == 2;
	peer.join();
	client.Disconnect();
	printf("le_hid_service_script=%s\n", okay && peerOK ? "pass" : "fail");
	return okay && peerOK ? 0 : 1;
}
