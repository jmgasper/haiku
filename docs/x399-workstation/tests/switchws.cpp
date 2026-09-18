/* Switch workspaces from outside, the way the Deskbar does.
 *
 * gltest can switch its own, but that is a program asking the window system to
 * take its window off the screen from inside its drawing loop, which is not
 * what anyone does. This does it from another program.
 *
 * usage: switchws [count] [seconds between]
 */
#include <stdio.h>
#include <stdlib.h>

#include <Application.h>
#include <InterfaceDefs.h>
#include <OS.h>

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	int count = argc > 1 ? atoi(argv[1]) : 6;
	double gap = argc > 2 ? atof(argv[2]) : 1.5;

	BApplication app("application/x-vnd.x399-switchws");

	for (int i = 0; i < count; i++) {
		int32 want = (i % 2) == 0 ? 1 : 0;
		activate_workspace(want);
		printf("  workspace %" B_PRId32 "\n", want);
		snooze((bigtime_t)(gap * 1000000));
	}
	activate_workspace(0);
	return 0;
}
