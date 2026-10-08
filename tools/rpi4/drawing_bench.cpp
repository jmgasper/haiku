/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Deterministic offscreen drawing and per-client app_server CPU measurements.

#include <Application.h>
#include <Bitmap.h>
#include <Font.h>
#include <OS.h>
#include <Picture.h>
#include <Region.h>
#include <Shape.h>
#include <View.h>
#include <Window.h>
#include <image.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void
require(bool value, const char* description)
{
	if (!value) {
		fprintf(stderr, "DRAWING_FAIL %s\n", description);
		exit(1);
	}
}


static team_id
find_app_server()
{
	int32 cookie = 0;
	team_info team;
	while (get_next_team_info(&cookie, &team) == B_OK) {
		if (strstr(team.args, "app_server") == NULL)
			continue;
		int32 imageCookie = 0;
		image_info image;
		while (get_next_image_info(team.team, &imageCookie, &image) == B_OK) {
			const char* leaf = strrchr(image.name, '/');
			if (image.type == B_APP_IMAGE && leaf != NULL
				&& strncmp(leaf + 1, "app_server", 10) == 0) {
				printf("APPSERVER team=%" B_PRId32 " path=%s\n", team.team,
					image.name);
				return team.team;
			}
		}
	}
	return B_NAME_NOT_FOUND;
}


static thread_id
find_drawing_thread(team_id server, team_id client, const char* title)
{
	char prefix[64];
	snprintf(prefix, sizeof(prefix), "w:%" B_PRId32 ":%s", client, title);
	int32 cookie = 0;
	thread_info info;
	while (get_next_thread_info(server, &cookie, &info) == B_OK) {
		if (strncmp(info.name, prefix, strlen(prefix)) == 0)
			return info.thread;
	}
	return B_NAME_NOT_FOUND;
}


static bigtime_t
cpu_time(thread_id thread)
{
	thread_info info;
	require(get_thread_info(thread, &info) == B_OK, "read thread CPU time");
	return info.user_time + info.kernel_time;
}


static void
draw_scene(BView& view, int mode)
{
	view.PushState();
	view.SetDrawingMode(B_OP_COPY);
	view.SetHighColor(19, 29, 43, 73);
	view.FillRect(BRect(0, 0, 639, 479));
	if (mode == 2) {
		BRegion clip;
		clip.Include(BRect(3, 7, 137, 473));
		clip.Include(BRect(151, 13, 319, 467));
		clip.Include(BRect(337, 3, 629, 479));
		view.ConstrainClippingRegion(&clip);
	}

	view.SetHighColor(31, 44, 62, 127);
	view.FillRoundRect(BRect(10.25, 12.5, 629.75, 261.25), 9.5, 9.5);
	view.FillRoundRect(BRect(10.5, 272.25, 395.75, 469.5), 9, 9);
	view.FillRoundRect(BRect(403.25, 272.5, 629.5, 469.75), 9, 9);
	view.SetHighColor(103, 137, 171, 33);
	view.SetPenSize(1.5);
	view.StrokeRoundRect(BRect(10.25, 12.5, 629.75, 261.25), 9.5, 9.5);
	for (int line = 0; line < 11; line++) {
		float y = 72.125f + line * 13.25f;
		view.StrokeLine(BPoint(24.25, y), BPoint(615.75, y));
	}

	BShape area;
	area.MoveTo(BPoint(24.25, 239.75));
	area.LineTo(BPoint(24.25, 196.125));
	for (int i = 0; i < 12; i++) {
		float x = 24.25f + i * 48.75f;
		float y = 98.375f + ((i * 37) % 121);
		BPoint controls[] = {BPoint(x + 16.25f, y),
			BPoint(x + 32.5f, y), BPoint(x + 48.75f, y)};
		area.BezierTo(controls);
	}
	area.LineTo(BPoint(609.25, 239.75));
	area.Close();
	view.MovePenTo(B_ORIGIN);
	view.SetHighColor(37, 137, 113, 0);
	view.FillShape(&area);
	view.MovePenTo(B_ORIGIN);
	view.SetHighColor(97, 231, 193, 255);
	view.StrokeShape(&area);

	for (int i = 0; i < 23; i++) {
		float x = 20.125f + i * 26.25f;
		view.SetHighColor((i * 17) & 255, (i * 37) & 255,
			(i * 71) & 255, i * 11);
		view.FillEllipse(BRect(x, 315.25, x + 17.5f, 347.875));
	}

	if (mode != 1) {
		BFont font(be_plain_font);
		font.SetSize(18);
		view.SetFont(&font);
		view.SetHighColor(231, 239, 247);
		view.DrawString("Processor  12.5%   Network  128 KiB/s", BPoint(25, 44));
		font.SetSize(11);
		view.SetFont(&font);
		view.DrawString("Clipped curves, fractional edges, and opaque copy alpha",
			BPoint(25, 385));
		font.SetSize(40);
		view.SetFont(&font);
		view.DrawString("42.7 C", BPoint(421, 436));
	}
	view.PopState();
}


static uint64
hash_pixels(const BBitmap& bitmap)
{
	uint64 hash = 1469598103934665603ULL;
	const uint8* bits = (const uint8*)bitmap.Bits();
	for (int32 y = 0; y <= bitmap.Bounds().IntegerHeight(); y++) {
		for (int32 x = 0; x < 4 * (bitmap.Bounds().IntegerWidth() + 1); x++) {
			hash ^= bits[y * bitmap.BytesPerRow() + x];
			hash *= 1099511628211ULL;
		}
	}
	return hash;
}


int
main(int argc, char** argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 400;
	if (frames < 1 || frames > 10000) {
		fprintf(stderr, "usage: %s [frames 1..10000]\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	BApplication application("application/x-vnd.airOS-drawing-bench");
	team_id server = find_app_server();
	require(server >= B_OK, "locate app_server image");
	const int sizes[][2] = {{16, 16}, {31, 19}, {64, 48}, {127, 97},
		{640, 480}, {1281, 721}};
	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		int width = sizes[s][0], height = sizes[s][1];
		BBitmap bitmap(BRect(0, 0, width - 1, height - 1),
			B_BITMAP_ACCEPTS_VIEWS, B_RGBA32);
		require(bitmap.InitCheck() == B_OK && bitmap.Lock(), "create bitmap");
		BView* view = new BView(bitmap.Bounds(), "drawing bench", B_FOLLOW_NONE, 0);
		bitmap.AddChild(view);
		char title[32];
		snprintf(title, sizeof(title), "drawing-%zu", s);
		view->Window()->SetTitle(title);
		view->SetScale(std::min(width / 640.f, height / 480.f));
		view->Sync();
		thread_id drawing = find_drawing_thread(server, application.Team(), title);
		require(drawing >= B_OK, "locate private drawing thread");
		for (int mode = 0; mode < 4; mode++) {
			memset(bitmap.Bits(), 0xa5, bitmap.BitsLength());
			BPicture picture;
			if (mode == 3) {
				view->BeginPicture(&picture);
				draw_scene(*view, 0);
				require(view->EndPicture() == &picture, "record drawing");
			}
			int count = width >= 640 ? frames : 1;
			for (int warmup = 0; warmup < 8; warmup++) {
				if (mode == 3)
					view->DrawPicture(&picture);
				else
					draw_scene(*view, mode);
				view->Sync();
			}
			bigtime_t clientStart = cpu_time(find_thread(NULL));
			bigtime_t serverStart = cpu_time(drawing);
			bigtime_t start = system_time();
			for (int frame = 0; frame < count; frame++) {
				if (mode == 3)
					view->DrawPicture(&picture);
				else
					draw_scene(*view, mode);
				view->Sync();
			}
			bigtime_t elapsed = system_time() - start;
			bigtime_t serverCPU = cpu_time(drawing) - serverStart;
			bigtime_t clientCPU = cpu_time(find_thread(NULL)) - clientStart;
			printf("DRAWING width=%d height=%d mode=%d frames=%d wall_ms=%.3f "
				"client_ms=%.3f server_ms=%.3f hash=%016llx\n", width, height,
				mode, count, elapsed / 1000., clientCPU / 1000., serverCPU / 1000.,
				(unsigned long long)hash_pixels(bitmap));
		}
		bitmap.Unlock();
	}
	puts("DRAWING_PASS");
	return 0;
}
