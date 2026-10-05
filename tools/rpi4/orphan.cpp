/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_orphan [count]
// Loads programs with load_image() and exits without resuming them, as a
// launcher does that is killed between loading a helper and starting it.
// The kernel used to leave such teams suspended for good; it now ends them
// with their parent. Prints the teams it loaded; `ps` afterwards shows
// whether any "sleep 31415" is left.


#include <stdio.h>
#include <stdlib.h>

#include <image.h>
#include <OS.h>


int
main(int argc, char** argv)
{
	int count = argc > 1 ? atoi(argv[1]) : 3;

	for (int i = 0; i < count; i++) {
		const char* args[] = { "/bin/sleep", "31415", NULL };
		extern char** environ;
		thread_id thread = load_image(2, args, (const char**)environ);
		printf("loaded team %" B_PRId32 "\n", thread);
	}

	fflush(stdout);
	return 0;
}
