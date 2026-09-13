/*
 * Bounded, checked memory streams for the private lab network.
 * Distributed under the terms of the MIT License.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


static const uint64_t kMaximumBytes = 512ULL * 1024 * 1024;


static bool
ReadExactly(int fd, void* buffer, size_t size)
{
	uint8_t* next = (uint8_t*)buffer;
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
	const uint8_t* next = (const uint8_t*)buffer;
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
ParseNumber(const char* text, uint64_t maximum, uint64_t* value)
{
	if (text[0] == 0 || strspn(text, "0123456789") != strlen(text))
		return false;
	char* end;
	errno = 0;
	*value = strtoull(text, &end, 10);
	return errno == 0 && *end == 0 && *value <= maximum;
}


static void
StoreBigEndian(uint8_t* buffer, uint64_t value)
{
	for (unsigned i = 0; i < 8; i++)
		buffer[i] = value >> (56 - 8 * i);
}


static void
FillPattern(uint8_t* buffer, size_t size, uint64_t offset, uint64_t seed)
{
	// Each 64-bit word depends on its absolute stream position and seed.
	// Explicit little-endian bytes make the fixture independent of host order.
	for (size_t i = 0; i < size; i += 8) {
		uint64_t word = seed + (offset + i) / 8 + 0x9e3779b97f4a7c15ULL;
		word = (word ^ (word >> 30)) * 0xbf58476d1ce4e5b9ULL;
		word = (word ^ (word >> 27)) * 0x94d049bb133111ebULL;
		word ^= word >> 31;
		for (unsigned j = 0; j < 8 && i + j < size; j++)
			buffer[i + j] = word >> (8 * j);
	}
}


static bool
Transfer(int input, int output, bool receive, bool peer, const char* token,
	uint64_t bytes, uint64_t seed)
{
	uint8_t expectedHeader[82];
	memcpy(expectedHeader, token, 64);
	expectedHeader[64] = '\n';
	StoreBigEndian(expectedHeader + 65, bytes);
	StoreBigEndian(expectedHeader + 73, seed);
	expectedHeader[81] = (receive != peer) ? 'R' : 'S';
	uint8_t header[sizeof(expectedHeader)];
	char acknowledgement;
	if (peer) {
		if (!ReadExactly(input, header, sizeof(header)))
			return false;
		if (memcmp(header, expectedHeader, sizeof(header)) != 0) {
			errno = EINVAL;
			return false;
		}
		if (!WriteExactly(output, "R", 1))
			return false;
	} else {
		if (!WriteExactly(output, expectedHeader, sizeof(expectedHeader))
			|| !ReadExactly(input, &acknowledgement, 1))
			return false;
		if (acknowledgement != 'R') {
			errno = EINVAL;
			return false;
		}
	}

	struct timespec start, end;
	if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
		return false;
	uint8_t expected[65536], actual[sizeof(expected)];
	for (uint64_t offset = 0; offset < bytes;) {
		size_t count = bytes - offset < sizeof(expected)
			? bytes - offset : sizeof(expected);
		FillPattern(expected, count, offset, seed);
		if (receive) {
			if (!ReadExactly(input, actual, count))
				return false;
			if (memcmp(actual, expected, count) != 0) {
				fprintf(stderr, "ROCK5_NETWORK_CORRUPTION block_offset=%llu\n",
					(unsigned long long)offset);
				errno = EIO;
				return false;
			}
		} else if (!WriteExactly(output, expected, count))
			return false;
		offset += count;
	}

	// A sender succeeds only after the receiver has checked every byte.
	if (receive) {
		if (!WriteExactly(output, "P", 1))
			return false;
	} else {
		if (!ReadExactly(input, &acknowledgement, 1))
			return false;
		if (acknowledgement != 'P') {
			errno = EIO;
			return false;
		}
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end) != 0)
		return false;
	double seconds = end.tv_sec - start.tv_sec
		+ (end.tv_nsec - start.tv_nsec) / 1000000000.0;
	if (seconds <= 0) {
		errno = EIO;
		return false;
	}
	fprintf(stderr, "ROCK5_NETWORK_PASS direction=%s bytes=%llu seed=%llu "
		"seconds=%.6f mbps=%.3f\n", receive ? "receive" : "send",
		(unsigned long long)bytes, (unsigned long long)seed, seconds,
		bytes * 8.0 / seconds / 1000000.0);
	return true;
}


int
main(int argc, char** argv)
{
	bool peer = argc == 5;
	bool receive = argc > 1 && (strcmp(argv[1], "receive") == 0
		|| strcmp(argv[1], "peer-receive") == 0);
	if ((!peer && argc != 8)
		|| (peer && strcmp(argv[1], "peer-receive") != 0
			&& strcmp(argv[1], "peer-send") != 0)
		|| (!peer && strcmp(argv[1], "receive") != 0
			&& strcmp(argv[1], "send") != 0)) {
		fprintf(stderr, "Usage: %s receive|send IPv4 PORT TOKEN64 BYTES SEED SOURCE_IPv4\n"
			"       %s peer-receive|peer-send TOKEN64 BYTES SEED (stdin/stdout)\n",
			argv[0], argv[0]);
		return 2;
	}
	const char* token = argv[peer ? 2 : 4];
	uint64_t bytes, seed;
	if (strlen(token) != 64 || strspn(token, "0123456789abcdef") != 64
		|| !ParseNumber(argv[peer ? 3 : 5], kMaximumBytes, &bytes) || bytes == 0
		|| !ParseNumber(argv[peer ? 4 : 6], UINT64_MAX, &seed))
		return 2;
	signal(SIGPIPE, SIG_IGN);
	alarm(180);
	int input = STDIN_FILENO, output = STDOUT_FILENO;
	if (!peer) {
		struct sockaddr_in address, source;
		memset(&address, 0, sizeof(address));
		memset(&source, 0, sizeof(source));
		address.sin_family = source.sin_family = AF_INET;
		uint64_t port;
		if (!ParseNumber(argv[3], 65535, &port) || port == 0
			|| inet_pton(AF_INET, argv[2], &address.sin_addr) != 1
			|| inet_pton(AF_INET, argv[7], &source.sin_addr) != 1)
			return 2;
		address.sin_port = htons(port);
		input = output = socket(AF_INET, SOCK_STREAM, 0);
		struct timeval timeout = {30, 0};
		if (input < 0
			|| setsockopt(input, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0
			|| setsockopt(input, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0
			|| bind(input, (struct sockaddr*)&source, sizeof(source)) != 0
			|| connect(input, (struct sockaddr*)&address, sizeof(address)) != 0) {
			perror("network probe connect");
			if (input >= 0)
				close(input);
			return 1;
		}
		fprintf(stderr, "ROCK5_NETWORK_PATH source=%s peer=%s port=%llu\n",
			argv[7], argv[2], (unsigned long long)port);
	}
	bool passed = Transfer(input, output, receive, peer, token, bytes, seed);
	if (!passed)
		perror("network probe");
	if (!peer)
		close(input);
	alarm(0);
	return passed ? 0 : 1;
}
