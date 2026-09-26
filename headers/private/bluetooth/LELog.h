/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_LOG_H_
#define _LE_LOG_H_

#include <SupportDefs.h>
#include <stddef.h>


namespace Bluetooth {

// Diagnostic logging for Bluetooth Low Energy connection, pairing and HID.
//
// Levels are cumulative. The level is read from the "level" entry of
// ~/config/settings/bluetooth/le_logging (driver settings syntax) and may be
// overridden with the BT_LE_LOG environment variable. The same file enables
// the kernel's LE traces (l2cap fixed channels, ACL link registration) when
// the level is LE_LOG_DEBUG or higher; the kernel re-reads it on every new LE
// link. Messages go to LELogFilePath() (rotated at 1 MiB, one ".old" copy)
// and, for errors and pairing steps, to syslog. Key material is never logged.
enum {
	LE_LOG_ERROR	= 0,	// failures only
	LE_LOG_INFO		= 1,	// each pairing / connection step (default)
	LE_LOG_DEBUG	= 2,	// HCI state changes, SMP/ATT PDU summaries
	LE_LOG_TRACE	= 3		// full PDU dumps (keys and randoms redacted)
};

int32		LELogLevel();
status_t	LESetLogLevel(int32 level);
	// Persists the level for userland and the kernel.
const char*	LELogFilePath();

void		LELog(int32 level, const char* component, const char* format, ...)
				__attribute__((format(printf, 3, 4)));
void		LELogHex(int32 level, const char* component, const char* label,
				const void* data, size_t length);

// Formats an address given in Bluetooth wire order as "AA:BB:CC:DD:EE:FF".
const char*	LEAddressString(const uint8 address[6], char buffer[18]);

} // namespace Bluetooth

#endif
