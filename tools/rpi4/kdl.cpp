/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_kdl [message]
// Enters the kernel debugger, which is then driven over the serial console
// (the NanoKVM's keyboard does not reach it): for looking at threads that
// hang in the kernel. "continue" there resumes the system.


#include <syscalls.h>


int
main(int argc, char** argv)
{
	_kern_kernel_debugger(argc > 1 ? argv[1] : "rpi4_kdl");
	return 0;
}
