/*
 * Copyright 2009, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * Copyright 2026, Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef DEVICELISTITEM_H_
#define DEVICELISTITEM_H_


#include <ListItem.h>
#include <ListView.h>
#include <String.h>

#include "DeviceModel.h"


// A two-line row: icon, device name, a secondary line, and on the right
// either a status text or signal-strength bars.
class DeviceItem : public BListItem {
public:
								DeviceItem(device_key key);

			device_key			Key() const { return fKey; }

			// Returns true when anything visible changed.
			bool				SetContent(device_kind kind,
									const BString& name,
									const BString& detail,
									const BString& status,
									int32 signalBars);

	virtual	void				DrawItem(BView* owner, BRect frame,
									bool complete = false);
	virtual	void				Update(BView* owner, const BFont* font);

private:
			void				_DrawSignalBars(BView* owner, BRect frame,
									rgb_color textColor,
									rgb_color background);

			device_key			fKey;
			device_kind			fKind;
			BString				fName;
			BString				fDetail;
			BString				fStatus;
			int32				fSignalBars;
};


// A list that shows a hint text while it is empty.
class DeviceListView : public BListView {
public:
								DeviceListView(const char* name);

			using BListView::AddItem;
			using BListView::RemoveItem;

	virtual	bool				AddItem(BListItem* item);
	virtual	bool				RemoveItem(BListItem* item);
	virtual	void				MakeEmpty();

			void				SetEmptyText(const char* text);
			DeviceItem*			SelectedDevice() const;

	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);

private:
			BString				fEmptyText;
};


#endif	// DEVICELISTITEM_H_
