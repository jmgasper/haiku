#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "RNDISPacket.h"


static void
put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
	for (unsigned int i = 0; i < 4; i++)
		bytes[offset + i] = value >> (i * 8);
}


static std::vector<uint8_t>
packet(size_t frameSize, size_t dataStart = 44, size_t padding = 0)
{
	std::vector<uint8_t> bytes(dataStart + frameSize + padding, 0);
	put32(bytes, 0, 1);
	put32(bytes, 4, bytes.size());
	put32(bytes, 8, dataStart - 8);
	put32(bytes, 12, frameSize);
	for (size_t i = 0; i < frameSize; i++)
		bytes[dataStart + i] = (i ^ (i >> 8)) & 0xff;
	return bytes;
}


static void
expect_invalid(const std::vector<uint8_t>& bytes, size_t capacity = 1514)
{
	std::vector<uint8_t> guarded(capacity + 2, 0xa5);
	size_t consumed = 99;
	size_t copied = 99;
	assert(!rndis_extract_packet(bytes.data(), bytes.size(), guarded.data() + 1,
		capacity, consumed, copied));
	assert(consumed == 0 && copied == 0);
	for (size_t i = 0; i < guarded.size(); i++)
		assert(guarded[i] == 0xa5);
}


int
main()
{
	// MTU 1500 needs 1514 bytes at the Ethernet interface, without touching
	// either guard byte. The previous 1500-byte receive buffer is too small.
	std::vector<uint8_t> full = packet(1514);
	uint8_t frame[1516];
	memset(frame, 0xa5, sizeof(frame));
	size_t consumed;
	size_t copied;
	assert(rndis_extract_packet(full.data(), full.size(), frame + 1, 1514,
		consumed, copied));
	assert(consumed == full.size() && copied == 1514);
	assert(memcmp(frame + 1, full.data() + 44, 1514) == 0);
	assert(frame[0] == 0xa5 && frame[1515] == 0xa5);
	expect_invalid(full, 1500);
	expect_invalid(full, 0);

	// The USB buffer need not be aligned for uint32 loads.
	std::vector<uint8_t> unaligned(full.size() + 1);
	memcpy(unaligned.data() + 1, full.data(), full.size());
	assert(rndis_extract_packet(unaligned.data() + 1, full.size(), frame, 1514,
		consumed, copied));
	assert(memcmp(frame, full.data() + 44, 1514) == 0);

	// Padded concatenated messages consume their declared lengths, including
	// padding, while each call returns exactly one Ethernet frame.
	std::vector<uint8_t> first = packet(61, 48, 3);
	std::vector<uint8_t> second = packet(1514);
	std::vector<uint8_t> batch(first);
	batch.insert(batch.end(), second.begin(), second.end());
	assert(rndis_extract_packet(batch.data(), batch.size(), frame, 1514,
		consumed, copied));
	assert(consumed == first.size() && copied == 61);
	assert(memcmp(frame, first.data() + 48, copied) == 0);
	size_t offset = consumed;
	assert(rndis_extract_packet(batch.data() + offset, batch.size() - offset,
		frame, 1514, consumed, copied));
	assert(offset + consumed == batch.size() && copied == 1514);
	assert(memcmp(frame, second.data() + 44, copied) == 0);

	// Each short allocation must be rejected before reading a complete header.
	for (size_t length = 0; length < 44; length++)
		expect_invalid(std::vector<uint8_t>(full.begin(), full.begin() + length));
	expect_invalid(std::vector<uint8_t>(full.begin(), full.end() - 1));

	const struct {
		size_t field;
		uint32_t value;
	} invalid[] = {
		{0, 2}, {4, 0}, {4, 43}, {4, UINT32_MAX},
		{8, 0}, {8, 35}, {8, 1550}, {8, UINT32_MAX},
		{12, 0}, {12, 13}, {12, 1515}, {12, UINT32_MAX}
	};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		std::vector<uint8_t> bytes(full);
		put32(bytes, invalid[i].field, invalid[i].value);
		expect_invalid(bytes);
	}

	// An incomplete packet cannot borrow data from the next message.
	std::vector<uint8_t> crosses(first);
	put32(crosses, 12, 80);
	crosses.insert(crosses.end(), second.begin(), second.end());
	expect_invalid(crosses);
	assert(!rndis_extract_packet(NULL, 44, frame, sizeof(frame), consumed, copied));
	assert(!rndis_extract_packet(full.data(), full.size(), NULL, 1514,
		consumed, copied));
	puts("RNDIS frame extraction checks passed");
	return 0;
}
