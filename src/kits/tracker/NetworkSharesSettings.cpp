/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "NetworkSharesSettings.h"

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <Locale.h>
#include <NodeMonitor.h>
#include <PathFinder.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringList.h>
#include <StringView.h>
#include <TextControl.h>
#include <VolumeRoster.h>

#include <MountServer.h>

#include "Commands.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "NetworkSharesSettings"


static const uint32 kShareSelected = 'NSsl';
static const uint32 kShareInvoked = 'NSiv';
static const uint32 kAddShare = 'NSad';
static const uint32 kEditShare = 'NSed';
static const uint32 kRemoveShare = 'NSrm';
static const uint32 kMountShare = 'NSmt';

static const uint32 kSaveShare = 'NSsv';
static const uint32 kShareEdited = 'NSch';
static const uint32 kShareNameEdited = 'NSnm';
static const uint32 kPasswordEdited = 'NSpw';

// What stands in a password field for a password that is stored, and that
// nobody here gets to see.
static const char* kStoredPassword = "\x01\x01\x01\x01\x01\x01\x01\x01";


namespace BPrivate {

class ShareItem : public BListItem {
public:
								ShareItem(const BMessage& share);

	virtual	void				DrawItem(BView* owner, BRect frame,
									bool complete);
	virtual	void				Update(BView* owner, const BFont* font);

			int32				ID() const
									{ return fID; }

private:
			int32				fID;
			BString				fName;
			BString				fAddress;
			BString				fState;
			bool				fFailed;
			float				fBaseline;
			float				fLineHeight;
};

}	// namespace BPrivate


/*!	What to tell someone whose share could not be mounted. The error alone
	is what the file system made of what the server said, and reads as if it
	were about a file.
*/
static BString
describe_error(status_t error)
{
	switch (error) {
		case B_PERMISSION_DENIED:
		case B_NOT_ALLOWED:
			return B_TRANSLATE("The server did not accept the user name "
				"or the password.");
		case B_ENTRY_NOT_FOUND:
			return B_TRANSLATE("The server has no such share.");
		case B_NOT_A_DIRECTORY:
			return B_TRANSLATE("The share has no such folder.");
		case B_DEVICE_NOT_FOUND:
		case B_NAME_NOT_FOUND:
			return B_TRANSLATE("The file system for network shares is not "
				"installed.");
		case B_TIMED_OUT:
			return B_TRANSLATE("The server did not answer.");
		case B_BUSY:
			return B_TRANSLATE("The share is in use.");
	}

	BString text(B_TRANSLATE("The server could not be reached: %error%."));
	text.ReplaceFirst("%error%", strerror(error));
	return text;
}


//	#pragma mark - ShareItem


ShareItem::ShareItem(const BMessage& share)
	:
	fID(share.GetInt32("id", -1)),
	fName(share.GetString("name", "")),
	fFailed(false),
	fBaseline(0),
	fLineHeight(0)
{
	fAddress.SetToFormat("//%s/%s", share.GetString("server", ""),
		share.GetString("share", ""));
	const char* path = share.GetString("path", "");
	if (path[0] != '\0')
		fAddress << "/" << path;

	const status_t error = share.GetInt32("error", B_OK);
	if (share.GetBool("mounted", false))
		fState = B_TRANSLATE("mounted");
	else if (error != B_OK) {
		fState = B_TRANSLATE("failed");
		fFailed = true;
	} else
		fState = B_TRANSLATE("not mounted");
}


void
ShareItem::Update(BView* owner, const BFont* font)
{
	BListItem::Update(owner, font);

	font_height height;
	font->GetHeight(&height);
	fLineHeight = ceilf(height.ascent + height.descent + height.leading);
	fBaseline = ceilf(height.ascent);

	SetHeight(2 * fLineHeight + be_control_look->DefaultLabelSpacing());
}


void
ShareItem::DrawItem(BView* owner, BRect frame, bool complete)
{
	const rgb_color background = ui_color(IsSelected()
		? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
	const rgb_color text = ui_color(IsSelected()
		? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);

	owner->SetLowColor(background);
	owner->FillRect(frame, B_SOLID_LOW);

	const float spacing = be_control_look->DefaultLabelSpacing();
	const float top = frame.top + floorf(spacing / 2);

	BFont font;
	owner->GetFont(&font);

	// what it is called, and how it is doing
	BFont bold(font);
	bold.SetFace(B_BOLD_FACE);
	owner->SetFont(&bold);
	owner->SetHighColor(text);

	const float stateWidth = font.StringWidth(fState.String());
	BString name(fName);
	bold.TruncateString(&name, B_TRUNCATE_END,
		frame.Width() - 4 * spacing - stateWidth);
	owner->DrawString(name.String(),
		BPoint(frame.left + spacing, top + fBaseline));

	owner->SetFont(&font);
	if (fFailed)
		owner->SetHighColor(ui_color(B_FAILURE_COLOR));
	else
		owner->SetHighColor(mix_color(text, background, 96));
	owner->DrawString(fState.String(),
		BPoint(frame.right - spacing - stateWidth, top + fBaseline));

	// where it is
	BString address(fAddress);
	font.TruncateString(&address, B_TRUNCATE_MIDDLE,
		frame.Width() - 2 * spacing);
	owner->SetHighColor(mix_color(text, background, 96));
	owner->DrawString(address.String(),
		BPoint(frame.left + spacing, top + fLineHeight + fBaseline));

	owner->SetHighColor(text);
}


//	#pragma mark - NetworkSharesSettingsView


NetworkSharesSettingsView::NetworkSharesSettingsView()
	:
	SettingsView("NetworkSharesSettingsView"),
	fTarget(kMountServerSignature),
	fBusy(-1)
{
	fListView = new BListView("shares", B_SINGLE_SELECTION_LIST);
	fListView->SetSelectionMessage(new BMessage(kShareSelected));
	fListView->SetInvocationMessage(new BMessage(kShareInvoked));

	BScrollView* scrollView = new BScrollView("scroll", fListView, 0, false,
		true);

	// room for a few, however many there are
	font_height height;
	be_plain_font->GetHeight(&height);
	const float lineHeight = ceilf(height.ascent + height.descent
		+ height.leading);
	scrollView->SetExplicitMinSize(BSize(be_plain_font->StringWidth("n") * 46,
		(2 * lineHeight + be_control_look->DefaultLabelSpacing()) * 4));

	fAddButton = new BButton("add", B_TRANSLATE("Add" B_UTF8_ELLIPSIS),
		new BMessage(kAddShare));
	fEditButton = new BButton("edit", B_TRANSLATE("Edit" B_UTF8_ELLIPSIS),
		new BMessage(kEditShare));
	fRemoveButton = new BButton("remove", B_TRANSLATE("Remove"),
		new BMessage(kRemoveShare));
	fMountButton = new BButton("mount", B_TRANSLATE("Unmount"),
		new BMessage(kMountShare));
		// the longer of the two it says, for the width

	fStatusView = new BStringView("status", "");
	fStatusView->SetTruncation(B_TRUNCATE_END);
	fStatusView->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
	fStatusView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	const float spacing = be_control_look->DefaultItemSpacing();

	BLayoutBuilder::Group<>(this, B_VERTICAL, spacing / 2)
		.Add(scrollView)
		.Add(fStatusView)
		.AddGroup(B_HORIZONTAL, spacing / 2)
			.Add(fAddButton)
			.Add(fEditButton)
			.Add(fRemoveButton)
			.AddGlue()
			.Add(fMountButton)
			.End()
		.SetInsets(spacing);

	_ShowSelection();
}


NetworkSharesSettingsView::~NetworkSharesSettingsView()
{
}


bool
NetworkSharesSettingsView::IsDefaultable() const
{
	return false;
}


bool
NetworkSharesSettingsView::IsRevertable() const
{
	// what is done here is done, to the shares too
	return false;
}


void
NetworkSharesSettingsView::ShowCurrentSettings()
{
	_Refresh();
}


void
NetworkSharesSettingsView::AttachedToWindow()
{
	fListView->SetTarget(this);
	fAddButton->SetTarget(this);
	fEditButton->SetTarget(this);
	fRemoveButton->SetTarget(this);
	fMountButton->SetTarget(this);

	// volumes come and go without this having to do with it
	fVolumeRoster.StartWatching(BMessenger(this));

	_Refresh();
}


void
NetworkSharesSettingsView::DetachedFromWindow()
{
	fVolumeRoster.StopWatching();
}


void
NetworkSharesSettingsView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kNetworkShareReply:
			_HandleReply(message);
			break;

		case B_NODE_MONITOR:
			_Refresh();
			break;

		case kShareSelected:
			_ShowSelection();
			break;

		case kAddShare:
			_EditShare(true);
			break;

		case kShareInvoked:
		case kEditShare:
			_EditShare(false);
			break;

		case kRemoveShare:
		{
			BMessage share;
			if (!_SelectedShare(share))
				break;

			BString text(B_TRANSLATE("Remove the share \"%name%\"?"));
			text.ReplaceFirst("%name%", share.GetString("name", ""));
			if (share.GetBool("mounted", false)) {
				text << "\n\n"
					<< B_TRANSLATE("It is mounted, and will be unmounted.");
			}

			BAlert* alert = new BAlert(B_TRANSLATE("Remove share"), text,
				B_TRANSLATE("Cancel"), B_TRANSLATE("Remove"), NULL,
				B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			if (alert->Go() == 1)
				_Send(kRemoveNetworkShare, share.GetInt32("id", -1));
			break;
		}

		case kMountShare:
		{
			BMessage share;
			if (!_SelectedShare(share))
				break;

			const bool mounted = share.GetBool("mounted", false);
			fBusyText = mounted ? B_TRANSLATE("Unmounting" B_UTF8_ELLIPSIS)
				: B_TRANSLATE("Waiting for the server" B_UTF8_ELLIPSIS);
			_Send(mounted ? kUnmountNetworkShare : kMountNetworkShare,
				share.GetInt32("id", -1));
			break;
		}

		default:
			_inherited::MessageReceived(message);
			break;
	}
}


void
NetworkSharesSettingsView::_Refresh()
{
	// the answer is for this, which has to be where answers get to
	if (Window() == NULL)
		return;

	BMessage request(kGetNetworkShares);
	fTarget.SendMessage(&request, this);
}


void
NetworkSharesSettingsView::_Send(uint32 what, int32 id)
{
	BMessage request(what);
	request.AddInt32("id", id);

	if (fTarget.SendMessage(&request, this) != B_OK) {
		fStatusView->SetText(
			B_TRANSLATE("The mount server could not be contacted."));
		return;
	}

	fBusy = id;
	_ShowSelection();
}


void
NetworkSharesSettingsView::_HandleReply(const BMessage* reply)
{
	const uint32 request = reply->GetInt32("request", 0);
	if (request == kGetNetworkShares) {
		_ShowShares(reply);
		return;
	}

	fBusy = -1;
	fBusyText = "";

	const status_t error = reply->GetInt32("error", B_OK);
	if (error != B_OK) {
		BString text;
		switch (request) {
			case kMountNetworkShare:
				text = B_TRANSLATE("\"%name%\" could not be mounted.");
				break;
			case kUnmountNetworkShare:
			case kRemoveNetworkShare:
				text = B_TRANSLATE("\"%name%\" could not be unmounted.");
				break;
			default:
				text = B_TRANSLATE("The share could not be saved.");
				break;
		}
		text.ReplaceFirst("%name%", reply->GetString("name", ""));

		if (request == kMountNetworkShare)
			text << "\n\n" << describe_error(error);
		else
			text << "\n\n" << strerror(error);

		BAlert* alert = new BAlert(B_TRANSLATE("Network shares"), text,
			B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go(NULL);
	}

	// select what was added
	if (request == kSetNetworkShare && error == B_OK)
		fShares.SetInt32("select", reply->GetInt32("id", -1));

	_Refresh();
}


void
NetworkSharesSettingsView::_ShowShares(const BMessage* reply)
{
	int32 selected = fShares.GetInt32("select", -1);
	if (selected < 0) {
		ShareItem* item = dynamic_cast<ShareItem*>(
			fListView->ItemAt(fListView->CurrentSelection()));
		if (item != NULL)
			selected = item->ID();
	}

	fShares = *reply;

	fListView->SetSelectionMessage(NULL);
		// none of what follows is someone selecting something
	while (BListItem* item = fListView->RemoveItem((int32)0))
		delete item;

	BMessage share;
	for (int32 i = 0; fShares.FindMessage("share", i, &share) == B_OK; i++) {
		ShareItem* item = new ShareItem(share);
		fListView->AddItem(item);
		if (item->ID() == selected)
			fListView->Select(i);
	}

	fListView->SetSelectionMessage(new BMessage(kShareSelected));
	fListView->SetTarget(this);

	_ShowSelection();
}


/*!	Has the buttons and the line below the list go with the share that is
	selected.
*/
void
NetworkSharesSettingsView::_ShowSelection()
{
	BMessage share;
	const bool selected = _SelectedShare(share);
	const bool mounted = share.GetBool("mounted", false);
	const bool busy = fBusy >= 0;

	fEditButton->SetEnabled(selected && !busy);
	fRemoveButton->SetEnabled(selected && !busy);
	fMountButton->SetEnabled(selected && !busy);
	fMountButton->SetLabel(mounted
		? B_TRANSLATE("Unmount") : B_TRANSLATE("Mount"));

	BString status;
	if (busy)
		status = fBusyText;
	else if (!_IsInstalled()) {
		status = B_TRANSLATE("The file system for network shares (smbfs) is "
			"not installed.");
	} else if (!selected) {
		if (fListView->IsEmpty()) {
			status = B_TRANSLATE("For shares of Windows, Samba and NAS "
				"servers.");
		}
	} else if (mounted) {
		status = B_TRANSLATE("Mounted at %path%.");
		status.ReplaceFirst("%path%", share.GetString("mount point", ""));
		if (share.GetBool("mount at startup", false)) {
			status << " "
				<< B_TRANSLATE("Is mounted when the system starts.");
		}
	} else if (share.GetInt32("error", B_OK) != B_OK)
		status = describe_error(share.GetInt32("error", B_OK));
	else if (share.GetBool("mount at startup", false))
		status = B_TRANSLATE("Is mounted when the system starts.");
	else
		status = B_TRANSLATE("Is mounted when asked to.");

	fStatusView->SetText(status);
}


void
NetworkSharesSettingsView::_EditShare(bool add)
{
	BMessage share;
	if (!add && (!_SelectedShare(share) || fBusy >= 0))
		return;

	fBusyText = B_TRANSLATE("Waiting for the server" B_UTF8_ELLIPSIS);

	NetworkShareWindow* window = new NetworkShareWindow(Window(),
		add ? NULL : &share, BMessenger(this));
	window->Show();
}


bool
NetworkSharesSettingsView::_SelectedShare(BMessage& share) const
{
	ShareItem* item = dynamic_cast<ShareItem*>(
		fListView->ItemAt(fListView->CurrentSelection()));
	if (item == NULL)
		return false;

	for (int32 i = 0; fShares.FindMessage("share", i, &share) == B_OK; i++) {
		if (share.GetInt32("id", -1) == item->ID())
			return true;
	}

	share.MakeEmpty();
	return false;
}


/*static*/ bool
NetworkSharesSettingsView::_IsInstalled()
{
	BStringList paths;
	return BPathFinder::FindPaths(B_FIND_PATH_ADD_ONS_DIRECTORY,
			"userlandfs/smbfs", B_FIND_PATH_EXISTING_ONLY, paths) == B_OK
		&& !paths.IsEmpty();
}


//	#pragma mark - NetworkShareWindow


NetworkShareWindow::NetworkShareWindow(BWindow* parent, const BMessage* share,
	const BMessenger& target)
	:
	BWindow(BRect(0, 0, 100, 100),
		share != NULL ? B_TRANSLATE("Edit network share")
			: B_TRANSLATE("Add network share"),
		B_FLOATING_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE),
	fTarget(target),
	fID(-1),
	fNameIsShare(true),
	fKeepPassword(false),
	fPasswordEdited(false)
{
	fServerControl = new BTextControl("server", B_TRANSLATE("Server:"), "",
		NULL);
	fShareControl = new BTextControl("share", B_TRANSLATE("Share:"), "",
		NULL);
	fNameControl = new BTextControl("name", B_TRANSLATE("Volume name:"), "",
		NULL);
	fUserControl = new BTextControl("user", B_TRANSLATE("User name:"), "",
		NULL);
	fPasswordControl = new BTextControl("password", B_TRANSLATE("Password:"),
		"", NULL);
	fPasswordControl->TextView()->HideTyping(true);
	fDomainControl = new BTextControl("domain", B_TRANSLATE("Domain:"), "",
		NULL);

	fServerControl->SetToolTip(B_TRANSLATE("The name or the address of the "
		"machine that has the share."));
	fShareControl->SetToolTip(B_TRANSLATE("The name of the share. A folder "
		"of it can follow, as in Music/Albums, to mount only that."));
	fUserControl->SetToolTip(B_TRANSLATE("Empty to be the server's guest."));
	fDomainControl->SetToolTip(B_TRANSLATE("The domain or work group of "
		"the user, which few servers need to be told."));

	fStartupCheckBox = new BCheckBox("startup",
		B_TRANSLATE("Mount when the system starts"), NULL);
	fReadOnlyCheckBox = new BCheckBox("read only",
		B_TRANSLATE("Read-only"), NULL);
	fEncryptCheckBox = new BCheckBox("encrypt",
		B_TRANSLATE("Encrypt what goes over the network"), NULL);
	fEncryptCheckBox->SetToolTip(B_TRANSLATE("Takes a server that talks "
		"SMB3."));

	fStartupCheckBox->SetValue(B_CONTROL_ON);

	if (share != NULL) {
		fID = share->GetInt32("id", -1);
		fPath = share->GetString("path", "");

		BString shareName(share->GetString("share", ""));
		fNameIsShare = shareName == share->GetString("name", "");
		if (!fPath.IsEmpty())
			shareName << "/" << fPath;

		fServerControl->SetText(share->GetString("server", ""));
		fShareControl->SetText(shareName);
		fNameControl->SetText(share->GetString("name", ""));
		fUserControl->SetText(share->GetString("user", ""));
		fDomainControl->SetText(share->GetString("domain", ""));

		if (share->GetBool("has password", false)) {
			fPasswordControl->SetText(kStoredPassword);
			fKeepPassword = true;
		}

		fStartupCheckBox->SetValue(
			share->GetBool("mount at startup", true));
		fReadOnlyCheckBox->SetValue(share->GetBool("read only", false));
		fEncryptCheckBox->SetValue(share->GetBool("encrypt", false));
	}

	fServerControl->SetModificationMessage(new BMessage(kShareEdited));
	fShareControl->SetModificationMessage(new BMessage(kShareEdited));
	fNameControl->SetModificationMessage(new BMessage(kShareNameEdited));
	fPasswordControl->SetModificationMessage(new BMessage(kPasswordEdited));

	BButton* cancelButton = new BButton("cancel", B_TRANSLATE("Cancel"),
		new BMessage(B_QUIT_REQUESTED));
	fSaveButton = new BButton("save",
		share != NULL ? B_TRANSLATE("Save") : B_TRANSLATE("Add"),
		new BMessage(kSaveShare));

	// wide enough for an address
	fServerControl->CreateTextViewLayoutItem()->SetExplicitMinSize(
		BSize(be_plain_font->StringWidth("n") * 30, B_SIZE_UNSET));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.AddGrid(B_USE_HALF_ITEM_SPACING, B_USE_HALF_ITEM_SPACING)
			.AddTextControl(fServerControl, 0, 0)
			.AddTextControl(fShareControl, 0, 1)
			.AddTextControl(fNameControl, 0, 2)
			.Add(new BSeparatorView(B_HORIZONTAL), 0, 3, 2)
			.AddTextControl(fUserControl, 0, 4)
			.AddTextControl(fPasswordControl, 0, 5)
			.AddTextControl(fDomainControl, 0, 6)
			.End()
		.AddGroup(B_VERTICAL, 0)
			.Add(fStartupCheckBox)
			.Add(fReadOnlyCheckBox)
			.Add(fEncryptCheckBox)
			.End()
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.AddGlue()
			.Add(cancelButton)
			.Add(fSaveButton)
			.End()
		.SetInsets(B_USE_WINDOW_SPACING);

	SetDefaultButton(fSaveButton);
	_UpdateSaveButton();

	if (parent != NULL) {
		AddToSubset(parent);
		CenterIn(parent->Frame());
	} else
		CenterOnScreen();

	fServerControl->MakeFocus(true);
}


void
NetworkShareWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kShareEdited:
			// the volume is called what the share is, until someone has
			// called it something
			if (fNameIsShare) {
				BString name(fShareControl->Text());
				name.Trim();
				name.ReplaceAll('\\', '/');
				while (name.EndsWith("/"))
					name.Truncate(name.Length() - 1);
				name.Remove(0, name.FindLast('/') + 1);

				fNameControl->SetModificationMessage(NULL);
				fNameControl->SetText(name);
				fNameControl->SetModificationMessage(
					new BMessage(kShareNameEdited));
			}
			_UpdateSaveButton();
			break;

		case kShareNameEdited:
			fNameIsShare = fNameControl->Text()[0] == '\0';
			break;

		case kPasswordEdited:
		{
			fPasswordEdited = true;
			if (!fKeepPassword)
				break;

			// what was typed is the password now, and what stood for the
			// old one is not part of it
			fKeepPassword = false;

			BString password(fPasswordControl->Text());
			password.RemoveAll("\x01");
			fPasswordControl->SetText(password);
			fPasswordControl->TextView()->Select(password.Length(),
				password.Length());
			break;
		}

		case kSaveShare:
			_Save();
			break;

		default:
			_inherited::MessageReceived(message);
			break;
	}
}


void
NetworkShareWindow::_Save()
{
	BMessage request(kSetNetworkShare);
	if (fID >= 0)
		request.AddInt32("id", fID);

	BString share(fShareControl->Text());
	share.Trim();
	share.ReplaceAll('\\', '/');

	// the folder is part of what was shown as the share; if that is as it
	// was, so is the folder
	request.AddString("server", fServerControl->Text());
	request.AddString("share", share);
	request.AddString("path", "");
	request.AddString("name", fNameControl->Text());
	request.AddString("user", fUserControl->Text());
	request.AddString("domain", fDomainControl->Text());
	// Only one that was entered: that none is shown does not mean there is
	// none, the mount server may not have got to know yet.
	if (fPasswordEdited)
		request.AddString("password", fPasswordControl->Text());
	request.AddBool("mount at startup",
		fStartupCheckBox->Value() == B_CONTROL_ON);
	request.AddBool("read only", fReadOnlyCheckBox->Value() == B_CONTROL_ON);
	request.AddBool("encrypt", fEncryptCheckBox->Value() == B_CONTROL_ON);

	// one that is added is wanted now
	if (fID < 0)
		request.AddBool("mount", true);

	BMessenger mountServer(kMountServerSignature);
	if (mountServer.SendMessage(&request, fTarget) != B_OK) {
		BAlert* alert = new BAlert(B_TRANSLATE("Network shares"),
			B_TRANSLATE("The mount server could not be contacted."),
			B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go(NULL);
		return;
	}

	PostMessage(B_QUIT_REQUESTED);
}


void
NetworkShareWindow::_UpdateSaveButton()
{
	BString server(fServerControl->Text());
	BString share(fShareControl->Text());
	server.Trim();
	share.Trim();

	fSaveButton->SetEnabled(!server.IsEmpty() && !share.IsEmpty());
}
