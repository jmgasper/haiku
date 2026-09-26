/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "IdentifyWindow.h"

#include <math.h>
#include <stdio.h>

#include <algorithm>

#include <Font.h>
#include <MessageRunner.h>
#include <String.h>
#include <View.h>

#include "Constants.h"


static const bigtime_t kIdentifyDuration = 3000000;
	// 3 seconds


class IdentifyView : public BView {
public:
								IdentifyView(BRect frame, int32 number,
									const char* name);

	virtual	void				Draw(BRect updateRect);

private:
			int32				fNumber;
			BString				fName;
};


IdentifyView::IdentifyView(BRect frame, int32 number, const char* name)
	:
	BView(frame, "identify", B_FOLLOW_ALL, B_WILL_DRAW),
	fNumber(number),
	fName(name)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
IdentifyView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	rgb_color background = { 28, 30, 34, 255 };
	rgb_color border = { 90, 94, 100, 255 };
	rgb_color textColor = { 240, 240, 240, 255 };
	rgb_color nameColor = { 190, 190, 190, 255 };

	float radius = std::max(6.0f, bounds.Height() * 0.08f);

	// The window is opaque; the corners outside the rounded panel are filled
	// with a darker shade so the badge still reads as a rounded panel.
	SetHighColor(tint_color(background, B_DARKEN_2_TINT));
	FillRect(bounds);

	SetHighColor(background);
	FillRoundRect(bounds, radius, radius);
	SetHighColor(border);
	StrokeRoundRect(bounds, radius, radius);

	SetDrawingMode(B_OP_OVER);
	SetLowColor(background);

	// the number, very large
	BFont numberFont(be_bold_font);
	numberFont.SetSize(std::max(24.0f, floorf(bounds.Height() * 0.5f)));
	SetFont(&numberFont);

	font_height numberHeight;
	numberFont.GetHeight(&numberHeight);
	float numberTextHeight = ceilf(numberHeight.ascent + numberHeight.descent);

	char number[16];
	snprintf(number, sizeof(number), "%" B_PRId32, fNumber);
	float numberWidth = StringWidth(number);

	// the monitor name below it
	BFont nameFont(be_plain_font);
	nameFont.SetSize(std::max(12.0f, floorf(bounds.Height() * 0.12f)));
	font_height nameHeight;
	nameFont.GetHeight(&nameHeight);
	float nameTextHeight = fName.Length() > 0
		? ceilf(nameHeight.ascent + nameHeight.descent) : 0;
	float spacing = fName.Length() > 0 ? bounds.Height() * 0.04f : 0;

	BString name = fName;
	nameFont.TruncateString(&name, B_TRUNCATE_END, bounds.Width() - 2 * radius);

	float totalHeight = numberTextHeight + spacing + nameTextHeight;
	float top = bounds.top + (bounds.Height() - totalHeight) / 2;

	SetHighColor(textColor);
	DrawString(number, BPoint(bounds.left + (bounds.Width() - numberWidth) / 2,
		top + numberHeight.ascent));

	if (fName.Length() > 0) {
		SetFont(&nameFont);
		SetHighColor(nameColor);
		float nameWidth = StringWidth(name.String());
		DrawString(name.String(),
			BPoint(bounds.left + (bounds.Width() - nameWidth) / 2,
				top + numberTextHeight + spacing + nameHeight.ascent));
	}
}


//	#pragma mark - IdentifyWindow


IdentifyWindow::IdentifyWindow(int32 number, const char* name,
	BRect displayFrame)
	:
	BWindow(BRect(0, 0, 199, 119), "identify", B_NO_BORDER_WINDOW_LOOK,
		B_FLOATING_ALL_WINDOW_FEEL, B_AVOID_FOCUS | B_NOT_MOVABLE
			| B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_NOT_CLOSABLE, B_ALL_WORKSPACES),
	fRunner(NULL)
{
	float width = floorf((displayFrame.Width() + 1) / 4);
	width = std::max(width, 160.0f);
	float height = floorf(width * 0.6f);
	height = std::min(height, floorf((displayFrame.Height() + 1) / 3));

	ResizeTo(width - 1, height - 1);
	MoveTo(floorf(displayFrame.left + (displayFrame.Width() + 1 - width) / 2),
		floorf(displayFrame.top + (displayFrame.Height() + 1 - height) / 2));

	AddChild(new IdentifyView(Bounds(), number, name));

	fRunner = new BMessageRunner(BMessenger(this),
		new BMessage(kMsgIdentifyDone), kIdentifyDuration, 1);
}


IdentifyWindow::~IdentifyWindow()
{
	delete fRunner;
}


void
IdentifyWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgIdentifyDone:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}
