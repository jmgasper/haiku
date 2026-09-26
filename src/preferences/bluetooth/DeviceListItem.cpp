/*
 * Copyright 2009, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * Copyright 2026, Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "DeviceListItem.h"

#include <ControlLook.h>
#include <algorithm>
#include <View.h>

#include "DeviceIcons.h"


DeviceItem::DeviceItem(device_key key)
	:
	BListItem(),
	fKey(key),
	fKind(DEVICE_KIND_GENERIC),
	fSignalBars(-1)
{
}


bool
DeviceItem::SetContent(device_kind kind, const BString& name,
	const BString& detail, const BString& status, int32 signalBars)
{
	if (kind == fKind && name == fName && detail == fDetail
		&& status == fStatus && signalBars == fSignalBars)
		return false;

	fKind = kind;
	fName = name;
	fDetail = detail;
	fStatus = status;
	fSignalBars = signalBars;
	return true;
}


void
DeviceItem::DrawItem(BView* owner, BRect frame, bool complete)
{
	rgb_color background = ui_color(IsSelected()
		? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
	rgb_color textColor = ui_color(IsSelected()
		? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);
	rgb_color detailColor = mix_color(textColor, background, 150);

	owner->SetLowColor(background);
	if (IsSelected() || complete) {
		owner->SetHighColor(background);
		owner->FillRect(frame);
	}

	float spacing = be_control_look->DefaultLabelSpacing();
	float iconSize = DeviceIconSize();
	BRect iconRect(0, 0, iconSize - 1, iconSize - 1);
	iconRect.OffsetTo(frame.left + spacing,
		floorf(frame.top + (frame.Height() - iconSize) / 2));
	DrawDeviceIcon(owner, iconRect, fKind);

	font_height plainHeight;
	be_plain_font->GetHeight(&plainHeight);
	BFont smallFont(be_plain_font);
	smallFont.SetSize(ceilf(be_plain_font->Size() * 0.85f));
	font_height smallHeight;
	smallFont.GetHeight(&smallHeight);

	float lineHeight = ceilf(plainHeight.ascent + plainHeight.descent);
	float smallLineHeight = ceilf(smallHeight.ascent + smallHeight.descent);
	float textTop = floorf(frame.top + (frame.Height() - lineHeight
		- smallLineHeight - spacing / 2) / 2);
	float textLeft = iconRect.right + spacing * 2;

	// Right-hand side: signal bars or status text.
	float rightEdge = frame.right - spacing;
	if (fSignalBars >= 0) {
		float barsWidth = ceilf(iconSize * 0.6f);
		BRect barsRect(rightEdge - barsWidth, frame.top, rightEdge,
			frame.bottom);
		_DrawSignalBars(owner, barsRect, textColor, background);
		rightEdge -= barsWidth + spacing;
	}
	if (!fStatus.IsEmpty()) {
		owner->SetFont(&smallFont);
		owner->SetHighColor(detailColor);
		float width = owner->StringWidth(fStatus.String());
		owner->DrawString(fStatus.String(), BPoint(rightEdge - width,
			floorf(frame.top + (frame.Height() + smallHeight.ascent
				- smallHeight.descent) / 2)));
		rightEdge -= width + spacing;
	}

	float textWidth = rightEdge - textLeft;

	owner->SetFont(be_plain_font);
	owner->SetHighColor(textColor);
	BString name(fName);
	owner->TruncateString(&name, B_TRUNCATE_END, textWidth);
	owner->DrawString(name.String(),
		BPoint(textLeft, textTop + plainHeight.ascent));

	owner->SetFont(&smallFont);
	owner->SetHighColor(detailColor);
	BString detail(fDetail);
	owner->TruncateString(&detail, B_TRUNCATE_END, textWidth);
	owner->DrawString(detail.String(), BPoint(textLeft,
		textTop + lineHeight + spacing / 2 + smallHeight.ascent));

	owner->SetFont(be_plain_font);
}


void
DeviceItem::_DrawSignalBars(BView* owner, BRect frame, rgb_color textColor,
	rgb_color background)
{
	const int32 kBars = 4;
	float gap = 2;
	float barWidth = floorf((frame.Width() - gap * (kBars - 1)) / kBars);
	float maxHeight = floorf(frame.Height() * 0.4f);
	float bottom = floorf(frame.top + (frame.Height() + maxHeight) / 2);
	rgb_color inactive = mix_color(textColor, background, 190);

	for (int32 i = 0; i < kBars; i++) {
		float height = floorf(maxHeight * (i + 1) / kBars);
		BRect bar(frame.left + i * (barWidth + gap), bottom - height,
			frame.left + i * (barWidth + gap) + barWidth - 1, bottom);
		owner->SetHighColor(i < fSignalBars ? textColor : inactive);
		owner->FillRect(bar);
	}
}


void
DeviceItem::Update(BView* owner, const BFont* font)
{
	BListItem::Update(owner, font);

	font_height plainHeight;
	font->GetHeight(&plainHeight);
	float spacing = be_control_look->DefaultLabelSpacing();
	float textHeight = ceilf((plainHeight.ascent + plainHeight.descent) * 1.85f
		+ spacing / 2);
	SetHeight(ceilf(std::max(textHeight, DeviceIconSize()) + spacing * 2));
}


//	#pragma mark - DeviceListView


DeviceListView::DeviceListView(const char* name)
	:
	BListView(name, B_SINGLE_SELECTION_LIST,
		B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE)
{
}


bool
DeviceListView::AddItem(BListItem* item)
{
	bool wasEmpty = IsEmpty();
	bool result = BListView::AddItem(item);
	if (wasEmpty && result)
		Invalidate();
			// clears the empty-list hint
	return result;
}


bool
DeviceListView::RemoveItem(BListItem* item)
{
	bool result = BListView::RemoveItem(item);
	if (result && IsEmpty())
		Invalidate();
	return result;
}


void
DeviceListView::MakeEmpty()
{
	BListView::MakeEmpty();
	Invalidate();
}


void
DeviceListView::SetEmptyText(const char* text)
{
	if (fEmptyText == text)
		return;
	fEmptyText = text;
	if (IsEmpty())
		Invalidate();
}


DeviceItem*
DeviceListView::SelectedDevice() const
{
	return static_cast<DeviceItem*>(ItemAt(CurrentSelection()));
}


void
DeviceListView::Draw(BRect updateRect)
{
	BListView::Draw(updateRect);

	if (!IsEmpty() || fEmptyText.IsEmpty())
		return;

	rgb_color background = ui_color(B_LIST_BACKGROUND_COLOR);
	SetHighColor(mix_color(ui_color(B_LIST_ITEM_TEXT_COLOR), background, 120));
	SetLowColor(background);
	BString text(fEmptyText);
	BRect bounds = Bounds();
	TruncateString(&text, B_TRUNCATE_END, bounds.Width() - 10);
	font_height height;
	GetFontHeight(&height);
	DrawString(text.String(), BPoint(
		floorf((bounds.left + bounds.right - StringWidth(text.String())) / 2),
		floorf((bounds.top + bounds.bottom + height.ascent - height.descent)
			/ 2)));
}


void
DeviceListView::FrameResized(float width, float height)
{
	BListView::FrameResized(width, height);
	if (IsEmpty())
		Invalidate();
}
