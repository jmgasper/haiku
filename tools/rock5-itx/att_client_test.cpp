/* Scripted ATT peer exercising service pagination, reads and notifications. */
#include <LEAttributeClient.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <thread>
#include <vector>


static bool
Exchange(int socket, const std::vector<uint8>& expected,
	const std::vector<uint8>& response,
	const std::vector<uint8>& beforeResponse = {})
{
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(socket, &readable);
	timeval timeout = { 6, 0 };
	if (select(socket + 1, &readable, NULL, NULL, &timeout) != 1)
		return false;
	uint8 request[32];
	ssize_t received = recv(socket, request, sizeof(request), 0);
	if (received != (ssize_t)expected.size()
		|| !std::equal(expected.begin(), expected.end(), request))
		return false;
	if (!beforeResponse.empty() && send(socket, beforeResponse.data(),
		beforeResponse.size(), 0) != (ssize_t)beforeResponse.size())
		return false;
	return send(socket, response.data(), response.size(), 0)
		== (ssize_t)response.size();
}


int
main()
{
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) != 0) {
		perror("socketpair");
		return 1;
	}
	bool highDescriptor = false;
	int high = fcntl(sockets[0], F_DUPFD, FD_SETSIZE + 16);
	if (high >= 0) {
		close(sockets[0]);
		sockets[0] = high;
		highDescriptor = true;
	}
	Bluetooth::LEAttributeClient client;
	client.AdoptConnectedSocket(sockets[0]);
	std::atomic<bool> peerOK(true);
	std::thread peer([&]() {
		peerOK = Exchange(sockets[1], {0x10,1,0,0xff,0xff,0,0x28},
			{0x11,6,1,0,5,0,0,0x18,6,0,30,0,0x12,0x18})
			&& Exchange(sockets[1], {0x10,31,0,0xff,0xff,0,0x28},
				{1,0x10,31,0,0x0a})
			&& Exchange(sockets[1], {8,6,0,30,0,3,0x28},
				{9,7,6,0,2,7,0,0x4b,0x2a,8,0,0x10,9,0,0x33,0x2a})
			&& Exchange(sockets[1], {8,9,0,30,0,3,0x28},
				{9,7,12,0,2,13,0,0x4e,0x2a})
			&& Exchange(sockets[1], {8,13,0,30,0,3,0x28},
				{1,8,13,0,0x0a})
			&& Exchange(sockets[1], {0x0a,7,0},
				{0x0b,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22})
			&& Exchange(sockets[1], {0x0c,7,0,22,0},
				{0x0d,23,24,25})
			&& Exchange(sockets[1], {4,10,0,11,0},
				{5,1,10,0,2,0x29})
			&& Exchange(sockets[1], {0x12,10,0,1,0}, {0x13},
				{0x1b,9,0,4,5,6});
		if (peerOK)
			peerOK = send(sockets[1], "\x1b\x09\x00\x01\x02\x03", 6, 0) == 6;
		close(sockets[1]);
	});

	std::vector<Bluetooth::LEPrimaryService> services;
	std::vector<Bluetooth::LECharacteristic> characteristics;
	std::vector<uint8> value;
	uint16 descriptor = 0, notificationHandle = 0;
	bool okay = client.DiscoverPrimaryServices(services) == B_OK
		&& services.size() == 2 && services[1].uuid16 == 0x1812
		&& client.DiscoverCharacteristics(6, 30, characteristics) == B_OK
		&& characteristics.size() == 3
		&& characteristics[1].uuid16 == 0x2a33
		&& client.ReadAttribute(7, value) == B_OK
		&& value.size() == 25 && value.back() == 25
		&& client.FindClientConfiguration(10, 11, descriptor) == B_OK
		&& descriptor == 10;
	const uint8 enable[2] = {1, 0};
	if (okay)
		okay = client.WriteAttribute(descriptor, enable, sizeof(enable)) == B_OK
			&& client.ReadNotification(notificationHandle, value) == B_OK
			&& notificationHandle == 9 && value.size() == 3 && value[2] == 6
			&& client.ReadNotification(notificationHandle, value) == B_OK
			&& notificationHandle == 9 && value.size() == 3 && value[2] == 3;
	peer.join();
	client.Disconnect();
	printf("att_client_script=%s att_high_fd=%s\n",
		okay && peerOK ? "pass" : "fail",
		highDescriptor ? "pass" : "unavailable");
	return okay && peerOK ? 0 : 1;
}
