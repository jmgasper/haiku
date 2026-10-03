/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SERIAL_H
#define SERIAL_H


#include <SupportDefs.h>


void serial_init();
	// uses gKernelArgs.arch_args.uart, which fdt_init() fills in
void serial_puts(const char* string, size_t size);
void serial_flush();
int serial_getc(bool wait);
	// -1 if there is no character (or no UART)


#endif	/* SERIAL_H */
