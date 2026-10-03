/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SMP_H
#define SMP_H


#include <SupportDefs.h>


class Menu;

void smp_init();
void smp_init_other_cpus();
	// allocates their kernel stacks
void smp_boot_other_cpus(addr_t kernelEntry);
void smp_add_safemode_menus(Menu* menu);


#endif	/* SMP_H */
