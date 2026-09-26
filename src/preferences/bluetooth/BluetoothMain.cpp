/*
 * Copyright 2008-10, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#include <Catalog.h>
#include <private/interface/AboutWindow.h>

#include "BluetoothMain.h"
#include "BluetoothWindow.h"
#include "defs.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "main"

BluetoothApplication::BluetoothApplication()
	:
	BApplication(BLUETOOTH_APP_SIGNATURE),
	fWindow(NULL)
{
}


void
BluetoothApplication::ReadyToRun()
{
	// The window itself reports a stopped service and offers to start it.
	fWindow = new BluetoothWindow();
	fWindow->Show();
}


void
BluetoothApplication::AboutRequested()
{
	BAboutWindow* about = new BAboutWindow(B_TRANSLATE_SYSTEM_NAME("Bluetooth"),
		BLUETOOTH_APP_SIGNATURE);
	about->AddCopyright(2010, "Oliver Ruiz Dorantes");
	about->AddText(B_TRANSLATE(
		"With support of:\n"
		" - Mika Lindqvist\n"
		" - Adrien Destugues\n"
		" - Maksym Yevmenkin\n\n"
		"Thanks to the individuals who helped" B_UTF8_ELLIPSIS "\n\n"
		"Shipping/donating hardware:\n"
		" - Henry Jair Abril Florez (el Colombian)\n"
		"	& Stefanie Bartolich\n"
		" - Edwin Erik Amsler\n"
		" - Dennis d'Entremont\n"
		" - Luroh\n"
		" - Pieter Panman\n\n"
		"Economically:\n"
		" - Karl vom Dorff, Andrea Bernardi (OSDrawer),\n"
		" - Matt M, Doug F, Hubert H,\n"
		" - Sebastian B, Andrew M, Jared E,\n"
		" - Frederik H, Tom S, Ferry B,\n"
		" - Greg G, David F, Richard S, Martin W:\n\n"
		"With patches:\n"
		" - Michael Weirauch\n"
		" - Fredrik Ekdahl\n"
		" - Raynald Lesieur\n"
		" - Andreas Färber\n"
		" - Joerg Meyer\n"
		"Testing:\n"
		" - Petter H. Juliussen\n"
		"Who gave me all the knowledge:\n"
		" - the yellowTAB team"));
	about->Show();
}


int
main(int, char**)
{
	BluetoothApplication app;
	app.Run();

	return 0;
}
