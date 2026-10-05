/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef WIFI_WINDOW_H
#define WIFI_WINDOW_H


#include <String.h>
#include <Window.h>

#include "WiFiController.h"


class BBox;
class BButton;
class BCheckBox;
class BMenuField;
class BPopUpMenu;
class BStringView;
class BTextControl;
class NetworkListView;
class SignalView;


class WiFiWindow : public BWindow {
public:
								WiFiWindow();
	virtual						~WiFiWindow();

	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_SetState(const WiFiState& state);
			void				_UpdateCurrent();
			void				_UpdateDevices();
			void				_UpdateButtons();
			void				_JoinResult(BMessage* message);

			const WiFiNetworkInfo* _SelectedNetwork() const;
			void				_JoinSelected();
			void				_ShowPrompt(const char* name,
									uint32 authentication);
			void				_HidePrompt();
			void				_JoinFromPrompt();
			void				_SetMessage(const char* text, bool error);
			void				_Forget();
			void				_ShowJoinOther();

private:
			WiFiController*		fController;
			BMessenger			fControllerMessenger;
			WiFiState			fState;
			bool				fHasState;
			bool				fShowingNetServerError;

			BCheckBox*			fPowerCheck;
			BMenuField*			fDeviceField;
			BPopUpMenu*			fDeviceMenu;

			SignalView*			fCurrentSignal;
			BStringView*		fCurrentName;
			BStringView*		fCurrentStatus;
			BButton*			fDisconnectButton;

			BStringView*		fKnownLabel;
			NetworkListView*	fKnownList;
			BButton*			fForgetButton;
			BStringView*		fOtherLabel;
			BStringView*		fScanLabel;
			NetworkListView*	fOtherList;

			BBox*				fPromptBox;
			BStringView*		fPromptLabel;
			BTextControl*		fPasswordControl;
			BCheckBox*			fShowPassword;
			BCheckBox*			fRememberCheck;
			BStringView*		fPromptError;
			BButton*			fPromptJoinButton;
			BString				fPromptName;
			uint32				fPromptAuthentication;

			BStringView*		fMessageView;
			BButton*			fJoinButton;

			BString				fJoiningName;
			BString				fRequestedName;
			uint32				fRequestedAuthentication;
			bigtime_t			fRequestedTime;
};


#endif	// WIFI_WINDOW_H
