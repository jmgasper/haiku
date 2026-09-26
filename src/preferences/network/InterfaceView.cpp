/*
 * Copyright 2004-2015 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, <axeld@pinc-software.de>
 *		Alexander von Gluck, kallisti5@unixzen.com
 *		John Scipione, jscipione@gmail.com
 */


#include "InterfaceView.h"

#include <set>
#include <string.h>

#include <net/if_media.h>

#include <AutoDeleter.h>
#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <NetworkAddress.h>
#include <StringForSize.h>
#include <StringView.h>
#include <TextControl.h>

#include "MediaTypes.h"
#include "WirelessNetworkMenuItem.h"
#include "../../shared/net/WirelessNetworkList.h"


static const uint32 kMsgInterfaceToggle = 'onof';
static const uint32 kMsgInterfaceRenegotiate = 'redo';
static const uint32 kMsgJoinNetwork = 'join';


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "IntefaceView"


// #pragma mark - InterfaceView


InterfaceView::InterfaceView()
	:
	BGroupView(B_VERTICAL),
	fPulseCount(0)
{
	SetFlags(Flags() | B_PULSE_NEEDED);

	// TODO: Small graph of throughput?

	BStringView* statusLabel = new BStringView("status label", B_TRANSLATE("Status:"));
	statusLabel->SetAlignment(B_ALIGN_RIGHT);
	fStatusField = new BStringView("status field", "");
	BStringView* macAddressLabel = new BStringView("mac address label",
		B_TRANSLATE("MAC address:"));
	macAddressLabel->SetAlignment(B_ALIGN_RIGHT);
	fMacAddressField = new BStringView("mac address field", "");
	BStringView* linkSpeedLabel = new BStringView("link speed label",
		B_TRANSLATE("Link speed:"));
	linkSpeedLabel->SetAlignment(B_ALIGN_RIGHT);
	fLinkSpeedField = new BStringView("link speed field", "");

	// TODO: These metrics may be better in a BScrollView?
	BStringView* linkTxLabel = new BStringView("tx label",
		B_TRANSLATE("Sent:"));
	linkTxLabel->SetAlignment(B_ALIGN_RIGHT);
	fLinkTxField = new BStringView("tx field", "");
	BStringView* linkRxLabel = new BStringView("rx label",
		B_TRANSLATE("Received:"));
	linkRxLabel->SetAlignment(B_ALIGN_RIGHT);
	fLinkRxField = new BStringView("rx field", "");

	fNetworkMenuField = new BMenuField(B_TRANSLATE("Network:"), new BMenu(
		B_TRANSLATE("Choose automatically")));
	fNetworkMenuField->SetAlignment(B_ALIGN_RIGHT);
	fNetworkMenuField->Menu()->SetLabelFromMarked(true);

	// Construct the BButtons
	fToggleButton = new BButton("onoff", B_TRANSLATE("Disable"),
		new BMessage(kMsgInterfaceToggle));

	fRenegotiateButton = new BButton("heal", B_TRANSLATE("Renegotiate"),
		new BMessage(kMsgInterfaceRenegotiate));
	fRenegotiateButton->SetEnabled(false);

	BLayoutBuilder::Group<>(this)
		.AddGrid()
			.Add(statusLabel, 0, 0)
			.Add(fStatusField, 1, 0)
			.AddMenuField(fNetworkMenuField, 0, 1, B_ALIGN_RIGHT, 1, 2)
			.Add(macAddressLabel, 0, 2)
			.Add(fMacAddressField, 1, 2)
			.Add(linkSpeedLabel, 0, 3)
			.Add(fLinkSpeedField, 1, 3)
			.Add(linkTxLabel, 0, 4)
			.Add(fLinkTxField, 1, 4)
			.Add(linkRxLabel, 0, 5)
			.Add(fLinkRxField, 1, 5)
		.End()
		.AddGlue()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fToggleButton)
			.Add(fRenegotiateButton)
		.End();
}


InterfaceView::~InterfaceView()
{
}


void
InterfaceView::SetTo(const char* name)
{
	fInterface.SetTo(name);
}


void
InterfaceView::AttachedToWindow()
{
	_Update();
		// Populate the fields

	fToggleButton->SetTarget(this);
	fRenegotiateButton->SetTarget(this);
}


void
InterfaceView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgJoinNetwork:
		{
			const char* name;
			if (message->FindString("name", &name) == B_OK) {
				BNetworkDevice device(fInterface.Name());
				status_t status = device.JoinNetwork(name);
				if (status != B_OK) {
					BString text
						= B_TRANSLATE("Could not join wireless network:\n");
					text << strerror(status);
					(new BAlert(name, text.String(),
						B_TRANSLATE("OK")))->Go(NULL);
				}
			}
			break;
		}
		case kMsgInterfaceToggle:
		{
			// Disable/enable interface
			uint32 flags = fInterface.Flags();
			if ((flags & IFF_UP) != 0)
				flags &= ~IFF_UP;
			else
				flags |= IFF_UP;

			if (fInterface.SetFlags(flags) == B_OK)
				_Update();
			break;
		}

		case kMsgInterfaceRenegotiate:
		{
			// TODO: renegotiate addresses
			break;
		}

		default:
			BView::MessageReceived(message);
	}
}


void
InterfaceView::Pulse()
{
	// Update the wireless network menu every 5 seconds
	_Update((fPulseCount++ % 5) == 0);
}


/*!	Populate fields with current settings.
*/
status_t
InterfaceView::_Update(bool updateWirelessNetworks)
{
	BNetworkDevice device(fInterface.Name());
	bool isWireless = device.IsWireless();
	bool disabled = (fInterface.Flags() & IFF_UP) == 0;

	uint32 flags = fInterface.Flags();

	if ((flags & IFF_LINK) == 0)
		fStatusField->SetText(B_TRANSLATE("no link"));
	else if ((flags & (IFF_UP | IFF_CONFIGURING)) == 0)
		fStatusField->SetText(B_TRANSLATE("no stateful configuration"));
	else if ((flags & IFF_CONFIGURING) == IFF_CONFIGURING)
		fStatusField->SetText(B_TRANSLATE("configuring"));
	else if ((flags & IFF_UP) == IFF_UP)
		fStatusField->SetText(B_TRANSLATE("connected"));
	else
		fStatusField->SetText(B_TRANSLATE("unknown"));

	BNetworkAddress hardwareAddress;
	if (device.GetHardwareAddress(hardwareAddress) == B_OK)
		fMacAddressField->SetText(hardwareAddress.ToString());
	else
		fMacAddressField->SetText("-");

	int media = fInterface.Media();
	if ((media & IFM_ACTIVE) != 0)
		fLinkSpeedField->SetText(media_type_to_string(media));
	else
		fLinkSpeedField->SetText("-");

	// Update Link stats
	ifreq_stats stats;
	if (fInterface.GetStats(stats) == B_OK) {
		char buffer[100];

		string_for_size(stats.send.bytes, buffer, sizeof(buffer));
		fLinkTxField->SetText(buffer);

		string_for_size(stats.receive.bytes, buffer, sizeof(buffer));
		fLinkRxField->SetText(buffer);
	}

	// TODO: move the wireless info to a separate tab. We should have a
	// BListView of available networks, rather than a menu, to make them more
	// readable and easier to browse and select.
	if (fNetworkMenuField->IsHidden(fNetworkMenuField) && isWireless)
		fNetworkMenuField->Show();
	else if (!fNetworkMenuField->IsHidden(fNetworkMenuField) && !isWireless)
		fNetworkMenuField->Hide();

	if (isWireless && updateWirelessNetworks) {
		BMenu* menu = fNetworkMenuField->Menu();
		while (menu->CountItems() > 0)
			delete menu->RemoveItem((int32)0);

		// Group access points by network name, like the WiFi preferences
		wireless_network* networks = NULL;
		uint32 count = 0;
		device.GetNetworks(networks, count);
		if (count <= 1 && (fPulseCount % 15) == 0
			&& (fInterface.Flags() & IFF_UP) != 0) {
			// Some drivers only keep the associated access point after
			// joining. Scanning a disabled interface would turn it back on.
			device.Scan(false, false);
			delete[] networks;
			networks = NULL;
			device.GetNetworks(networks, count);
		}
		ArrayDeleter<wireless_network> networksDeleter(networks);
		BString associatedName = AssociatedWirelessNetworkName(device,
			networks, count);
		std::vector<WirelessNetworkGroup> visible = GroupWirelessNetworks(
			networks, count, associatedName.String());

		if (visible.empty()) {
			BMenuItem* item = new BMenuItem(
				B_TRANSLATE("<no wireless networks found>"), NULL);
			item->SetEnabled(false);
			menu->AddItem(item);
		} else {
			menu->AddItem(new BMenuItem(
				B_TRANSLATE("Choose automatically"), NULL));
			menu->AddSeparatorItem();
			for (size_t i = 0; i < visible.size(); i++) {
				wireless_network network = visible[i].network;
				// The menu item draws the signal as a percentage
				network.signal_strength = WirelessSignalPercent(network);

				BMessage* message = new BMessage(kMsgJoinNetwork);
				message->AddString("device", fInterface.Name());
				message->AddString("name", network.name);
				BMenuItem* item = new WirelessNetworkMenuItem(network, message);
				item->SetMarked(visible[i].connected);
				menu->AddItem(item);
			}
		}
		menu->SetTargetForItems(this);
	}

	//fRenegotiateButton->SetEnabled(!disabled);
	fToggleButton->SetLabel(disabled
		? B_TRANSLATE("Enable") : B_TRANSLATE("Disable"));

	return B_OK;
}
