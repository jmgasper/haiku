/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>


int
main(int argc, char** argv)
{
	bool expectError = argc == 2
		&& strcmp(argv[1], "--expect-partial-error") == 0;
	if (argc != 1 && !expectError)
		return 2;
	int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
		perror("open master");
		return 2;
	}
	const char* name = ptsname(master);
	int slave = name != NULL ? open(name, O_RDWR | O_NOCTTY | O_NONBLOCK) : -1;
	struct termios attributes;
	if (slave < 0 || tcgetattr(slave, &attributes) != 0) {
		perror("open slave");
		return 2;
	}
	cfmakeraw(&attributes);
	if (tcsetattr(slave, TCSANOW, &attributes) != 0) {
		perror("raw mode");
		return 2;
	}
	unsigned char source[65536];
	unsigned char received[sizeof(source)];
	for (size_t i = 0; i < sizeof(source); i++)
		source[i] = (unsigned char)((i * 37 + i / 251) & 255);
	bool pass = true;
	size_t offset = 0;
	unsigned rounds = 0;
	while (offset < sizeof(source) && rounds++ < 256) {
		errno = 0;
		ssize_t written = write(master, source + offset, sizeof(source) - offset);
		int writeError = errno;
		size_t count = 0;
		while (count < sizeof(received)) {
			ssize_t bytes = read(slave, received + count, sizeof(received) - count);
			if (bytes > 0)
				count += bytes;
			else if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			else {
				perror("read slave");
				return 2;
			}
		}
		bool prefix = count > 0 && count <= sizeof(source) - offset
			&& memcmp(received, source + offset, count) == 0;
		bool partialError = written < 0
			&& (writeError == EAGAIN || writeError == EWOULDBLOCK) && prefix;
		bool accounted = written > 0 && (size_t)written == count && prefix;
		printf("ROCK5_PTY_WRITE round=%u returned=%ld errno=%d received=%lu "
			"prefix=%d partial_error=%d accounted=%d\n", rounds, (long)written,
			writeError, (unsigned long)count, prefix, partialError, accounted);
		if (expectError) {
			pass = partialError;
			break;
		}
		if (!accounted) {
			pass = false;
			break;
		}
		offset += count;
	}
	if (!expectError && offset != sizeof(source))
		pass = false;
	close(slave);
	close(master);
	printf("ROCK5_PTY_RESULT expect_partial_error=%d bytes=%lu rounds=%u "
		"status=%s\n", expectError, (unsigned long)offset, rounds,
		pass ? "pass" : "fail");
	return pass ? 0 : 1;
}
