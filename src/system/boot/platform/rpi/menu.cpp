/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2004-2005, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <boot/menu.h>
#include <boot/platform/generic/text_menu.h>
#include <safemode.h>

#include "smp.h"
#include "video.h"


void
platform_add_menus(Menu* menu)
{
	MenuItem* item;

	switch (menu->Type()) {
		case MAIN_MENU:
			item = new(std::nothrow) MenuItem("Select video mode",
				video_mode_menu());
			if (item != NULL) {
				menu->AddItem(item);
				item->SetTarget(video_mode_hook);
				item->SetShortcut('v');
			}
			break;

		case SAFE_MODE_MENU:
			item = new(std::nothrow) MenuItem("Use fail-safe graphics driver");
			if (item != NULL) {
				menu->AddItem(item);
				item->SetType(MENU_ITEM_MARKABLE);
				item->SetData(B_SAFEMODE_FAIL_SAFE_VIDEO_MODE);
				item->SetHelpText("The system will use the firmware's frame "
					"buffer and won't try to use any video graphics drivers.");
			}

			smp_add_safemode_menus(menu);
			break;

		default:
			break;
	}
}


void
platform_update_menu_item(Menu* menu, MenuItem* item)
{
	platform_generic_update_text_menu_item(menu, item);
}


void
platform_run_menu(Menu* menu)
{
	platform_generic_run_text_menu(menu);
}


size_t
platform_get_user_input_text(Menu* menu, MenuItem* item, char* buffer,
	size_t bufferSize)
{
	return platform_generic_get_user_input_text(menu, item, buffer,
		bufferSize);
}
