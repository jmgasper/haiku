/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CONSOLE_H
#define CONSOLE_H


#include <boot/platform/generic/text_console.h>


status_t console_init(void);
void console_write_debug(const char* buffer, size_t length);
	// debug output to the frame buffer, if there is one


#endif	/* CONSOLE_H */
