/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "NetworkListView.h"

#include <Catalog.h>
#include <ControlLook.h>
#include <String.h>
#include <ToolTip.h>
#include <Window.h>

#include "WiFiGlyphs.h"
#include "../../shared/net/WirelessNetworkList.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "NetworkListView"


BString
NetworkToolTip(const WiFiNetworkInfo& info)
{
	BString text(info.name);
	if (!info.inRange) {
		text << "\n" << B_TRANSLATE("Not in range");
	} else {
		BString signal(B_TRANSLATE("Signal: %percent%% (%dBm% dBm)"));
		BString value;
		value << info.percent;
		signal.ReplaceFirst("%percent%", value);
		value = "";
		value << info.dBm;
		signal.ReplaceFirst("%dBm%", value);
		text << "\n" << signal;

		if (info.accessPoints > 1) {
			BString points(B_TRANSLATE("%count% access points"));
			value = "";
			value << info.accessPoints;
			points.ReplaceFirst("%count%", value);
			text << "\n" << points;
		}
	}

	text << "\n";
	if (info.secured) {
		BString security(B_TRANSLATE("Security: %mode%"));
		security.ReplaceFirst("%mode%",
			WirelessAuthenticationLabel(info.authentication, true));
		text << security;
	} else
		text << B_TRANSLATE("Open network");

	if (info.saved)
		text << "\n" << B_TRANSLATE("Saved network");
	return text;
}


// #pragma mark - NetworkListItem


NetworkListItem::NetworkListItem(const WiFiNetworkInfo& info)
	:
	fInfo(info),
	fBaselineOffset(0),
	fShowAvailability(true)
{
}


void
NetworkListItem::Update(BView* owner, const BFont* font)
{
	BListItem::Update(owner, font);

	font_height height;
	font->GetHeight(&height);
	float textHeight = ceilf(height.ascent + height.descent);
	float itemHeight = ceilf(textHeight * 1.7f);
	SetHeight(itemHeight);
	fBaselineOffset = floorf((itemHeight - textHeight) / 2 + height.ascent);
}


void
NetworkListItem::DrawItem(BView* owner, BRect frame, bool complete)
{
	rgb_color background = IsSelected()
		? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
		: ui_color(B_LIST_BACKGROUND_COLOR);
	rgb_color text = IsSelected()
		? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
		: ui_color(B_LIST_ITEM_TEXT_COLOR);

	owner->PushState();
	owner->SetLowColor(background);
	owner->FillRect(frame, B_SOLID_LOW);

	float padding = be_control_look->DefaultLabelSpacing();
	float glyphSize = floorf(frame.Height() * 0.55f);
	float glyphTop = floorf(frame.top + (frame.Height() - glyphSize) / 2);

	// Signal bars
	BRect bars(frame.left + padding, glyphTop,
		frame.left + padding + glyphSize * 1.2f, glyphTop + glyphSize - 1);
	if (fInfo.inRange) {
		DrawWiFiSignalBars(owner, bars, fInfo.bars, text, background);
	} else {
		DrawWiFiSignalBars(owner, bars, 0, text, background,
			SIGNAL_GLYPH_DISABLED);
	}

	// Lock
	float right = frame.right - padding;
	if (fInfo.secured) {
		BRect lock(right - glyphSize + 1, glyphTop, right,
			glyphTop + glyphSize - 1);
		DrawWiFiLock(owner, lock, WiFiGlyphTint(text, background,
			fInfo.inRange ? 0.8f : 0.5f));
	}
	right -= glyphSize + padding;

	// Secondary text
	const char* detail = NULL;
	if (!fInfo.inRange) {
		if (fShowAvailability)
			detail = B_TRANSLATE("Not in range");
	}
	else if (fInfo.connected)
		detail = B_TRANSLATE("Connected");

	float baseline = frame.top + fBaselineOffset;
	if (detail != NULL) {
		float width = owner->StringWidth(detail);
		owner->SetHighColor(WiFiGlyphTint(text, background, 0.6f));
		owner->DrawString(detail, BPoint(right - width, baseline));
		right -= width + padding;
	}

	// Name
	float left = bars.right + padding;
	BString name(fInfo.name);
	owner->TruncateString(&name, B_TRUNCATE_END, right - left);
	owner->SetHighColor(fInfo.inRange ? text
		: WiFiGlyphTint(text, background, 0.6f));
	owner->DrawString(name, BPoint(left, baseline));

	owner->PopState();
}


// #pragma mark - NetworkListView


NetworkListView::NetworkListView(const char* name)
	:
	BListView(name, B_SINGLE_SELECTION_LIST),
	fShowAvailability(true)
{
}


void
NetworkListView::SetShowAvailability(bool show)
{
	if (show == fShowAvailability)
		return;

	fShowAvailability = show;
	for (int32 i = 0; i < CountItems(); i++) {
		NetworkListItem* item = dynamic_cast<NetworkListItem*>(ItemAt(i));
		if (item != NULL)
			item->SetShowAvailability(show);
	}
	Invalidate();
}


void
NetworkListView::SetNetworks(const std::vector<WiFiNetworkInfo>& list)
{
	bool sameOrder = list.size() == fNetworks.size();
	for (size_t i = 0; sameOrder && i < list.size(); i++) {
		if (list[i].name != fNetworks[i].name)
			sameOrder = false;
	}

	if (sameOrder) {
		for (size_t i = 0; i < list.size(); i++) {
			if (list[i] == fNetworks[i])
				continue;
			NetworkListItem* item
				= dynamic_cast<NetworkListItem*>(ItemAt((int32)i));
			if (item != NULL) {
				item->SetInfo(list[i]);
				InvalidateItem((int32)i);
			}
		}
		fNetworks = list;
		return;
	}

	BString selected;
	const WiFiNetworkInfo* selection = SelectedNetwork();
	if (selection != NULL)
		selected = selection->name;

	// Keep the selection message from firing for the rebuild
	BMessage* message = SelectionMessage() != NULL
		? new BMessage(*SelectionMessage()) : NULL;
	SetSelectionMessage(NULL);

	float scroll = Bounds().top;
	for (int32 i = CountItems() - 1; i >= 0; i--)
		delete RemoveItem(i);

	fNetworks = list;
	for (size_t i = 0; i < list.size(); i++) {
		NetworkListItem* item = new NetworkListItem(list[i]);
		item->SetShowAvailability(fShowAvailability);
		AddItem(item);
	}

	if (!selected.IsEmpty())
		Select(selected);
	ScrollTo(0, std::min(scroll, std::max(0.0f,
		(CountItems() > 0 ? ItemFrame(CountItems() - 1).bottom : 0)
			- Bounds().Height())));

	SetSelectionMessage(message);
}


const WiFiNetworkInfo*
NetworkListView::SelectedNetwork() const
{
	int32 index = CurrentSelection();
	if (index < 0 || index >= (int32)fNetworks.size())
		return NULL;
	return &fNetworks[index];
}


bool
NetworkListView::Select(const char* name)
{
	for (size_t i = 0; i < fNetworks.size(); i++) {
		if (fNetworks[i].name == name) {
			BListView::Select((int32)i);
			ScrollToSelection();
			return true;
		}
	}
	return false;
}


bool
NetworkListView::GetToolTipAt(BPoint point, BToolTip** _tip)
{
	int32 index = IndexOf(point);
	if (index < 0 || index >= (int32)fNetworks.size())
		return false;

	SetToolTip(NetworkToolTip(fNetworks[index]));
	*_tip = ToolTip();
	return *_tip != NULL;
}


// #pragma mark - SignalView


SignalView::SignalView(const char* name)
	:
	BView(name, B_WILL_DRAW | B_PULSE_NEEDED),
	fBars(0),
	fEnabled(false),
	fAnimate(false),
	fPhase(0)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
}


void
SignalView::SetSignal(int32 bars, bool enabled, bool animate)
{
	if (bars == fBars && enabled == fEnabled && animate == fAnimate)
		return;

	fBars = bars;
	fEnabled = enabled;
	fAnimate = animate;
	fPhase = 0;
	Invalidate();
}


void
SignalView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	int32 bars = fAnimate ? fPhase + 1 : fBars;
	signal_glyph_style style = fEnabled || fAnimate
		? SIGNAL_GLYPH_NORMAL : SIGNAL_GLYPH_DISABLED;
	DrawWiFiSignalBars(this, bounds.InsetByCopy(1, 1), bars,
		ui_color(B_PANEL_TEXT_COLOR), ViewColor(), style);
}


void
SignalView::Pulse()
{
	if (!fAnimate)
		return;

	fPhase = (fPhase + 1) % kWirelessSignalMaxBars;
	Invalidate();
}


BSize
SignalView::MinSize()
{
	float size = ceilf(be_plain_font->Size() * 2);
	return BSize(ceilf(size * 1.25f), size);
}


BSize
SignalView::MaxSize()
{
	return MinSize();
}
