/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Capture each connected screen without requiring image translators.
#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <stdio.h>

int
main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	BApplication app("application/x-vnd.airos-screen-capture");
	int saved = 0;
	for (int32 index = 0; index < 4; index++) {
		screen_id id = {index};
		BScreen screen(id);
		BRect frame = screen.Frame();
		printf("screen %ld valid=%d frame=%.0f,%.0f,%.0f,%.0f\n",
			(long)index, screen.IsValid(), frame.left, frame.top,
			frame.right, frame.bottom);
		if (!screen.IsValid() || frame.Width() < 1 || frame.Height() < 1)
			continue;
		BBitmap* bitmap = NULL;
		status_t status = screen.GetBitmap(&bitmap, false);
		if (status != B_OK || bitmap == NULL)
			continue;
		if (bitmap->ColorSpace() != B_RGB32 && bitmap->ColorSpace() != B_RGBA32) {
			delete bitmap;
			continue;
		}
		char path[1024];
		snprintf(path, sizeof(path), "%s-%ld.ppm", argv[1], (long)index);
		FILE* file = fopen(path, "wb");
		if (file == NULL) {
			delete bitmap;
			return 1;
		}
		int width = bitmap->Bounds().IntegerWidth() + 1;
		int height = bitmap->Bounds().IntegerHeight() + 1;
		fprintf(file, "P6\n%d %d\n255\n", width, height);
		for (int y = 0; y < height; y++) {
			const uint8* row = (const uint8*)bitmap->Bits()
				+ y * bitmap->BytesPerRow();
			for (int x = 0; x < width; x++) {
				uint8 rgb[] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4]};
				fwrite(rgb, sizeof(rgb), 1, file);
			}
		}
		bool failed = ferror(file);
		if (fclose(file) != 0)
			failed = true;
		delete bitmap;
		if (failed)
			return 1;
		saved++;
	}
	return saved > 0 ? 0 : 1;
}
