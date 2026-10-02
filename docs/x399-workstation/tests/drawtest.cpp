/* How fast is ordinary drawing on the screen?
 *
 * The accelerant offers no 2D hooks, so app_server draws every pixel with the
 * processor, into a frame buffer that lives in video memory across the PCIe
 * bus. Writes that way are slow - about 0.8 GB/s by measurement - so the
 * question is whether it shows.
 *
 * Each test is run twice: once into a window on the screen, and once into an
 * off-screen bitmap in ordinary memory. The difference between the two is what
 * the bus is costing, and what a 2D engine would be worth.
 *
 * usage: drawtest [seconds per test]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Bitmap.h>
#include <View.h>
#include <Window.h>
#include <OS.h>

static const int kWidth = 1000;
static const int kHeight = 700;
static double sSeconds = 1.5;


// Run `work` against `view` until the time is up, and return operations a
// second. Sync() each round so the cost cannot hide in a queue.
static double
Measure(BView *view, BWindow *window, BBitmap *bitmap, void (*work)(BView*, int))
{
	if (window != NULL)
		window->Lock();
	else
		bitmap->Lock();

	bigtime_t end = system_time() + (bigtime_t)(sSeconds * 1000000);
	int64 ops = 0;
	int round = 0;
	while (system_time() < end) {
		work(view, round++);
		view->Sync();
		ops++;
	}

	if (window != NULL)
		window->Unlock();
	else
		bitmap->Unlock();

	return ops / sSeconds;
}


static void
FillWholeArea(BView *view, int round)
{
	rgb_color color = {(uint8)(round * 7), (uint8)(round * 13), 200, 255};
	view->SetHighColor(color);
	view->FillRect(BRect(0, 0, kWidth - 1, kHeight - 1));
}


static void
FillManySmall(BView *view, int round)
{
	for (int i = 0; i < 200; i++) {
		rgb_color color = {(uint8)(i * 3), (uint8)(round * 5), 128, 255};
		view->SetHighColor(color);
		int x = (i * 37) % (kWidth - 60);
		int y = (i * 53) % (kHeight - 60);
		view->FillRect(BRect(x, y, x + 60, y + 60));
	}
}


// What dragging a window over another one costs: a large copy inside the
// frame buffer, which on the screen means reading video memory back over the
// bus and writing it again.
static void
ScrollArea(BView *view, int round)
{
	BRect source(0, 20, kWidth - 1, kHeight - 1);
	view->CopyBits(source, source.OffsetByCopy(0, -20));
}


static void
DrawText(BView *view, int round)
{
	view->SetHighColor(0, 0, 0);
	for (int i = 0; i < 40; i++) {
		view->DrawString("The quick brown fox jumps over the lazy dog",
			BPoint(10, 20 + i * 16));
	}
}


struct Test {
	const char *name;
	void (*work)(BView*, int);
	double bytes;		// touched per operation, for a rate
};


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 1)
		sSeconds = atof(argv[1]);

	BApplication app("application/x-vnd.x399-drawtest");

	BWindow *window = new BWindow(BRect(60, 60, 60 + kWidth, 60 + kHeight),
		"drawtest", B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
	BView *onScreen = new BView(window->Bounds(), "v", B_FOLLOW_ALL_SIDES,
		B_WILL_DRAW);
	window->AddChild(onScreen);
	window->Show();
	snooze(500000);

	BBitmap *bitmap = new BBitmap(BRect(0, 0, kWidth - 1, kHeight - 1),
		B_RGB32, true);
	BView *offScreen = new BView(bitmap->Bounds(), "b", B_FOLLOW_ALL_SIDES,
		B_WILL_DRAW);
	bitmap->AddChild(offScreen);

	const double area = (double)kWidth * kHeight * 4;
	Test tests[] = {
		{ "fill the whole window", FillWholeArea, area },
		{ "200 small rectangles", FillManySmall, 200.0 * 61 * 61 * 4 },
		{ "scroll it up 20 rows", ScrollArea, area * 2 },
		{ "40 lines of text", DrawText, 0 },
	};

	printf("%-24s %12s %12s   %s\n", "", "on screen", "in memory", "ratio");
	for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		double screen = Measure(onScreen, window, NULL, tests[i].work);
		double memory = Measure(offScreen, NULL, bitmap, tests[i].work);

		printf("%-24s %9.1f /s %9.1f /s   %5.2fx", tests[i].name, screen,
			memory, memory > 0 ? screen / memory : 0);
		if (tests[i].bytes > 0) {
			printf("   (%.2f GB/s against %.2f)",
				screen * tests[i].bytes / 1e9, memory * tests[i].bytes / 1e9);
		}
		printf("\n");
	}

	if (window->Lock())
		window->Quit();
	return 0;
}
