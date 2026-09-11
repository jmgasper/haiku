/*
 * Exercise the real Services listener with reverse-ordered pipe descriptors.
 * Distributed under the terms of the MIT License.
 */

#include "Services.h"

#include <OS.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static sem_id sSelecting;
static thread_id sListener = -1;
static int sReadPipe;
static int sWritePipe;
static bool sLegacyRange;
static const char kReply[] = "ROCK5_SERVICE_OK\n";

extern "C" int __real_pipe(int*);
extern "C" int __real_select(int, fd_set*, fd_set*, fd_set*, struct timeval*);


extern "C" int
__wrap_pipe(int* descriptors)
{
	if (__real_pipe(descriptors) != 0)
		return -1;
	int high = fcntl(descriptors[0], F_DUPFD, 64);
	if (high < 0) {
		close(descriptors[0]);
		close(descriptors[1]);
		return -1;
	}
	close(descriptors[0]);
	descriptors[0] = high;
	sReadPipe = descriptors[0];
	sWritePipe = descriptors[1];
	return 0;
}


extern "C" int
__wrap_select(int count, fd_set* read, fd_set* write, fd_set* error,
	struct timeval* timeout)
{
	if (sListener == -1) {
		sListener = find_thread(NULL);
		release_sem(sSelecting);
	}
	// Negative control: reproduce the old initial select() range.
	if (sLegacyRange)
		count = sWritePipe + 1;
	return __real_select(count, read, write, error, timeout);
}


static bool
CheckListener(const char* executable)
{
	sSelecting = create_sem(0, "service select entered");
	if (sSelecting < B_OK)
		return false;
	Services* services = new Services(BMessage());
	if (services->InitCheck() != B_OK
		|| acquire_sem_etc(sSelecting, 1, B_RELATIVE_TIMEOUT, 2000000) != B_OK)
		return false;
	// Wait until the listener is sleeping in its first select, before adding
	// any socket. This prevents a pre-existing service from hiding the bug.
	bigtime_t deadline = system_time() + 2000000;
	thread_info info = {};
	while (get_thread_info(sListener, &info) == B_OK
		&& info.state != B_THREAD_WAITING && system_time() < deadline)
		snooze(1000);
	if (info.state != B_THREAD_WAITING || sReadPipe <= sWritePipe)
		return false;
	printf("ROCK5_SERVICES_SETUP read_pipe=%d write_pipe=%d\n", sReadPipe, sWritePipe);

	int connection = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	if (connection < 0 || bind(connection, (sockaddr*)&address, sizeof(address)) != 0
		|| getsockname(connection, (sockaddr*)&address, &length) != 0)
		return false;
	close(connection);

	BMessage endpoint, value, update(kMsgUpdateServices);
	if (endpoint.AddString("address", "127.0.0.1") != B_OK
		|| value.AddString("name", "rock5_service_test") != B_OK
		|| value.AddInt32("family", AF_INET) != B_OK
		|| value.AddString("type", "stream") != B_OK
		|| value.AddString("protocol", "tcp") != B_OK
		|| value.AddInt32("port", ntohs(address.sin_port)) != B_OK
		|| value.AddString("launch", executable) != B_OK
		|| value.AddString("launch", "--handler") != B_OK
		|| value.AddMessage("address", &endpoint) != B_OK
		|| update.AddMessage("service", &value) != B_OK)
		return false;
	services->MessageReceived(&update);

	connection = socket(AF_INET, SOCK_STREAM, 0);
	struct timeval timeout = {3, 0};
	char reply[sizeof(kReply) - 1] = {};
	bool passed = connection >= 0
		&& setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
		&& connect(connection, (sockaddr*)&address, sizeof(address)) == 0
		&& recv(connection, reply, sizeof(reply), MSG_WAITALL) == sizeof(reply)
		&& memcmp(reply, kReply, sizeof(reply)) == 0;
	close(connection);
	printf("ROCK5_SERVICES read_pipe=%d write_pipe=%d legacy=%d reply=%d\n",
		sReadPipe, sWritePipe, sLegacyRange, passed);
	// This one-shot process owns the listener and all its sockets. Exit below
	// tears them down without waiting for the production server's lifetime.
	return passed;
}


int
main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "--handler") == 0)
		return write(STDOUT_FILENO, kReply, sizeof(kReply) - 1) == sizeof(kReply) - 1 ? 0 : 1;
	sLegacyRange = argc == 2 && strcmp(argv[1], "--legacy-range") == 0;
	if (argc != 1 && !sLegacyRange)
		return 2;
	alarm(10);
	bool passed = CheckListener(argv[0]);
	printf("ROCK5_SERVICES_%s\n", passed ? "PASS" : "FAIL");
	fflush(stdout);
	_exit(passed ? 0 : 1);
}
