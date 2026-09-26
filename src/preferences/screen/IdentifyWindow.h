/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef IDENTIFY_WINDOW_H
#define IDENTIFY_WINDOW_H


#include <Window.h>


class BMessageRunner;


/*!	A borderless badge shown in the middle of a display for a few seconds,
	with the display's number and monitor name.
*/
class IdentifyWindow : public BWindow {
public:
								IdentifyWindow(int32 number, const char* name,
									BRect displayFrame);
	virtual						~IdentifyWindow();

	virtual	void				MessageReceived(BMessage* message);

private:
			BMessageRunner*		fRunner;
};


#endif	// IDENTIFY_WINDOW_H
