/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef LE_ADVERTISING_DATA_H
#define LE_ADVERTISING_DATA_H

#include <stddef.h>
#include <stdint.h>
#include <string>


struct LEAdvertisingData {
	std::string name;
	bool completeName;
	uint16_t appearance;
	bool advertisesHID;

	LEAdvertisingData()
		:
		completeName(false),
		appearance(0),
		advertisesHID(false)
	{
	}
};


// Parse bounded legacy advertising or scan-response data. A shortened name
// remains useful until a complete name arrives in a later report.
inline LEAdvertisingData
ParseLEAdvertisingData(const uint8_t* data, size_t size)
{
	LEAdvertisingData result;
	if (data == NULL)
		return result;
	for (size_t offset = 0; offset < size;) {
		uint8_t length = data[offset++];
		if (length == 0 || length > size - offset)
			break;
		uint8_t type = data[offset];
		const uint8_t* value = data + offset + 1;
		size_t valueSize = length - 1;
		if ((type == 0x08 || type == 0x09) && valueSize > 0
			&& (type == 0x09 || !result.completeName)) {
			result.name.assign((const char*)value, valueSize);
			result.completeName = type == 0x09;
		} else if (type == 0x19 && valueSize == 2) {
			result.appearance = value[0] | ((uint16_t)value[1] << 8);
		} else if (type == 0x02 || type == 0x03) {
			for (size_t i = 0; i + 1 < valueSize; i += 2) {
				if (value[i] == 0x12 && value[i + 1] == 0x18)
					result.advertisesHID = true;
			}
		} else if (type == 0x16 && valueSize >= 2
			&& value[0] == 0x12 && value[1] == 0x18) {
			result.advertisesHID = true;
		}
		offset += length;
	}
	return result;
}

#endif
