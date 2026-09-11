/*
 * Bounded binary transfers for the private lab network.
 * Distributed under the terms of the MIT License.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>


static const uint64_t kMaximumBytes = 16 * 1024 * 1024;


static bool
ReadExactly(int fd, void* buffer, size_t size)
{
	char* next = (char*)buffer;
	while (size > 0) {
		ssize_t count = read(fd, next, size);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			return false;
		}
		next += count;
		size -= count;
	}
	return true;
}


static bool
WriteExactly(int fd, const void* buffer, size_t size)
{
	const char* next = (const char*)buffer;
	while (size > 0) {
		ssize_t count = write(fd, next, size);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			return false;
		}
		next += count;
		size -= count;
	}
	return true;
}


static bool
Transfer(int connection, bool receive, const char* path)
{
	uint64_t size = 0;
	uint8_t header[8];
	int file;
	if (receive) {
		if (!ReadExactly(connection, header, sizeof(header)))
			return false;
		for (unsigned i = 0; i < sizeof(header); i++)
			size = (size << 8) | header[i];
		if (size > kMaximumBytes) {
			errno = EFBIG;
			return false;
		}
		file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	} else {
		file = open(path, O_RDONLY);
		if (file < 0)
			return false;
		struct stat info;
		if (fstat(file, &info) != 0 || !S_ISREG(info.st_mode)
			|| info.st_size < 0 || (uint64_t)info.st_size > kMaximumBytes) {
			close(file);
			errno = EINVAL;
			return false;
		}
		size = info.st_size;
		for (unsigned i = 0; i < sizeof(header); i++)
			header[i] = size >> (56 - 8 * i);
		if (!WriteExactly(connection, header, sizeof(header))) {
			close(file);
			return false;
		}
	}
	if (file < 0)
		return false;
	char buffer[65536];
	uint64_t remaining = size;
	bool passed = true;
	while (remaining > 0) {
		size_t count = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		if (!ReadExactly(receive ? connection : file, buffer, count)
			|| !WriteExactly(receive ? file : connection, buffer, count)) {
			passed = false;
			break;
		}
		remaining -= count;
	}
	if (passed && receive)
		passed = fsync(file) == 0;
	int savedError = errno;
	if (close(file) < 0 && passed)
		return false;
	errno = savedError;
	if (passed)
		fprintf(stderr, "ROCK5_TRANSFER bytes=%llu direction=%s\n",
			(unsigned long long)size, receive ? "receive" : "send");
	return passed;
}


int
main(int argc, char** argv)
{
	if (argc != 6 || (strcmp(argv[1], "receive") != 0
			&& strcmp(argv[1], "send") != 0) || strlen(argv[4]) != 64
		|| strspn(argv[4], "0123456789abcdef") != 64) {
		fprintf(stderr, "Usage: %s receive|send IPv4 PORT TOKEN64 FILE\n", argv[0]);
		return 2;
	}
	char* end;
	errno = 0;
	unsigned long port = strtoul(argv[3], &end, 10);
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	if (errno != 0 || *end != 0 || port < 1 || port > 65535
		|| inet_pton(AF_INET, argv[2], &address.sin_addr) != 1)
		return 2;
	address.sin_port = htons(port);
	alarm(120);
	int connection = socket(AF_INET, SOCK_STREAM, 0);
	if (connection < 0) {
		perror("socket");
		return 1;
	}
	struct timeval timeout = {30, 0};
	bool passed = setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout)) == 0
		&& setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout,
			sizeof(timeout)) == 0
		&& connect(connection, (struct sockaddr*)&address, sizeof(address)) == 0
		&& WriteExactly(connection, argv[4], 64)
		&& WriteExactly(connection, "\n", 1)
		&& Transfer(connection, strcmp(argv[1], "receive") == 0, argv[5]);
	if (!passed)
		perror("file transfer");
	close(connection);
	alarm(0);
	return passed ? 0 : 1;
}
