/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef DISPLAY_LAYOUT_STATE_H
#define DISPLAY_LAYOUT_STATE_H


#include <vector>

#include <Message.h>
#include <Rect.h>
#include <String.h>


struct display_mode_entry {
	int32			width;
	int32			height;
	float			refresh;
};


/*!	Everything the app_server tells us about one monitor, plus the settings
	the user may edit before they are applied.
*/
struct display_state {
	int32			id;
	BString			name;			// connector, e.g. "DP-2"
	BString			key;
	BString			monitor;		// name from EDID, may be empty
	BString			vendor;			// PNP id, e.g. "DEL"
	BString			serial;
	int32			productID;
	int32			week;
	int32			year;
	float			widthCM;
	float			heightCM;
	bool			hasEDID;
	int32			flags;
	bool			connected;
	bool			enabled;
	bool			primary;
	BRect			frame;			// desktop (logical) coordinates
	int32			scale;			// percent
	int32			nativeWidth;
	int32			nativeHeight;
	float			nativeRefresh;
	int32			modeWidth;
	int32			modeHeight;
	float			modeRefresh;
	std::vector<display_mode_entry> modes;

					display_state();

			status_t	SetTo(const BMessage& message);
			void		UpdateFrameSize();
			bool		SameSettings(const display_state& other) const;
			bool		HasMode(int32 width, int32 height) const;
			float		DiagonalInches() const;
			int32		DPI() const;
};


class DisplayLayoutState {
public:
								DisplayLayoutState();

			status_t			Load();
			status_t			SetTo(const BMessage& layout);
			void				SetToSingleDisplay(BRect frame, int32 width,
									int32 height, float refresh);

			int32				CountDisplays() const;
			display_state*		DisplayAt(int32 index);
			const display_state* DisplayAt(int32 index) const;
			display_state*		DisplayByID(int32 id);
			const display_state* DisplayByID(int32 id) const;
			int32				IndexOf(int32 id) const;
			int32				CountEnabled() const;
			int32				PrimaryID() const;
			int32				FirstEnabledID() const;

			bool				HasLayout() const { return fHasLayout; }
			bool				ZoomToDisplay() const
									{ return fZoomToDisplay; }
			BRect				ScreenFrame() const { return fScreenFrame; }
			const std::vector<int32>& Scales() const { return fScales; }

			BRect				Frame() const;
			void				Normalize();
			void				SetPrimary(int32 id);
			void				SetEnabled(int32 id, bool enabled);
			void				SetDefaults();

			bool				SameIDs(const DisplayLayoutState& other) const;
			bool				SameArrangement(
									const DisplayLayoutState& other) const;

			void				BuildRequest(BMessage& request) const;

private:
			std::vector<display_state> fDisplays;
			std::vector<int32>	fScales;
			BRect				fFrame;
			BRect				fScreenFrame;
			bool				fHasLayout;
			bool				fZoomToDisplay;
};


bool refresh_rates_equal(float a, float b);


#endif	// DISPLAY_LAYOUT_STATE_H
