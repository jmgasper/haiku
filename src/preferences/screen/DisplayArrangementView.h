/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef DISPLAY_ARRANGEMENT_VIEW_H
#define DISPLAY_ARRANGEMENT_VIEW_H


#include <vector>

#include <String.h>
#include <View.h>


class DisplayLayoutState;


/*!	Shows the connected displays as they are arranged on the desktop and
	lets the user drag them around. Changes are reported to the window with
	kMsgDisplayMoved; clicks with kMsgDisplaySelected.
	Displays that mirror another are drawn with it, as one display carrying
	all their numbers; clicking it again selects the next of them.
*/
class DisplayArrangementView : public BView {
public:
								DisplayArrangementView(const char* name);
	virtual						~DisplayArrangementView();

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MessageReceived(BMessage* message);

	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();

			void				SetDisplays(const DisplayLayoutState& state);
			void				SetSelectedID(int32 id);
			int32				SelectedID() const { return fSelectedID; }
			void				SetDraggingEnabled(bool enabled);

private:
			struct entry {
				int32			id;
				int32			number;
				BString			label;
				BRect			frame;
				bool			enabled;
				bool			primary;
				int32			mirrorOf;
			};

			void				_UpdateScale();
			BRect				_ViewRect(const BRect& frame) const;
			BRect				_EntryRect(int32 index) const;
			BRect				_NumberRect(int32 index) const;
			int32				_EntryAt(BPoint where) const;
			bool				_InGroup(int32 index, int32 id) const;
			std::vector<int32>	_Group(int32 index) const;
			BRect				_Snap(const BRect& frame, int32 index) const;
			void				_DrawEntry(int32 index,
									const BRect& updateRect);

private:
			std::vector<entry>	fEntries;
			int32				fSelectedID;
			bool				fDraggingEnabled;

			float				fScale;
			BPoint				fOrigin;

			int32				fDragIndex;
			BPoint				fDragStart;
			BRect				fDragOriginalFrame;
			bool				fDragMoved;
			bool				fCycleOnClick;
};


#endif	// DISPLAY_ARRANGEMENT_VIEW_H
