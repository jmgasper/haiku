/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Regression check for deferring clipping while a window is constructed.
// Run on an otherwise unobstructed desktop at scale 1.

#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>


static const rgb_color kRed = {220, 30, 40, 255};
static const rgb_color kBlue = {20, 70, 230, 255};
static const rgb_color kGreen = {30, 200, 50, 255};
static const rgb_color kYellow = {240, 210, 20, 255};


static BView*
view(BRect frame, const char* name, rgb_color color)
{
	BView* result = new BView(frame, name, B_FOLLOW_NONE, B_WILL_DRAW);
	result->SetViewColor(color);
	return result;
}


static bool
pixel(const BBitmap& bitmap, int x, int y, rgb_color color)
{
	const uint8* p = (const uint8*)bitmap.Bits()
		+ y * bitmap.BytesPerRow() + x * 4;
	return p[0] == color.blue && p[1] == color.green && p[2] == color.red;
}


static bool
check_window(BWindow* window, bool changed, const char* label)
{
	BScreen screen(window);
	BBitmap bitmap(window->Bounds(), B_RGB32);
	BRect frame = window->Frame();
	bigtime_t deadline = system_time() + 3000000;
	do {
		if (screen.ReadBitmap(&bitmap, false, &frame) == B_OK
			&& pixel(bitmap, 5, 5, kRed)
			&& pixel(bitmap, 25, 25, changed ? kRed : kBlue)
			&& pixel(bitmap, 55, 25, changed ? kGreen : kRed)
			&& pixel(bitmap, 85, 25, changed ? kYellow : kRed)) {
			printf("PASS %s\n", label);
			return true;
		}
		snooze(20000);
	} while (system_time() < deadline);
	fprintf(stderr, "FAIL %s\n", label);
	return false;
}


int
main()
{
	BApplication application("application/x-vnd.airOS-view-visibility-test");
	BWindow* window = new BWindow(BRect(100, 140, 279, 269),
		"View visibility test", B_TITLED_WINDOW, B_NOT_ZOOMABLE);
	BView* parent = view(window->Bounds(), "parent", kRed);
	BView* blue = view(BRect(20, 20, 39, 39), "blue", kBlue);
	BView* green = view(BRect(50, 20, 69, 39), "green", kGreen);
	green->Hide();
	parent->AddChild(blue);
	parent->AddChild(green);
	window->AddChild(parent);
	window->Show();
	bool ok = check_window(window, false, "first show, including hidden child");

	window->Lock();
	window->Hide();
	blue->Hide();
	green->Show();
	parent->AddChild(view(BRect(80, 20, 99, 39), "yellow", kYellow));
	window->ResizeTo(199, 149);
	parent->ResizeTo(199, 149);
	window->Show();
	window->Unlock();
	ok &= check_window(window, true, "show after hidden changes and resize");
	window->Lock();
	window->Minimize(true);
	window->Minimize(false);
	window->Unlock();
	ok &= check_window(window, true, "restore minimized window");
	window->Lock();
	window->Quit();

	// A BBitmap's server window is never shown, but must stay drawable.
	BBitmap bitmap(BRect(0, 0, 31, 31), B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
	if (bitmap.InitCheck() != B_OK || !bitmap.Lock())
		return 1;
	BView* canvas = view(bitmap.Bounds(), "offscreen", kRed);
	bitmap.AddChild(canvas);
	canvas->SetHighColor(kBlue);
	canvas->FillRect(canvas->Bounds());
	canvas->Sync();
	bool offscreen = pixel(bitmap, 10, 10, kBlue);
	bitmap.Unlock();
	printf("%s offscreen drawing without Show\n", offscreen ? "PASS" : "FAIL");
	return ok && offscreen ? 0 : 1;
}
