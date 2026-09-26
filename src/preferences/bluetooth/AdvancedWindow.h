/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef ADVANCED_WINDOW_H
#define ADVANCED_WINDOW_H


#include <Messenger.h>
#include <Window.h>

#include <bluetooth/LocalDevice.h>


class BluetoothSettingsView;
class BOptionPopUp;


// Adapter settings and diagnostics, kept out of the main window.
class AdvancedWindow : public BWindow {
public:
								AdvancedWindow(const BMessenger& mainWindow,
									LocalDevice* device);

	virtual	void				MessageReceived(BMessage* message);

private:
			BluetoothSettingsView* fSettingsView;
			BOptionPopUp*		fLogLevel;
			LocalDevice*		fDevice;
};


// Opens the LE diagnostics log in a text editor.
void	OpenLELog();


#endif	// ADVANCED_WINDOW_H
