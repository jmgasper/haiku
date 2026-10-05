/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef DEBUG_H
#define DEBUG_H


#include <SupportDefs.h>


extern bool gDebugToScreen;
	// dprintf() output also goes to the frame buffer console

void debug_cleanup(void);


#endif	/* DEBUG_H */
