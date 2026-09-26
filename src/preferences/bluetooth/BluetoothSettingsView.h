/*
 * Copyright 2008-2009, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes@gmail.com>
 * Copyright 2012-2013, Tri-Edge AI, <triedgeai@gmail.com>
 *
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef BLUETOOTH_SETTINGS_VIEW_H
#define BLUETOOTH_SETTINGS_VIEW_H

#include "BluetoothSettings.h"

#include <Messenger.h>
#include <View.h>

class ExtendedLocalDeviceView;

class BOptionPopUp;
class BTextControl;

// Settings of the local adapter. SetLocalDevice() talks to the Bluetooth
// server synchronously, so it is called from the advanced window's thread.
class BluetoothSettingsView : public BView {
public:
								BluetoothSettingsView(const char* name,
									const BMessenger& mainWindow);
	virtual						~BluetoothSettingsView();

			void				SetLocalDevice(LocalDevice* device);

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);

private:
			bool				_SetDeviceClass(uint8 major, uint8 minor,
									uint16 service);
			int					_GetClassForMenu();

			BluetoothSettings	fSettings;
			BMessenger			fMainWindow;
			LocalDevice*		fLocalDevice;

			BOptionPopUp*		fPolicyMenu;
			BOptionPopUp*		fClassMenu;
			BTextControl*		fFriendlyName;

			ExtendedLocalDeviceView* fExtDeviceView;
};

#endif // BLUETOOTH_SETTINGS_VIEW_H
