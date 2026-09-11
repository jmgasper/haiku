/*
	Distributed under the terms of the MIT license.
*/
#ifndef _USB_RNDIS_PACKET_H_
#define _USB_RNDIS_PACKET_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>


static const size_t kRNDISPacketHeaderSize = 44;


static inline uint32_t
rndis_read_le32(const uint8_t* bytes)
{
	return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8)
		| (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}


// Extract one Ethernet frame from a USB batch. Lengths are zero on failure,
// and an invalid message must not change any destination bytes.
static inline bool
rndis_extract_packet(const uint8_t* message, size_t available, uint8_t* frame,
	size_t capacity, size_t& messageLength, size_t& frameLength)
{
	messageLength = 0;
	frameLength = 0;
	if (message == NULL || available < kRNDISPacketHeaderSize)
		return false;

	const uint32_t type = rndis_read_le32(message);
	const uint32_t length = rndis_read_le32(message + 4);
	const uint32_t dataOffset = rndis_read_le32(message + 8);
	const uint32_t dataLength = rndis_read_le32(message + 12);
	if (type != 1 || length < kRNDISPacketHeaderSize || length > available)
		return false;

	// DataOffset is relative to its own field, eight bytes into the message.
	// Check by subtraction so corrupt 32-bit lengths cannot wrap around.
	// The frame must contain at least its 14-byte Ethernet header.
	if (dataOffset < kRNDISPacketHeaderSize - 8 || dataOffset > length - 8
		|| dataLength > length - 8 - dataOffset || dataLength < 14
		|| dataLength > capacity || frame == NULL) {
		return false;
	}

	memcpy(frame, message + 8 + dataOffset, dataLength);
	messageLength = length;
	frameLength = dataLength;
	return true;
}

#endif // _USB_RNDIS_PACKET_H_
