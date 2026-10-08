/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _AUX_DISPLAY_H
#define _AUX_DISPLAY_H


#include <GraphicsDefs.h>
#include <Rect.h>
#include <SupportDefs.h>


class BBitmap;
class BString;
struct aux_display_touch_event;


/*!	An auxiliary display: a small screen beside the desktop that a program
	draws on itself (the DSI panel of a Raspberry Pi, a USB LCD). The
	display is not part of app_server's desktop; one program at a time
	draws into a bitmap of the display's size and presents it.

	\code
	BAuxDisplay display;
	if (display.SetTo(0) == B_OK) {
		BBitmap* bitmap = display.Bitmap();	// B_RGB32, accepts views
		bitmap->Lock();
		view->Draw...;
		bitmap->Unlock();
		display.Present();
	}
	\endcode
*/
class BAuxDisplay {
public:
								BAuxDisplay();
	virtual						~BAuxDisplay();

	static	int32				CountDisplays();
	static	status_t			GetDisplayPath(int32 index, BString& path);

			status_t			SetTo(int32 index = 0);
			status_t			SetTo(const char* devicePath);
			void				Unset();
			status_t			InitCheck() const;

			const char*			Name() const;
			const char*			DevicePath() const;
			int32				Width() const;
			int32				Height() const;
			BRect				Bounds() const;
			color_space			ColorSpace() const;
			float				RefreshRate() const;
			bool				HasTouch() const;
			bool				HasBacklight() const;
			bool				IsOn() const;

			status_t			SetPower(bool on);
			status_t			SetBacklight(float brightness);

			//! A bitmap of the display's size to draw into; owned here.
			BBitmap*			Bitmap();
			//! Shows the bitmap from the next frame on.
			status_t			Present();
			//! Shows \a bitmap, scaled to the display if the sizes differ.
			status_t			Present(const BBitmap* bitmap);

			status_t			WaitForTouch(aux_display_touch_event* events,
									int32* _count, bigtime_t timeout);

private:
			struct Buffer;

			status_t			_Open(const char* path);
			status_t			_MapBuffers();
			void				_UnmapBuffers();

			int					fDevice;
			status_t			fStatus;
			char				fPath[256];
			char				fName[64];
			uint32				fFlags;
			int32				fWidth;
			int32				fHeight;
			int32				fBytesPerRow;
			color_space			fColorSpace;
			uint32				fRefreshRate;
			uint32				fBufferCount;
			uint32				fBufferSize;
			Buffer*				fBuffers;
			int32				fNext;
			BBitmap*			fBitmap;

			uint32				_reserved[8];
};


#endif	// _AUX_DISPLAY_H
