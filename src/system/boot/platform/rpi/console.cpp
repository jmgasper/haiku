/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The loader's console: text on the firmware's frame buffer, copied to the
	UART. Keys come from the UART only, as the loader has no USB driver. */


#include "console.h"

#include <string.h>

#include <KernelExport.h>

#include <boot/platform.h>
#include <boot/platform/generic/video.h>
#include <boot/stage2.h>
#include <boot/stdio.h>

#include "serial.h"
#include "video.h"


class Console : public ConsoleNode {
public:
								Console();

			void				SetScreen(ConsoleNode* screen)
									{ fScreen = screen; }
			ConsoleNode*		Screen() const
									{ return fScreen; }

	virtual	ssize_t				ReadAt(void* cookie, off_t pos, void* buffer,
									size_t bufferSize);
	virtual	ssize_t				WriteAt(void* cookie, off_t pos,
									const void* buffer, size_t bufferSize);

	virtual	void				ClearScreen();
	virtual	int32				Width();
	virtual	int32				Height();
	virtual	void				SetCursor(int32 x, int32 y);
	virtual	void				SetCursorVisible(bool visible);
	virtual	void				SetColors(int32 foreground, int32 background);

private:
			ConsoleNode*		fScreen;
};


extern ConsoleNode* gConsoleNode;

FILE* stdin;
FILE* stdout;
FILE* stderr;

static Console sConsole;


Console::Console()
	:
	ConsoleNode(),
	fScreen(NULL)
{
}


ssize_t
Console::ReadAt(void* cookie, off_t pos, void* buffer, size_t bufferSize)
{
	return B_ERROR;
}


ssize_t
Console::WriteAt(void* cookie, off_t pos, const void* buffer,
	size_t bufferSize)
{
	serial_puts((const char*)buffer, bufferSize);
	if (fScreen != NULL)
		fScreen->WriteAt(cookie, pos, buffer, bufferSize);
	return bufferSize;
}


void
Console::ClearScreen()
{
	if (fScreen != NULL)
		fScreen->ClearScreen();
}


int32
Console::Width()
{
	return fScreen != NULL ? fScreen->Width() : 80;
}


int32
Console::Height()
{
	return fScreen != NULL ? fScreen->Height() : 25;
}


void
Console::SetCursor(int32 x, int32 y)
{
	if (fScreen != NULL)
		fScreen->SetCursor(x, y);
}


void
Console::SetCursorVisible(bool visible)
{
	if (fScreen != NULL)
		fScreen->SetCursorVisible(visible);
}


void
Console::SetColors(int32 foreground, int32 background)
{
	if (fScreen != NULL)
		fScreen->SetColors(foreground, background);
}


//	#pragma mark -


void
console_write_debug(const char* buffer, size_t length)
{
	if (sConsole.Screen() != NULL)
		sConsole.Screen()->WriteAt(NULL, 0, buffer, length);
}


int
console_wait_for_key(void)
{
	int c = serial_getc(false);
	if (c < 0) {
		// Nothing to read keys from, or nobody typing: keep the menu on the
		// screen for a while and then take what is selected.
		bigtime_t timeout = system_time() + 10 * 1000000LL;
		while ((c = serial_getc(false)) < 0) {
			if (system_time() > timeout)
				return TEXT_CONSOLE_KEY_RETURN;
		}
	}

	if (c == '\n')
		return TEXT_CONSOLE_KEY_RETURN;
	if (c == 0x7f)
		return TEXT_CONSOLE_KEY_BACKSPACE;
	if (c != 0x1b)
		return c;

	// an escape sequence, or the escape key by itself
	bigtime_t timeout = system_time() + 100000;
	while ((c = serial_getc(false)) < 0) {
		if (system_time() > timeout)
			return TEXT_CONSOLE_KEY_ESCAPE;
	}
	if (c != '[' && c != 'O')
		return TEXT_CONSOLE_KEY_ESCAPE;

	switch (serial_getc(true)) {
		case 'A':
			return TEXT_CONSOLE_KEY_UP;
		case 'B':
			return TEXT_CONSOLE_KEY_DOWN;
		case 'C':
			return TEXT_CONSOLE_KEY_RIGHT;
		case 'D':
			return TEXT_CONSOLE_KEY_LEFT;
		case 'H':
			return TEXT_CONSOLE_KEY_HOME;
		case 'F':
			return TEXT_CONSOLE_KEY_END;
		case '5':
			serial_getc(true);
			return TEXT_CONSOLE_KEY_PAGE_UP;
		case '6':
			serial_getc(true);
			return TEXT_CONSOLE_KEY_PAGE_DOWN;
	}

	return TEXT_CONSOLE_NO_KEY;
}


status_t
console_init(void)
{
	if (video_frame_buffer() != 0)
		sConsole.SetScreen(video_text_console_init(video_frame_buffer()));

	gConsoleNode = &sConsole;
	stdin = (FILE*)&sConsole;
	stdout = (FILE*)&sConsole;
	stderr = (FILE*)&sConsole;
	return B_OK;
}
