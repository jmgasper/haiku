/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef L2CAP_LE_H
#define L2CAP_LE_H

#include <btCoreData.h>
#include <net_buffer.h>


// 0 quiet, 2 link/channel traces, 3 packet dumps (SMP secrets redacted).
// Set with "level N" in ~/config/settings/bluetooth/le_logging; re-read
// whenever an LE fixed channel is opened.
extern int32 gL2capLELogLevel;

#define L2CAP_LE_TRACE(format, args...) \
	do { if (gL2capLELogLevel >= 2) dprintf("l2cap-le: " format, ##args); } \
		while (false)

void l2cap_le_reload_settings();
void l2cap_le_dump(const char* label, HciConnection* connection, uint16 cid,
	net_buffer* buffer);

// Consumes the buffer.
status_t l2cap_le_handle_signaling(HciConnection* connection,
	net_buffer* buffer);
// Returns true when the PDU was a request answered here; the caller then frees
// the buffer. Indications are confirmed and still delivered.
bool l2cap_le_answer_att(HciConnection* connection, net_buffer* buffer);

#endif
