/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef NETWORK_LIST_VIEW_H
#define NETWORK_LIST_VIEW_H


#include <ListItem.h>
#include <ListView.h>
#include <View.h>

#include <vector>

#include "WiFiController.h"


class NetworkListItem : public BListItem {
public:
								NetworkListItem(const WiFiNetworkInfo& info);

			const WiFiNetworkInfo& Info() const { return fInfo; }
			void				SetInfo(const WiFiNetworkInfo& info)
									{ fInfo = info; }
			void				SetShowAvailability(bool show)
									{ fShowAvailability = show; }

	virtual	void				DrawItem(BView* owner, BRect frame,
									bool complete = false);
	virtual	void				Update(BView* owner, const BFont* font);

private:
			WiFiNetworkInfo		fInfo;
			float				fBaselineOffset;
			bool				fShowAvailability;
};


class NetworkListView : public BListView {
public:
								NetworkListView(const char* name);

			void				SetNetworks(
									const std::vector<WiFiNetworkInfo>& list);
			const WiFiNetworkInfo* SelectedNetwork() const;
			using BListView::Select;
			bool				Select(const char* name);
			void				SetShowAvailability(bool show);

	virtual	bool				GetToolTipAt(BPoint point, BToolTip** _tip);

private:
			std::vector<WiFiNetworkInfo> fNetworks;
			bool				fShowAvailability;
};


class SignalView : public BView {
public:
								SignalView(const char* name);

			void				SetSignal(int32 bars, bool enabled,
									bool animate);

	virtual	void				Draw(BRect updateRect);
	virtual	void				Pulse();
	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();

private:
			int32				fBars;
			bool				fEnabled;
			bool				fAnimate;
			int32				fPhase;
};


BString NetworkToolTip(const WiFiNetworkInfo& info);


#endif	// NETWORK_LIST_VIEW_H
