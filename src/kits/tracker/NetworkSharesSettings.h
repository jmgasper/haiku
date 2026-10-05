/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NETWORK_SHARES_SETTINGS_H
#define _NETWORK_SHARES_SETTINGS_H


#include <Message.h>
#include <Messenger.h>
#include <VolumeRoster.h>
#include <Window.h>

#include "SettingsViews.h"


class BButton;
class BCheckBox;
class BListView;
class BStringView;
class BTextControl;


namespace BPrivate {

/*!	The page of Tracker's preferences for the shares of other machines:
	which there are, and whether they are mounted when the system starts.
	The mount server has the list and does the mounting, this shows it and
	tells it what to do.

	Nothing here waits for the mount server. It may be waiting for a server
	itself, and says when it is done.
*/
class NetworkSharesSettingsView : public SettingsView {
public:
								NetworkSharesSettingsView();
	virtual						~NetworkSharesSettingsView();

	virtual	bool				IsDefaultable() const;
	virtual	bool				IsRevertable() const;
	virtual	void				ShowCurrentSettings();

protected:
	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_Refresh();
			void				_Send(uint32 what, int32 id);
			void				_ShowShares(const BMessage* reply);
			void				_ShowSelection();
			void				_HandleReply(const BMessage* reply);
			void				_EditShare(bool add);

			bool				_SelectedShare(BMessage& share) const;

	static	bool				_IsInstalled();

private:
			BListView*			fListView;
			BButton*			fAddButton;
			BButton*			fEditButton;
			BButton*			fRemoveButton;
			BButton*			fMountButton;
			BStringView*		fStatusView;

			BMessenger			fTarget;
			BVolumeRoster		fVolumeRoster;
			BMessage			fShares;
			int32				fBusy;
				// the share the mount server is asked to do something
				// with, if any
			BString				fBusyText;

			typedef SettingsView _inherited;
};


/*!	Asks for what there is to a share. What is entered is sent to the mount
	server, and the answer to that to \a target.
*/
class NetworkShareWindow : public BWindow {
public:
								NetworkShareWindow(BWindow* parent,
									const BMessage* share,
									const BMessenger& target);

	virtual	void				MessageReceived(BMessage* message);

private:
			void				_Save();
			void				_UpdateSaveButton();

private:
			BTextControl*		fServerControl;
			BTextControl*		fShareControl;
			BTextControl*		fNameControl;
			BTextControl*		fUserControl;
			BTextControl*		fPasswordControl;
			BTextControl*		fDomainControl;
			BCheckBox*			fStartupCheckBox;
			BCheckBox*			fReadOnlyCheckBox;
			BCheckBox*			fEncryptCheckBox;
			BButton*			fSaveButton;

			BMessenger			fTarget;
			int32				fID;
			BString				fPath;
			bool				fNameIsShare;
			bool				fKeepPassword;
				// what the password field shows stands for a password
				// that is stored
			bool				fPasswordEdited;

			typedef BWindow _inherited;
};

} // namespace BPrivate

using namespace BPrivate;


#endif	// _NETWORK_SHARES_SETTINGS_H
