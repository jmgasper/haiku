/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include <Application.h>
#include <Catalog.h>

#include "WiFiController.h"
#include "WiFiWindow.h"


class WiFiApplication : public BApplication {
public:
								WiFiApplication();

	virtual	void				ReadyToRun();
	virtual	void				MessageReceived(BMessage* message);

private:
			WiFiWindow*			fWindow;
			BMessage			fPendingMessage;
			bool				fHasPendingMessage;
};


WiFiApplication::WiFiApplication()
	:
	BApplication(kWiFiPreferencesSignature),
	fWindow(NULL),
	fHasPendingMessage(false)
{
}


void
WiFiApplication::ReadyToRun()
{
	fWindow = new WiFiWindow();
	fWindow->Show();
	if (fHasPendingMessage) {
		fWindow->PostMessage(&fPendingMessage);
		fHasPendingMessage = false;
	}
}


void
WiFiApplication::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgWiFiSelectNetwork:
		case kMsgWiFiJoinOther:
			// Requests from the WiFiStatus applet
			if (fWindow != NULL) {
				fWindow->PostMessage(message);
				fWindow->Activate(true);
			} else {
				fPendingMessage = *message;
				fHasPendingMessage = true;
			}
			break;

		default:
			BApplication::MessageReceived(message);
			break;
	}
}


int
main()
{
	WiFiApplication application;
	application.Run();
	return 0;
}
