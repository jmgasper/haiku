/* Does BScreen::WaitForRetrace() wait for the display?
 *
 * This is the call every Haiku program makes to line its drawing up with the
 * screen. It only works if the accelerant hands out a semaphore that something
 * releases once per frame, so the proof is the rate: at 60 Hz the waits should
 * come back 60 times a second, about 16.7 ms apart, and evenly.
 *
 * usage: retracetest [seconds]
 */
#include <stdio.h>
#include <stdlib.h>

#include <Application.h>
#include <Screen.h>
#include <OS.h>


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	double seconds = argc > 1 ? atof(argv[1]) : 3.0;

	BApplication app("application/x-vnd.x399-retracetest");
	BScreen screen(B_MAIN_SCREEN_ID);

	display_mode mode;
	if (screen.GetMode(&mode) == B_OK) {
		printf("screen: %" B_PRIu16 "x%" B_PRIu16 "\n",
			mode.timing.h_display, mode.timing.v_display);
	}

	// The first one may have to start the machinery off, so it is not timed.
	status_t first = screen.WaitForRetrace(1000000);
	if (first != B_OK) {
		printf("WaitForRetrace: %s\n", strerror(first));
		printf("the accelerant does not offer a retrace semaphore\n");
		return 1;
	}

	bigtime_t start = system_time();
	bigtime_t end = start + (bigtime_t)(seconds * 1000000);
	bigtime_t previous = system_time();
	bigtime_t shortest = 0, longest = 0, total = 0;
	int64 count = 0;
	int64 timeouts = 0;

	while (system_time() < end) {
		if (screen.WaitForRetrace(500000) != B_OK) {
			timeouts++;
			previous = system_time();
			continue;
		}
		bigtime_t now = system_time();
		bigtime_t gap = now - previous;
		previous = now;

		if (count == 0 || gap < shortest)
			shortest = gap;
		if (gap > longest)
			longest = gap;
		total += gap;
		count++;
	}

	if (count < 2) {
		printf("only %" B_PRId64 " waits came back\n", count);
		return 1;
	}

	double average = (double)total / count;
	printf("%" B_PRId64 " waits: one every %.2f ms (%.1f a second),"
		" shortest %.2f, longest %.2f, %" B_PRId64 " timed out\n",
		count, average / 1000.0, 1000000.0 / average,
		shortest / 1000.0, longest / 1000.0, timeouts);
	return 0;
}
