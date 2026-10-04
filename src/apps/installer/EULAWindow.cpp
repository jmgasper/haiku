/*
 * Copyright 2013,	Jérôme DUVAL.
 * All rights reserved. Distributed under the terms of the MIT license.
 */


#include "EULAWindow.h"

#include <Application.h>
#include <Box.h>
#include <Button.h>
#include <Catalog.h>
#include <LayoutBuilder.h>
#include <LayoutUtils.h>
#include <Roster.h>
#include <ScrollView.h>
#include <SpaceLayoutItem.h>

#include "tracker_private.h"

static const uint32 kMsgAgree = 'agre';
static const uint32 kMsgNext = 'next';

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InstallerApp"


EULAWindow::EULAWindow()
	:
	BWindow(BRect(), B_TRANSLATE("README"), B_MODAL_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
		B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_AUTO_UPDATE_SIZE_LIMITS)
{
	BString infoText;
	infoText << B_TRANSLATE(
		"Welcome to the air/OS Installer!\n\n");
	infoText << B_TRANSLATE(
		"air/OS is beta software. Back up your data before you install "
		"it.\n\n");
	infoText << B_TRANSLATE(
		"The easiest way is to give air/OS a whole disk: the Installer "
		"erases it, sets it up to start with UEFI and installs air/OS. "
		"Everything else on that disk is lost.\n\n");
	infoText << B_TRANSLATE(
		"To keep what is already on a disk, first set up a partition for "
		"air/OS with \"Set up partitions" B_UTF8_ELLIPSIS "\" and install "
		"onto that. You then have to set up booting from it yourself.");

	BTextView* textView = new BTextView("eula", be_plain_font, NULL, B_WILL_DRAW);
	textView->SetInsets(10, 10, 10, 10);
	textView->MakeEditable(false);
	textView->MakeSelectable(false);
	textView->SetText(infoText);

	BScrollView* scrollView = new BScrollView("eulaScroll",
		textView, B_WILL_DRAW, false, true);

	BButton* cancelButton = new BButton(B_TRANSLATE("Quit"),
		new BMessage(B_QUIT_REQUESTED));
	cancelButton->SetTarget(be_app);

	BButton* continueButton = new BButton(B_TRANSLATE("Continue"),
		new BMessage(kMsgAgree));
	continueButton->SetTarget(be_app);
	continueButton->MakeDefault(true);

	if (!be_roster->IsRunning(kTrackerSignature))
		SetWorkspaces(B_ALL_WORKSPACES);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(scrollView)
		.AddGroup(B_HORIZONTAL, B_USE_ITEM_SPACING)
			.AddGlue()
			.Add(cancelButton)
			.Add(continueButton);

	font_height fontHeight;
	be_plain_font->GetHeight(&fontHeight);
	const float lineHeight = fontHeight.ascent + fontHeight.descent;
	GetLayout()->SetExplicitSize(BSize(be_plain_font->StringWidth("M") * 40, lineHeight * 18));
	CenterOnScreen();
	Show();
}


bool
EULAWindow::QuitRequested()
{
	be_app->PostMessage(kMsgNext);
	return true;
}
