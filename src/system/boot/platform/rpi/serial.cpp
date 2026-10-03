/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "serial.h"

#include <string.h>

#include <new>

#include <arch/arm/arch_uart_pl011.h>
#include <boot/kernel_args.h>
#include <boot/stage2.h>


static const uint32 kSerialBaudRate = 115200;

static DebugUART* sUART = NULL;


void
serial_init()
{
	const uart_info& uart = gKernelArgs.arch_args.uart;
	if (strcmp(uart.kind, UART_KIND_PL011) != 0)
		return;

	static char buffer[sizeof(ArchUARTPL011)];
	sUART = new(buffer) ArchUARTPL011(uart.regs.start, uart.clock);
	sUART->InitEarly();
	// Set the port up ourselves: writing to a PL011 that the firmware left
	// disabled would wait forever for room in the FIFO.
	sUART->InitPort(kSerialBaudRate);
}


void
serial_puts(const char* string, size_t size)
{
	if (sUART == NULL)
		return;

	while (size-- != 0) {
		char c = *string++;
		if (c == '\n')
			sUART->PutChar('\r');
		sUART->PutChar(c);
	}
}


int
serial_getc(bool wait)
{
	if (sUART == NULL)
		return -1;
	return sUART->GetChar(wait);
}


void
serial_flush()
{
	// The kernel sets the port up again, which garbles what is still in
	// the FIFO.
	if (sUART != NULL)
		sUART->FlushTx();
}
