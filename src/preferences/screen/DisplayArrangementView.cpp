/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "DisplayArrangementView.h"

#include <math.h>
#include <stdio.h>

#include <algorithm>

#include <Catalog.h>
#include <ControlLook.h>
#include <Font.h>
#include <LayoutUtils.h>
#include <Message.h>
#include <Window.h>

#include "Constants.h"
#include "DisplayLayoutState.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Screen"


static const float kSnapDistance = 12.0f;
	// view pixels
static const float kDragThreshold = 3.0f;


DisplayArrangementView::DisplayArrangementView(const char* name)
	:
	BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
	fSelectedID(-1),
	fDraggingEnabled(true),
	fScale(1.0f),
	fOrigin(0, 0),
	fDragIndex(-1),
	fDragMoved(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetToolTip(B_TRANSLATE("Drag the displays to arrange them."));
}


DisplayArrangementView::~DisplayArrangementView()
{
}


void
DisplayArrangementView::AttachedToWindow()
{
	BView::AttachedToWindow();
	_UpdateScale();
}


void
DisplayArrangementView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_UpdateScale();
	Invalidate();
}


BSize
DisplayArrangementView::MinSize()
{
	float factor = std::max(1.0f, be_plain_font->Size() / 12.0f);
	return BLayoutUtils::ComposeSize(ExplicitMinSize(),
		BSize(260 * factor, 170 * factor));
}


BSize
DisplayArrangementView::PreferredSize()
{
	float factor = std::max(1.0f, be_plain_font->Size() / 12.0f);
	return BLayoutUtils::ComposeSize(ExplicitPreferredSize(),
		BSize(380 * factor, 240 * factor));
}


void
DisplayArrangementView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_COLORS_UPDATED:
			Invalidate();
			break;

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
DisplayArrangementView::SetDisplays(const DisplayLayoutState& state)
{
	fEntries.clear();
	fDragIndex = -1;

	int32 number = 1;
	for (int32 i = 0; i < state.CountDisplays(); i++) {
		const display_state* display = state.DisplayAt(i);
		if (!display->connected)
			continue;

		entry item;
		item.id = display->id;
		item.number = number++;
		item.label = display->monitor.Length() > 0
			? display->monitor : display->name;
		item.frame = display->frame;
		item.enabled = display->enabled;
		item.primary = display->primary;
		fEntries.push_back(item);
	}

	_UpdateScale();
	Invalidate();
}


void
DisplayArrangementView::SetSelectedID(int32 id)
{
	if (fSelectedID == id)
		return;

	fSelectedID = id;
	Invalidate();
}


void
DisplayArrangementView::SetDraggingEnabled(bool enabled)
{
	fDraggingEnabled = enabled;
	if (!enabled)
		SetToolTip((const char*)NULL);
}


//	#pragma mark - drawing


void
DisplayArrangementView::Draw(BRect updateRect)
{
	rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);
	BRect bounds = Bounds();

	// a slightly recessed canvas for the displays
	SetHighColor(tint_color(base, 1.04f));
	FillRoundRect(bounds, 4, 4);
	SetHighColor(tint_color(base, B_DARKEN_1_TINT));
	StrokeRoundRect(bounds, 4, 4);

	for (int32 i = 0; i < (int32)fEntries.size(); i++) {
		if (i == fDragIndex)
			continue;
		_DrawEntry(i, updateRect);
	}

	// the dragged display is drawn on top of the others
	if (fDragIndex >= 0)
		_DrawEntry(fDragIndex, updateRect);
}


void
DisplayArrangementView::_DrawEntry(int32 index, const BRect& updateRect)
{
	const entry& item = fEntries[index];
	BRect rect = _EntryRect(index);
	if (!rect.IsValid() || !rect.Intersects(updateRect))
		return;

	bool selected = item.id == fSelectedID;

	rgb_color background = ui_color(B_PANEL_BACKGROUND_COLOR);
	rgb_color base = ui_color(B_CONTROL_BACKGROUND_COLOR);
	rgb_color textColor = ui_color(B_CONTROL_TEXT_COLOR);
	if (selected) {
		base = mix_color(ui_color(B_CONTROL_HIGHLIGHT_COLOR), base, 140);
		textColor = mix_color(ui_color(B_CONTROL_HIGHLIGHT_COLOR), textColor,
			140);
	}

	uint32 flags = 0;
	if (!item.enabled) {
		flags |= BControlLook::B_DISABLED;
		textColor = tint_color(textColor, B_DISABLED_LABEL_TINT);
	}
	if (selected)
		flags |= BControlLook::B_FOCUSED;
	if (index == fDragIndex)
		flags |= BControlLook::B_ACTIVATED;

	float radius = std::max(3.0f, std::min(rect.Width(), rect.Height())
		* 0.06f);

	be_control_look->DrawButtonFrame(this, rect, updateRect, radius, base,
		background, flags);
	be_control_look->DrawButtonBackground(this, rect, updateRect, radius,
		base, flags);

	// the primary display gets a bar along its top edge, where the Deskbar
	// lives
	if (item.primary && rect.Height() > 20) {
		BRect bar = rect;
		bar.InsetBy(radius, 0);
		bar.top += 3;
		bar.bottom = bar.top + std::max(2.0f, rect.Height() * 0.05f);
		SetHighColor(item.enabled
			? tint_color(base, B_DARKEN_3_TINT)
			: tint_color(base, B_DARKEN_1_TINT));
		FillRoundRect(bar, 1, 1);
	}

	SetDrawingMode(B_OP_OVER);
	SetLowColor(base);
	SetHighColor(textColor);

	// the number, as large as fits
	BFont numberFont(be_bold_font);
	float numberSize = std::min(rect.Height() * 0.5f, rect.Width() * 0.4f);
	numberSize = std::max(10.0f, std::min(72.0f, floorf(numberSize)));
	numberFont.SetSize(numberSize);
	SetFont(&numberFont);

	font_height numberHeight;
	numberFont.GetHeight(&numberHeight);
	float numberTextHeight = ceilf(numberHeight.ascent
		+ numberHeight.descent);

	char number[16];
	snprintf(number, sizeof(number), "%" B_PRId32, item.number);
	float numberWidth = StringWidth(number);

	// the monitor name, if there is room for it
	BFont labelFont(be_plain_font);
	font_height labelHeight;
	labelFont.GetHeight(&labelHeight);
	float labelTextHeight = ceilf(labelHeight.ascent + labelHeight.descent);
	float spacing = be_control_look->DefaultLabelSpacing();

	BString label = item.label;
	bool showLabel = label.Length() > 0
		&& rect.Height() > numberTextHeight + labelTextHeight + 3 * spacing
		&& rect.Width() > labelFont.StringWidth("WWWWWW");
	if (showLabel)
		labelFont.TruncateString(&label, B_TRUNCATE_END, rect.Width() - 12);

	float totalHeight = numberTextHeight
		+ (showLabel ? labelTextHeight + spacing : 0);
	float top = rect.top + (rect.Height() - totalHeight) / 2;

	DrawString(number, BPoint(rect.left + (rect.Width() - numberWidth) / 2,
		top + numberHeight.ascent));

	if (showLabel) {
		SetFont(&labelFont);
		float labelWidth = StringWidth(label.String());
		DrawString(label.String(),
			BPoint(rect.left + (rect.Width() - labelWidth) / 2,
				top + numberTextHeight + spacing + labelHeight.ascent));
	}

	SetDrawingMode(B_OP_COPY);
}


//	#pragma mark - mouse


void
DisplayArrangementView::MouseDown(BPoint where)
{
	int32 index = _EntryAt(where);
	if (index < 0)
		return;

	if (fEntries[index].id != fSelectedID) {
		fSelectedID = fEntries[index].id;
		Invalidate();

		BMessage message(kMsgDisplaySelected);
		message.AddInt32("id", fSelectedID);
		Window()->PostMessage(&message, Window());
	}

	fDragIndex = index;
	fDragStart = where;
	fDragOriginalFrame = fEntries[index].frame;
	fDragMoved = false;

	SetMouseEventMask(B_POINTER_EVENTS,
		B_LOCK_WINDOW_FOCUS | B_NO_POINTER_HISTORY);
}


void
DisplayArrangementView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (fDragIndex < 0 || !fDraggingEnabled || fEntries.size() < 2)
		return;

	BPoint delta = where - fDragStart;
	if (!fDragMoved
		&& fabs(delta.x) < kDragThreshold && fabs(delta.y) < kDragThreshold)
		return;

	fDragMoved = true;

	BRect frame = fDragOriginalFrame;
	frame.OffsetBy(floorf(delta.x / fScale), floorf(delta.y / fScale));
	frame = _Snap(frame, fDragIndex);

	if (frame != fEntries[fDragIndex].frame) {
		fEntries[fDragIndex].frame = frame;
		Invalidate();
	}
}


void
DisplayArrangementView::MouseUp(BPoint where)
{
	if (fDragIndex < 0)
		return;

	int32 index = fDragIndex;
	fDragIndex = -1;

	if (fDragMoved) {
		BMessage message(kMsgDisplayMoved);
		message.AddInt32("id", fEntries[index].id);
		message.AddRect("frame", fEntries[index].frame);
		Window()->PostMessage(&message, Window());
	} else if (_NumberRect(index).Contains(where)) {
		// A click on the number badge identifies the displays
		Window()->PostMessage(kMsgIdentifyDisplays, Window());
	}

	Invalidate();
}


//	#pragma mark - geometry


/*!	Computes the factor that maps desktop coordinates to view pixels so that
	all displays fit, keeping their aspect ratio, and centers the group.
*/
void
DisplayArrangementView::_UpdateScale()
{
	BRect bounds = Bounds();
	float margin = be_control_look->DefaultItemSpacing() * 2;
	BRect area = bounds.InsetByCopy(margin, margin);

	BRect frame(0, 0, -1, -1);
	for (size_t i = 0; i < fEntries.size(); i++) {
		if (!frame.IsValid())
			frame = fEntries[i].frame;
		else
			frame = frame | fEntries[i].frame;
	}

	if (!frame.IsValid() || area.Width() <= 0 || area.Height() <= 0) {
		fScale = 1.0f;
		fOrigin = area.LeftTop();
		return;
	}

	float width = frame.Width() + 1;
	float height = frame.Height() + 1;
	fScale = std::min(area.Width() / width, area.Height() / height);
	// Do not blow up small layouts beyond a sensible size
	fScale = std::min(fScale, 0.5f);

	fOrigin.x = area.left + (area.Width() - width * fScale) / 2
		- frame.left * fScale;
	fOrigin.y = area.top + (area.Height() - height * fScale) / 2
		- frame.top * fScale;
}


BRect
DisplayArrangementView::_ViewRect(const BRect& frame) const
{
	BRect rect(fOrigin.x + frame.left * fScale,
		fOrigin.y + frame.top * fScale,
		fOrigin.x + (frame.right + 1) * fScale - 1,
		fOrigin.y + (frame.bottom + 1) * fScale - 1);
	rect.left = floorf(rect.left);
	rect.top = floorf(rect.top);
	rect.right = floorf(rect.right);
	rect.bottom = floorf(rect.bottom);
	return rect;
}


BRect
DisplayArrangementView::_EntryRect(int32 index) const
{
	if (index < 0 || index >= (int32)fEntries.size())
		return BRect(0, 0, -1, -1);

	BRect rect = _ViewRect(fEntries[index].frame);
	// leave a small gap between adjacent displays
	rect.InsetBy(2, 2);
	return rect;
}


BRect
DisplayArrangementView::_NumberRect(int32 index) const
{
	BRect rect = _EntryRect(index);
	if (!rect.IsValid())
		return rect;

	float size = std::min(rect.Height() * 0.5f, rect.Width() * 0.4f);
	size = std::max(10.0f, std::min(72.0f, size));
	BPoint center(rect.left + rect.Width() / 2,
		rect.top + rect.Height() / 2);
	return BRect(center.x - size, center.y - size, center.x + size,
		center.y + size);
}


int32
DisplayArrangementView::_EntryAt(BPoint where) const
{
	// the topmost (last drawn) display wins
	for (int32 i = (int32)fEntries.size() - 1; i >= 0; i--) {
		if (_EntryRect(i).Contains(where))
			return i;
	}
	return -1;
}


/*!	Magnetic snapping while dragging: the edges of \a frame are pulled to the
	edges of the other enabled displays when they are close, on both axes
	independently. This covers side by side and stacked arrangements as well
	as aligned edges and corners.
*/
BRect
DisplayArrangementView::_Snap(const BRect& frame, int32 index) const
{
	if (fScale <= 0)
		return frame;

	float threshold = kSnapDistance / fScale;
	float bestX = 0;
	float bestY = 0;
	float bestDistanceX = threshold;
	float bestDistanceY = threshold;

	float centerX = (frame.left + frame.right) / 2;
	float centerY = (frame.top + frame.bottom) / 2;

	for (int32 i = 0; i < (int32)fEntries.size(); i++) {
		if (i == index || !fEntries[i].enabled)
			continue;

		const BRect& other = fEntries[i].frame;
		float otherCenterX = (other.left + other.right) / 2;
		float otherCenterY = (other.top + other.bottom) / 2;

		float candidatesX[] = {
			other.left - frame.left,			// left edges aligned
			other.right + 1 - frame.left,		// right of the other
			other.right - frame.right,			// right edges aligned
			other.left - 1 - frame.right,		// left of the other
			otherCenterX - centerX				// centered
		};
		float candidatesY[] = {
			other.top - frame.top,
			other.bottom + 1 - frame.top,
			other.bottom - frame.bottom,
			other.top - 1 - frame.bottom,
			otherCenterY - centerY
		};

		for (size_t j = 0; j < B_COUNT_OF(candidatesX); j++) {
			if (fabs(candidatesX[j]) < bestDistanceX) {
				bestDistanceX = fabs(candidatesX[j]);
				bestX = candidatesX[j];
			}
			if (fabs(candidatesY[j]) < bestDistanceY) {
				bestDistanceY = fabs(candidatesY[j]);
				bestY = candidatesY[j];
			}
		}
	}

	BRect snapped = frame;
	snapped.OffsetBy(bestX, bestY);
	return snapped;
}
