/*
 * Copyright 2001-2015, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Rafael Romo
 *		Stefano Ceccherini (burton666@libero.it)
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "ScreenSettings.h"

#include <File.h>
#include <FindDirectory.h>
#include <Path.h>


static const char* kSettingsFileName = "Screen_data";


ScreenSettings::ScreenSettings()
{
	fWindowFrame.Set(0, 0, 640, 400);
	BPoint offset;

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
		path.Append(kSettingsFileName);

		BFile file(path.Path(), B_READ_ONLY);
		off_t size;
		if (file.InitCheck() == B_OK && file.GetSize(&size) == B_OK) {
			BRect frame;
			if (size >= (off_t)sizeof(BRect)
				&& file.Read(&frame, sizeof(BRect)) == sizeof(BRect)
				&& frame.IsValid()) {
				// the complete frame, as written by newer versions
				fWindowFrame = frame;
				return;
			}
			file.Seek(0, SEEK_SET);
			file.Read(&offset, sizeof(BPoint));
		}
	}

	fWindowFrame.OffsetBy(offset);
}


ScreenSettings::~ScreenSettings()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) < B_OK)
		return;

	path.Append(kSettingsFileName);

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		file.Write(&fWindowFrame, sizeof(BRect));
}


void
ScreenSettings::SetWindowFrame(BRect frame)
{
	fWindowFrame = frame;
}
