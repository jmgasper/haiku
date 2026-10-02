/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "DisplayLayoutState.h"

#include <math.h>

#include <algorithm>

#include <Control.h>
#include <InterfacePrivate.h>


bool
refresh_rates_equal(float a, float b)
{
	return fabs(a - b) < 0.05f;
}


//	#pragma mark - display_state


display_state::display_state()
	:
	id(-1),
	productID(0),
	week(0),
	year(0),
	widthCM(0),
	heightCM(0),
	hasEDID(false),
	flags(0),
	connected(true),
	enabled(true),
	primary(false),
	mirrorOf(-1),
	frame(0, 0, -1, -1),
	scale(100),
	nativeWidth(0),
	nativeHeight(0),
	nativeRefresh(0),
	modeWidth(0),
	modeHeight(0),
	modeRefresh(0)
{
}


status_t
display_state::SetTo(const BMessage& message)
{
	if (message.FindInt32("id", &id) != B_OK)
		return B_BAD_VALUE;

	const char* string;
	name = message.FindString("name", &string) == B_OK ? string : "";
	key = message.FindString("key", &string) == B_OK ? string : "";
	monitor = message.FindString("monitor", &string) == B_OK ? string : "";
	vendor = message.FindString("vendor", &string) == B_OK ? string : "";
	serial = message.FindString("serial", &string) == B_OK ? string : "";

	if (message.FindInt32("product id", &productID) != B_OK)
		productID = 0;
	if (message.FindInt32("week", &week) != B_OK)
		week = 0;
	if (message.FindInt32("year", &year) != B_OK)
		year = 0;
	if (message.FindFloat("width cm", &widthCM) != B_OK)
		widthCM = 0;
	if (message.FindFloat("height cm", &heightCM) != B_OK)
		heightCM = 0;
	if (message.FindBool("has edid", &hasEDID) != B_OK)
		hasEDID = false;
	if (message.FindInt32("flags", &flags) != B_OK)
		flags = 0;
	if (message.FindBool("connected", &connected) != B_OK)
		connected = true;
	if (message.FindBool("enabled", &enabled) != B_OK)
		enabled = true;
	if (message.FindBool("primary", &primary) != B_OK)
		primary = false;
	if (message.FindInt32("mirror", &mirrorOf) != B_OK || mirrorOf < 0)
		mirrorOf = -1;
	if (message.FindRect("frame", &frame) != B_OK)
		frame = BRect(0, 0, -1, -1);
	if (message.FindInt32("scale", &scale) != B_OK || scale <= 0)
		scale = 100;

	if (message.FindInt32("native width", &nativeWidth) != B_OK)
		nativeWidth = 0;
	if (message.FindInt32("native height", &nativeHeight) != B_OK)
		nativeHeight = 0;
	if (message.FindFloat("native refresh", &nativeRefresh) != B_OK)
		nativeRefresh = 0;
	if (message.FindInt32("mode width", &modeWidth) != B_OK)
		modeWidth = 0;
	if (message.FindInt32("mode height", &modeHeight) != B_OK)
		modeHeight = 0;
	if (message.FindFloat("mode refresh", &modeRefresh) != B_OK)
		modeRefresh = 0;

	if (modeWidth <= 0 || modeHeight <= 0) {
		// Nothing is driven yet; fall back to the native mode or the frame
		if (nativeWidth > 0 && nativeHeight > 0) {
			modeWidth = nativeWidth;
			modeHeight = nativeHeight;
			modeRefresh = nativeRefresh;
		} else if (frame.IsValid()) {
			modeWidth = frame.IntegerWidth() + 1;
			modeHeight = frame.IntegerHeight() + 1;
		}
	}
	if (nativeWidth <= 0 || nativeHeight <= 0) {
		nativeWidth = modeWidth;
		nativeHeight = modeHeight;
		nativeRefresh = modeRefresh;
	}

	modes.clear();
	BMessage mode;
	for (int32 i = 0; message.FindMessage("modes", i, &mode) == B_OK; i++) {
		display_mode_entry entry;
		if (mode.FindInt32("width", &entry.width) != B_OK
			|| mode.FindInt32("height", &entry.height) != B_OK)
			continue;
		if (mode.FindFloat("refresh", &entry.refresh) != B_OK)
			entry.refresh = 0;
		if (entry.width <= 0 || entry.height <= 0)
			continue;
		modes.push_back(entry);
	}

	if (!frame.IsValid())
		UpdateFrameSize();

	return B_OK;
}


/*!	Derives the logical size of the display from its mode and scale, the same
	way the app_server does, keeping the position.
*/
void
display_state::UpdateFrameSize()
{
	if (scale <= 0)
		scale = 100;
	int32 logicalWidth = (modeWidth * 100 + scale / 2) / scale;
	int32 logicalHeight = (modeHeight * 100 + scale / 2) / scale;
	frame.right = frame.left + logicalWidth - 1;
	frame.bottom = frame.top + logicalHeight - 1;
}


/*!	Compares only what is sent to the app_server on Apply. A mirror's
	place and scale follow from its source.
*/
bool
display_state::SameSettings(const display_state& other) const
{
	if (id != other.id || enabled != other.enabled
		|| primary != other.primary || mirrorOf != other.mirrorOf
		|| modeWidth != other.modeWidth || modeHeight != other.modeHeight
		|| !refresh_rates_equal(modeRefresh, other.modeRefresh))
		return false;

	if (!IsMirror() && scale != other.scale)
		return false;

	// The position of a disabled display does not matter
	if (enabled && !IsMirror() && frame.LeftTop() != other.frame.LeftTop())
		return false;

	return true;
}


bool
display_state::HasMode(int32 width, int32 height) const
{
	for (size_t i = 0; i < modes.size(); i++) {
		if (modes[i].width == width && modes[i].height == height)
			return true;
	}
	return false;
}


float
display_state::DiagonalInches() const
{
	if (widthCM <= 0 || heightCM <= 0)
		return 0;
	return roundf(sqrtf(widthCM * widthCM + heightCM * heightCM) / 0.254f)
		/ 10.0f;
}


int32
display_state::DPI() const
{
	if (widthCM <= 0 || nativeWidth <= 0)
		return 0;
	return (int32)roundf(nativeWidth / (widthCM / 2.54f));
}


//	#pragma mark - DisplayLayoutState


DisplayLayoutState::DisplayLayoutState()
	:
	fFrame(0, 0, -1, -1),
	fScreenFrame(0, 0, -1, -1),
	fHasLayout(false),
	fCanScale(false),
	fZoomToDisplay(false)
{
}


status_t
DisplayLayoutState::Load()
{
	BMessage layout;
	status_t status = BPrivate::get_display_layout(layout);
	if (status != B_OK)
		return status;

	return SetTo(layout);
}


status_t
DisplayLayoutState::SetTo(const BMessage& layout)
{
	fDisplays.clear();
	fScales.clear();

	BMessage displayMessage;
	for (int32 i = 0; layout.FindMessage("display", i, &displayMessage)
			== B_OK; i++) {
		display_state display;
		if (display.SetTo(displayMessage) == B_OK)
			fDisplays.push_back(display);
	}

	int32 scale;
	for (int32 i = 0; layout.FindInt32("scales", i, &scale) == B_OK; i++)
		fScales.push_back(scale);
	if (fScales.empty())
		fScales.push_back(100);

	if (layout.FindRect("frame", &fFrame) != B_OK)
		fFrame = BRect(0, 0, -1, -1);
	if (layout.FindRect("screen frame", &fScreenFrame) != B_OK)
		fScreenFrame = fFrame;
	if (layout.FindBool("has layout", &fHasLayout) != B_OK)
		fHasLayout = false;
	if (layout.FindBool("zoom to display", &fZoomToDisplay) != B_OK)
		fZoomToDisplay = false;

	if (fDisplays.empty())
		return B_ENTRY_NOT_FOUND;

	// Without a layout the driver shows one display with the whole frame
	// buffer; the app_server still draws into it at the display's scale.
	fCanScale = true;

	if (PrimaryID() < 0 && FirstEnabledID() >= 0)
		SetPrimary(FirstEnabledID());

	return B_OK;
}


/*!	Used when the app_server does not know about display layouts at all. */
void
DisplayLayoutState::SetToSingleDisplay(BRect frame, int32 width,
	int32 height, float refresh)
{
	fDisplays.clear();
	fScales.clear();
	fScales.push_back(100);

	display_state display;
	display.id = 0;
	display.connected = true;
	display.enabled = true;
	display.primary = true;
	display.frame = frame;
	display.scale = 100;
	display.modeWidth = display.nativeWidth = width;
	display.modeHeight = display.nativeHeight = height;
	display.modeRefresh = display.nativeRefresh = refresh;
	fDisplays.push_back(display);

	fFrame = fScreenFrame = frame;
	fHasLayout = false;
	fCanScale = false;
	fZoomToDisplay = false;
}


int32
DisplayLayoutState::CountDisplays() const
{
	return (int32)fDisplays.size();
}


display_state*
DisplayLayoutState::DisplayAt(int32 index)
{
	if (index < 0 || index >= (int32)fDisplays.size())
		return NULL;
	return &fDisplays[index];
}


const display_state*
DisplayLayoutState::DisplayAt(int32 index) const
{
	if (index < 0 || index >= (int32)fDisplays.size())
		return NULL;
	return &fDisplays[index];
}


display_state*
DisplayLayoutState::DisplayByID(int32 id)
{
	return DisplayAt(IndexOf(id));
}


const display_state*
DisplayLayoutState::DisplayByID(int32 id) const
{
	return DisplayAt(IndexOf(id));
}


int32
DisplayLayoutState::IndexOf(int32 id) const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].id == id)
			return (int32)i;
	}
	return -1;
}


int32
DisplayLayoutState::CountEnabled() const
{
	int32 count = 0;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].connected && fDisplays[i].enabled)
			count++;
	}
	return count;
}


int32
DisplayLayoutState::CountConnected() const
{
	int32 count = 0;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].connected)
			count++;
	}
	return count;
}


int32
DisplayLayoutState::NumberOf(int32 id) const
{
	int32 number = 1;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (!fDisplays[i].connected)
			continue;
		if (fDisplays[i].id == id)
			return number;
		number++;
	}
	return 0;
}


int32
DisplayLayoutState::PrimaryID() const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].connected && fDisplays[i].primary)
			return fDisplays[i].id;
	}
	return -1;
}


int32
DisplayLayoutState::FirstEnabledID() const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].connected && fDisplays[i].enabled)
			return fDisplays[i].id;
	}
	return -1;
}


/*!	The union of the enabled displays. */
BRect
DisplayLayoutState::Frame() const
{
	BRect frame(0, 0, -1, -1);
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const display_state& display = fDisplays[i];
		if (!display.connected || !display.enabled)
			continue;
		if (!frame.IsValid())
			frame = display.frame;
		else
			frame = frame | display.frame;
	}
	return frame;
}


/*!	Mirrors what the app_server does with a request: overlapping displays are
	separated by pushing later ones to the right, mirrors take their source's
	place, and the union of the enabled displays is moved to 0,0.
*/
void
DisplayLayoutState::Normalize()
{
	std::vector<display_state*> enabled;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].connected && fDisplays[i].enabled
			&& !fDisplays[i].IsMirror())
			enabled.push_back(&fDisplays[i]);
	}
	std::sort(enabled.begin(), enabled.end(),
		[](const display_state* a, const display_state* b) {
			if (a->frame.left != b->frame.left)
				return a->frame.left < b->frame.left;
			return a->frame.top < b->frame.top;
		});

	for (size_t i = 1; i < enabled.size(); i++) {
		for (size_t j = 0; j < i; j++) {
			BRect common = enabled[i]->frame & enabled[j]->frame;
			if (common.IsValid() && common.Width() >= 0
				&& common.Height() >= 0) {
				float shift = enabled[j]->frame.right + 1
					- enabled[i]->frame.left;
				enabled[i]->frame.OffsetBy(shift, 0);
			}
		}
	}

	_PlaceMirrors();

	BRect frame = Frame();
	if (!frame.IsValid())
		return;
	for (size_t i = 0; i < fDisplays.size(); i++)
		fDisplays[i].frame.OffsetBy(-frame.left, -frame.top);
	fFrame = Frame();
}


/*!	The main display is never a mirror: making a mirror the main display
	makes it the source of its group, which is what the app_server does.
*/
void
DisplayLayoutState::SetPrimary(int32 id)
{
	display_state* display = DisplayByID(id);
	if (display != NULL && display->IsMirror()) {
		display_state* source = DisplayByID(display->mirrorOf);
		display->mirrorOf = -1;
		if (source != NULL) {
			for (size_t i = 0; i < fDisplays.size(); i++) {
				if (fDisplays[i].mirrorOf == source->id)
					fDisplays[i].mirrorOf = id;
			}
			source->mirrorOf = id;
			display->frame.OffsetTo(source->frame.LeftTop());
		}
		display->scale = _ScaleStep(display->scale);
		display->UpdateFrameSize();
		_PlaceMirrors();
	}

	for (size_t i = 0; i < fDisplays.size(); i++)
		fDisplays[i].primary = fDisplays[i].id == id;
}


void
DisplayLayoutState::SetEnabled(int32 id, bool enabled)
{
	display_state* display = DisplayByID(id);
	if (display == NULL)
		return;
	if (!enabled && CountEnabled() <= 1 && display->enabled)
		return;

	display->enabled = enabled;
	if (!enabled) {
		// what mirrored it shows its own part now
		for (size_t i = 0; i < fDisplays.size(); i++) {
			if (fDisplays[i].mirrorOf == id)
				_Unmirror(fDisplays[i]);
		}
		if (display->IsMirror())
			_Unmirror(*display);
	}

	// The primary display must be an enabled one
	if (!enabled && display->primary) {
		int32 first = FirstEnabledID();
		if (first >= 0)
			SetPrimary(first);
	}
	if (enabled && PrimaryID() < 0)
		SetPrimary(id);
}


/*!	Native resolution, no scaling, all connected displays enabled and placed
	side by side, left to right, in connector order.
*/
void
DisplayLayoutState::SetDefaults()
{
	for (size_t i = 0; i < fDisplays.size(); i++)
		fDisplays[i].mirrorOf = -1;

	float left = 0;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		display_state& display = fDisplays[i];
		if (!display.connected)
			continue;

		display.enabled = true;
		display.scale = 100;
		if (display.nativeWidth > 0 && display.nativeHeight > 0) {
			display.modeWidth = display.nativeWidth;
			display.modeHeight = display.nativeHeight;
			display.modeRefresh = display.nativeRefresh;
		}
		display.frame.OffsetTo(left, 0);
		display.UpdateFrameSize();
		left = display.frame.right + 1;
	}

	if (PrimaryID() < 0 && FirstEnabledID() >= 0)
		SetPrimary(FirstEnabledID());

	Normalize();
}


bool
DisplayLayoutState::SameIDs(const DisplayLayoutState& other) const
{
	if (fDisplays.size() != other.fDisplays.size())
		return false;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].id != other.fDisplays[i].id
			|| fDisplays[i].connected != other.fDisplays[i].connected)
			return false;
	}
	return true;
}


/*!	Whether applying this layout instead of \a other would change anything
	on the screen.
*/
bool
DisplayLayoutState::SameArrangement(const DisplayLayoutState& other) const
{
	if (!SameIDs(other))
		return false;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (!fDisplays[i].connected)
			continue;
		if (!fDisplays[i].SameSettings(other.fDisplays[i]))
			return false;
	}
	return true;
}


/*!	Builds the request for set_display_layout() that reproduces this layout.
*/
void
DisplayLayoutState::BuildRequest(BMessage& request) const
{
	request.MakeEmpty();
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const display_state& display = fDisplays[i];
		if (!display.connected)
			continue;

		BMessage entry;
		entry.AddInt32("id", display.id);
		entry.AddBool("enabled", display.enabled);
		entry.AddInt32("mirror", display.mirrorOf);
		entry.AddRect("frame", display.frame);
		if (!display.IsMirror())
			entry.AddInt32("scale", display.scale);
		if (display.modeWidth > 0 && display.modeHeight > 0) {
			entry.AddInt32("mode width", display.modeWidth);
			entry.AddInt32("mode height", display.modeHeight);
			entry.AddFloat("mode refresh", display.modeRefresh);
		}
		if (display.primary)
			entry.AddBool("primary", true);
		request.AddMessage("display", &entry);
	}
}


//	#pragma mark - mirroring


/*!	Mirroring needs a driver that arranges the displays, and two of them. */
bool
DisplayLayoutState::CanMirror() const
{
	return fHasLayout && CountConnected() >= 2;
}


int32
DisplayLayoutState::MirrorState() const
{
	int32 primary = PrimaryID();
	int32 others = 0;
	int32 mirrors = 0;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const display_state& display = fDisplays[i];
		if (!display.connected || !display.enabled || display.id == primary)
			continue;
		others++;
		if (display.mirrorOf == primary)
			mirrors++;
	}
	if (mirrors == 0)
		return B_CONTROL_OFF;
	return mirrors == others ? B_CONTROL_ON : B_CONTROL_PARTIALLY_ON;
}


/*!	With \a mirrored, every connected display shows what the main display
	shows. A display with fewer pixels than the main display's part of the
	desktop cannot do that (the display engine only enlarges): the main
	display is then scaled up until it fits, or, failing that, all of them
	are set to a resolution they share.
	Without, every mirror shows its own part again, to the right of the
	others.
*/
void
DisplayLayoutState::SetMirrored(bool mirrored)
{
	if (!mirrored) {
		for (size_t i = 0; i < fDisplays.size(); i++) {
			if (fDisplays[i].IsMirror())
				_Unmirror(fDisplays[i]);
		}
		Normalize();
		return;
	}

	display_state* source = DisplayByID(PrimaryID());
	if (source == NULL)
		return;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		display_state& display = fDisplays[i];
		if (!display.connected || display.id == source->id)
			continue;
		display.enabled = true;
		display.mirrorOf = source->id;
	}

	if (!_MirrorsFit(*source, source->scale)) {
		bool fixed = false;
		for (size_t i = 0; i < fScales.size() && !fixed; i++) {
			if (fScales[i] > source->scale && _MirrorsFit(*source, fScales[i])) {
				source->scale = fScales[i];
				source->UpdateFrameSize();
				fixed = true;
			}
		}
		if (!fixed)
			_SetCommonMode(*source);
	}

	Normalize();
}


/*!	A mirror that would have to show more pixels than it has; the app_server
	refuses such a layout.
*/
bool
DisplayLayoutState::FindTooSmallMirror(int32& mirrorID, int32& sourceID) const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const display_state& display = fDisplays[i];
		if (!display.connected || !display.enabled || !display.IsMirror())
			continue;
		const display_state* source = DisplayByID(display.mirrorOf);
		if (source != NULL && _MirrorScale(display, *source) < 100) {
			mirrorID = display.id;
			sourceID = source->id;
			return true;
		}
	}
	return false;
}


/*!	Every mirror takes its source's frame, and the scale that fits it, the
	way the app_server works them out.
*/
void
DisplayLayoutState::_PlaceMirrors()
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		display_state& display = fDisplays[i];
		if (!display.IsMirror())
			continue;
		const display_state* source = DisplayByID(display.mirrorOf);
		if (source == NULL || source->IsMirror() || !source->enabled) {
			_Unmirror(display);
			continue;
		}
		display.frame = source->frame;
		display.scale = std::max((int32)100, _MirrorScale(display, *source));
	}
}


/*!	The display shows its own part of the desktop again, at the size things
	had on it, to the right of the displays that are not mirrors.
*/
void
DisplayLayoutState::_Unmirror(display_state& display)
{
	display.mirrorOf = -1;
	display.scale = _ScaleStep(display.scale);

	float right = 0;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const display_state& other = fDisplays[i];
		if (&other != &display && other.connected && other.enabled
			&& !other.IsMirror())
			right = std::max(right, other.frame.right + 1);
	}
	display.frame.OffsetTo(right, 0);
	display.UpdateFrameSize();
}


bool
DisplayLayoutState::_MirrorsFit(const display_state& source,
	int32 scale) const
{
	display_state scaled = source;
	scaled.scale = scale;
	scaled.UpdateFrameSize();
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const display_state& display = fDisplays[i];
		if (display.connected && display.enabled
			&& display.mirrorOf == source.id
			&& _MirrorScale(display, scaled) < 100)
			return false;
	}
	return true;
}


/*!	Sets the source and its mirrors to the largest resolution all of them
	offer, at 100%.
*/
bool
DisplayLayoutState::_SetCommonMode(display_state& source)
{
	int32 bestWidth = 0;
	int32 bestHeight = 0;
	for (size_t i = 0; i < source.modes.size(); i++) {
		const display_mode_entry& mode = source.modes[i];
		if ((int64)mode.width * mode.height <= (int64)bestWidth * bestHeight)
			continue;
		bool everywhere = true;
		for (size_t j = 0; j < fDisplays.size() && everywhere; j++) {
			const display_state& display = fDisplays[j];
			if (display.connected && display.mirrorOf == source.id
				&& !display.HasMode(mode.width, mode.height))
				everywhere = false;
		}
		if (everywhere) {
			bestWidth = mode.width;
			bestHeight = mode.height;
		}
	}
	if (bestWidth == 0)
		return false;

	for (size_t i = 0; i < fDisplays.size(); i++) {
		display_state& display = fDisplays[i];
		if (display.id != source.id && display.mirrorOf != source.id)
			continue;
		// the rate it has if it offers it there, else the highest
		float refresh = 0;
		bool keep = false;
		for (size_t j = 0; j < display.modes.size(); j++) {
			const display_mode_entry& mode = display.modes[j];
			if (mode.width != bestWidth || mode.height != bestHeight)
				continue;
			if (refresh_rates_equal(mode.refresh, display.modeRefresh))
				keep = true;
			refresh = std::max(refresh, mode.refresh);
		}
		display.modeWidth = bestWidth;
		display.modeHeight = bestHeight;
		if (!keep)
			display.modeRefresh = refresh;
		display.scale = 100;
		display.UpdateFrameSize();
	}
	return true;
}


/*!	How much \a mirror enlarges its source's part of the desktop, the way
	the app_server works it out: a monitor like the source's gets its scale,
	another as much as fits both ways.
*/
/*static*/ int32
DisplayLayoutState::_MirrorScale(const display_state& mirror,
	const display_state& source)
{
	if (mirror.modeWidth == source.modeWidth
		&& mirror.modeHeight == source.modeHeight)
		return source.scale;

	float width = source.frame.Width() + 1;
	float height = source.frame.Height() + 1;
	if (width < 1 || height < 1 || mirror.modeWidth <= 0
		|| mirror.modeHeight <= 0)
		return source.scale;

	float fit = std::min(mirror.modeWidth / width, mirror.modeHeight / height);
	return (int32)std::min(400.0f, floorf(fit * 100 + 0.5f));
}


/*!	The largest scale one can choose that is not above \a scale. */
int32
DisplayLayoutState::_ScaleStep(int32 scale) const
{
	int32 step = fScales.empty() ? 100 : fScales[0];
	for (size_t i = 0; i < fScales.size(); i++) {
		if (fScales[i] <= scale)
			step = fScales[i];
	}
	return step;
}
