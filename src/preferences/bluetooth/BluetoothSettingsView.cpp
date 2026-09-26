/*
 * Copyright 2008-2009, Oliver Ruiz Dorantes <oliver.ruiz.dorantes@gmail.com>
 * Copyright 2012-2013, Tri-Edge AI, <triedgeai@gmail.com>
 * Copyright 2021, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Fredrik Modéen <fredrik_at_modeen.se>
 */

#include "BluetoothSettingsView.h"

#include "defs.h"
#include "BluetoothSettings.h"
#include "ExtendedLocalDeviceView.h"

#include <bluetooth/LocalDevice.h>

#include <Catalog.h>
#include <LayoutBuilder.h>
#include <OptionPopUp.h>
#include <String.h>
#include <TextControl.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Settings view"

static const char* kAllLabel = B_TRANSLATE_MARK("From all devices");
static const char* kTrustedLabel =
	B_TRANSLATE_MARK("Only from trusted devices");
static const char* kAlwaysLabel = B_TRANSLATE_MARK("Always ask");

static const char* kDesktopLabel = B_TRANSLATE_MARK("Desktop");
static const char* kServerLabel = B_TRANSLATE_MARK("Server");
static const char* kLaptopLabel = B_TRANSLATE_MARK("Laptop");
static const char* kHandheldLabel = B_TRANSLATE_MARK("Handheld");
static const char* kPhoneLabel = B_TRANSLATE_MARK("Smart phone");

//	#pragma mark -

BluetoothSettingsView::BluetoothSettingsView(const char* name,
	const BMessenger& mainWindow)
	:
	BView(name, 0),
	fMainWindow(mainWindow),
	fLocalDevice(NULL)
{
	fSettings.LoadSettings();

	fPolicyMenu = new BOptionPopUp("policy",
		B_TRANSLATE("Incoming connections policy:"),
		new BMessage(kMsgSetConnectionPolicy));
	fPolicyMenu->AddOption(B_TRANSLATE_NOCOLLECT(kAllLabel), 1);
	fPolicyMenu->AddOption(B_TRANSLATE_NOCOLLECT(kTrustedLabel), 2);
	fPolicyMenu->AddOption(B_TRANSLATE_NOCOLLECT(kAlwaysLabel), 3);
	fPolicyMenu->SetValue(fSettings.Policy());

	fExtDeviceView = new ExtendedLocalDeviceView(NULL);

	fFriendlyName = new BTextControl("FriendlyName",
		B_TRANSLATE("Friendly name:"), NULL,
		new BMessage(kMsgSetFriendlyName));
	fFriendlyName->SetEnabled(false);

	fClassMenu = new BOptionPopUp("DeviceClass",
		B_TRANSLATE("Identify host as:"), new BMessage(kMsgSetDeviceClass));
	fClassMenu->AddOption(B_TRANSLATE_NOCOLLECT(kDesktopLabel), 1);
	fClassMenu->AddOption(B_TRANSLATE_NOCOLLECT(kServerLabel), 2);
	fClassMenu->AddOption(B_TRANSLATE_NOCOLLECT(kLaptopLabel), 3);
	fClassMenu->AddOption(B_TRANSLATE_NOCOLLECT(kHandheldLabel), 4);
	fClassMenu->AddOption(B_TRANSLATE_NOCOLLECT(kPhoneLabel), 5);
	fClassMenu->SetValue(_GetClassForMenu());

	BLayoutBuilder::Grid<>(this, B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
		.AddTextControl(fFriendlyName, 0, 0)
		.Add(fClassMenu, 0, 1, 2)
		.Add(fPolicyMenu, 0, 2, 2)
		.Add(fExtDeviceView, 0, 3, 2)
	.End();
}


BluetoothSettingsView::~BluetoothSettingsView()
{
}


void
BluetoothSettingsView::SetLocalDevice(LocalDevice* device)
{
	fLocalDevice = device;
	fFriendlyName->SetEnabled(device != NULL);
	if (device == NULL)
		return;

	fFriendlyName->SetText(device->GetFriendlyName());
	fExtDeviceView->SetLocalDevice(device);
	fExtDeviceView->SetEnabled(true);

	DeviceClass rememberedClass = device->GetDeviceClass();
	if (!rememberedClass.IsUnknownDeviceClass()) {
		fSettings.LoadSettings();
		fSettings.SetLocalDeviceClass(rememberedClass);
		fSettings.SaveSettings();
		fClassMenu->SetValue(_GetClassForMenu());
	}
}


void
BluetoothSettingsView::AttachedToWindow()
{
	if (Parent() != NULL)
		SetViewColor(Parent()->ViewColor());
	else
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	fPolicyMenu->SetTarget(this);
	fClassMenu->SetTarget(this);
	fFriendlyName->SetTarget(this);
}


void
BluetoothSettingsView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSetConnectionPolicy:
		{
			int32 policy;
			if (message->FindInt32("be:value", &policy) == B_OK) {
				fSettings.LoadSettings();
				fSettings.SetPolicy(policy);
				fSettings.SaveSettings();
			}
			break;
		}

		case kMsgSetDeviceClass:
		{
			int32 deviceClass;
			if (message->FindInt32("be:value", &deviceClass) == B_OK) {
				if (deviceClass == 5)
					_SetDeviceClass(2, 3, 0x72);
				else
					_SetDeviceClass(1, deviceClass, 0x72);
			}
			break;
		}

		case kMsgSetFriendlyName:
		{
			if (fLocalDevice == NULL)
				break;

			BString friendlyName = fFriendlyName->Text();
			fLocalDevice->SetFriendlyName(friendlyName);
			fExtDeviceView->SetLocalDevice(fLocalDevice);

			BMessage changed(kMsgLocalNameChanged);
			changed.AddInt32("id", fLocalDevice->ID());
			changed.AddString("name", friendlyName);
			fMainWindow.SendMessage(&changed);
			break;
		}

		default:
			BView::MessageReceived(message);
			break;
	}
}


bool
BluetoothSettingsView::_SetDeviceClass(uint8 major, uint8 minor,
	uint16 service)
{
	fSettings.LoadSettings();
	fSettings.SetLocalDeviceClass(DeviceClass(major, minor, service));
	fSettings.SaveSettings();

	if (fLocalDevice == NULL)
		return false;

	fLocalDevice->SetDeviceClass(fSettings.LocalDeviceClass());
	return true;
}


int
BluetoothSettingsView::_GetClassForMenu()
{
	int deviceClass =
			fSettings.LocalDeviceClass().MajorDeviceClass()+
			fSettings.LocalDeviceClass().MinorDeviceClass();

	// As of now we only support MajorDeviceClass = 1 and MinorDeviceClass 1-4
	// and MajorDeviceClass = 2 and MinorDeviceClass 3.
	if (fSettings.LocalDeviceClass().MajorDeviceClass() == 1
			&& (fSettings.LocalDeviceClass().MinorDeviceClass() > 0
			&& fSettings.LocalDeviceClass().MinorDeviceClass() < 5))
		deviceClass -= 1;

	return deviceClass;
}
