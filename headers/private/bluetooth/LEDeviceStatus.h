/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_DEVICE_STATUS_H_
#define _LE_DEVICE_STATUS_H_

#include <SupportDefs.h>

#include <vector>


namespace Bluetooth {

// Live state of a bonded LE device, published by the client that holds its
// link (the input_server mouse add-on) for status displays such as the
// Bluetooth Deskbar applet. Stored in ~/config/settings/bluetooth/le_status;
// entries written before the current boot are ignored when read.
struct LEDeviceStatus {
	uint8		localAddress[6];
	uint8		address[6];
		// Bluetooth wire order
	uint8		addressType;
	bool		connected;
	int32		battery;
		// Percent, or -1 when the device reports no battery level
	bigtime_t	updated;
		// system_time() of the last change
};

status_t	PublishLEDeviceStatus(const LEDeviceStatus& status);
status_t	ReadLEDeviceStatus(std::vector<LEDeviceStatus>& devices);
status_t	LEDeviceStatusPath(char* path, size_t capacity);

} // namespace Bluetooth

#endif
