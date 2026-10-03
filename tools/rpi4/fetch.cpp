/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab tool: fetch a file from a TCP port of the lab host (tools/rpi4/
	push.py serves it) and write it to a file. The copy over the telnet
	login manages a few kilobytes a second; this is for everything larger.

	rpi4_fetch <IPv4 address> <port> <file> */


#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


int
main(int argc, char** argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s <IPv4 address> <port> <file>\n", argv[0]);
		return 1;
	}

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(atoi(argv[2]));
	if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
		fprintf(stderr, "bad address %s\n", argv[1]);
		return 1;
	}

	int connection = socket(AF_INET, SOCK_STREAM, 0);
	if (connection < 0
		|| connect(connection, (sockaddr*)&address, sizeof(address)) != 0) {
		fprintf(stderr, "connect: %s\n", strerror(errno));
		return 1;
	}

	int file = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (file < 0) {
		fprintf(stderr, "%s: %s\n", argv[3], strerror(errno));
		return 1;
	}

	static char buffer[256 * 1024];
	long long total = 0;
	while (true) {
		ssize_t count = read(connection, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			fprintf(stderr, "read: %s\n", strerror(errno));
			return 1;
		}
		if (count == 0)
			break;

		for (ssize_t written = 0; written < count;) {
			ssize_t result = write(file, buffer + written, count - written);
			if (result < 0) {
				fprintf(stderr, "write: %s\n", strerror(errno));
				return 1;
			}
			written += result;
		}
		total += count;
	}

	fsync(file);
	close(file);
	printf("%lld bytes\n", total);
	return 0;
}
