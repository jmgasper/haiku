/*
 * dpmstest - put the monitors to sleep and wake them the way the screen
 * saver does, and report what the display layout looks like before and
 * after.
 *
 *   dpmstest state
 *   dpmstest off|on|standby|suspend
 *   dpmstest cycle <seconds asleep>
 *
 * Build: g++ -o dpmstest dpmstest.cpp -lbe
 */

#include <Application.h>
#include <Screen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static const char*
mode_name(uint32 mode)
{
	switch (mode) {
		case B_DPMS_ON: return "on";
		case B_DPMS_STAND_BY: return "standby";
		case B_DPMS_SUSPEND: return "suspend";
		case B_DPMS_OFF: return "off";
		default: return "?";
	}
}


static void
report(const char* when)
{
	BScreen screen;
	BRect frame = screen.Frame();
	printf("%s: dpms %s, capabilities 0x%" B_PRIx32 ", desktop %gx%g\n",
		when, mode_name(screen.DPMSState()), screen.DPMSCapabilites(),
		frame.Width() + 1, frame.Height() + 1);
	fflush(stdout);
}


static int
set(uint32 mode)
{
	BScreen screen;
	status_t status = screen.SetDPMS(mode);
	if (status != B_OK) {
		fprintf(stderr, "SetDPMS(%s): %s\n", mode_name(mode), strerror(status));
		return 1;
	}
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s state|off|on|standby|suspend|cycle <s>\n",
			argv[0]);
		return 2;
	}
	BApplication app("application/x-vnd.x399-dpmstest");

	const char* command = argv[1];
	if (strcmp(command, "state") == 0) {
		report("now");
		return 0;
	}
	if (strcmp(command, "on") == 0)
		return set(B_DPMS_ON);
	if (strcmp(command, "off") == 0)
		return set(B_DPMS_OFF);
	if (strcmp(command, "standby") == 0)
		return set(B_DPMS_STAND_BY);
	if (strcmp(command, "suspend") == 0)
		return set(B_DPMS_SUSPEND);
	if (strcmp(command, "cycle") == 0) {
		int seconds = argc > 2 ? atoi(argv[2]) : 60;
		report("before");
		if (set(B_DPMS_OFF) != 0)
			return 1;
		report("asleep");
		for (int i = 0; i < seconds; i += 10) {
			sleep(10);
			report("asleep");
		}
		int result = set(B_DPMS_ON);
		report("woken");
		sleep(5);
		report("5 s later");
		return result;
	}
	fprintf(stderr, "unknown command %s\n", command);
	return 2;
}
