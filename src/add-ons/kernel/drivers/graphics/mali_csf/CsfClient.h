/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_CLIENT_H
#define MALI_CSF_CLIENT_H

#include "CsfBuffer.h"

namespace MaliCSF {
status_t OpenClient(bool writable, void** cookie);
status_t AccessClient(void* cookie);
void CloseClient(void* cookie);
void FreeClient(void* cookie);
status_t ControlClient(void* cookie, uint32 op, void* buffer, size_t length);
}
#endif
