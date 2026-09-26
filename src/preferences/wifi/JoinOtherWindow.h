/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef JOIN_OTHER_WINDOW_H
#define JOIN_OTHER_WINDOW_H


#include <Messenger.h>
#include <Window.h>


class BButton;
class BCheckBox;
class BMenuField;
class BTextControl;


//!	Joins a network by name, typically one that hides its SSID.
class JoinOtherWindow : public BWindow {
public:
								JoinOtherWindow(BWindow* parent,
									const BMessenger& controller);

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			uint32				_Authentication() const;
			void				_UpdateControls();

			BMessenger			fController;
			BTextControl*		fNameControl;
			BMenuField*			fSecurityField;
			BTextControl*		fPasswordControl;
			BCheckBox*			fShowPassword;
			BCheckBox*			fRememberCheck;
			BButton*			fJoinButton;
};


#endif	// JOIN_OTHER_WINDOW_H
