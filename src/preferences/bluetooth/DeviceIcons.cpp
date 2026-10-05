/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "DeviceIcons.h"

#include <Application.h>
#include <Bitmap.h>
#include <IconUtils.h>
#include <Resources.h>
#include <View.h>

#include <map>
#include <math.h>


static const char*
IconResourceName(device_kind kind)
{
	switch (kind) {
		case DEVICE_KIND_MOUSE:
			return "device:mouse";
		case DEVICE_KIND_KEYBOARD:
		case DEVICE_KIND_INPUT:
			return "device:keyboard";
		case DEVICE_KIND_AUDIO:
			return "device:audio";
		case DEVICE_KIND_COMPUTER:
			return "device:computer";
		case DEVICE_KIND_GAMEPAD:
			return "device:gamepad";
		case DEVICE_KIND_PHONE:
			return NULL;
		default:
			return "BEOS:ICON";
	}
}


static const BBitmap*
LoadIcon(const char* resourceName, float size)
{
	typedef std::map<std::pair<BString, int32>, BBitmap*> IconCache;
	static IconCache sCache;

	int32 pixels = (int32)ceilf(size);
	std::pair<BString, int32> key(resourceName, pixels);
	IconCache::iterator found = sCache.find(key);
	if (found != sCache.end())
		return found->second;

	BBitmap* bitmap = NULL;
	BResources* resources = be_app != NULL ? be_app->AppResources() : NULL;
	size_t dataSize;
	const void* data = resources != NULL
		? resources->LoadResource(B_VECTOR_ICON_TYPE, resourceName, &dataSize)
		: NULL;
	if (data != NULL) {
		bitmap = new BBitmap(BRect(0, 0, pixels - 1, pixels - 1), 0,
			B_RGBA32);
		if (bitmap->InitCheck() != B_OK
			|| BIconUtils::GetVectorIcon((const uint8*)data, dataSize,
				bitmap) != B_OK) {
			delete bitmap;
			bitmap = NULL;
		}
	}
	sCache[key] = bitmap;
	return bitmap;
}


const BBitmap*
DeviceIcon(device_kind kind, float size)
{
	const char* name = IconResourceName(kind);
	if (name == NULL)
		return NULL;
	return LoadIcon(name, size);
}


const BBitmap*
BluetoothIcon(float size)
{
	return LoadIcon("BEOS:ICON", size);
}


static void
DrawPhone(BView* view, BRect rect)
{
	float width = rect.Width();
	BRect body(rect.left + width * 0.28f, rect.top + width * 0.06f,
		rect.right - width * 0.28f, rect.bottom - width * 0.06f);
	float radius = width * 0.08f;

	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetHighColor(52, 58, 70);
	view->FillRoundRect(body, radius, radius);
	view->SetHighColor(120, 170, 225);
	BRect screen = body.InsetByCopy(width * 0.05f, width * 0.12f);
	view->FillRect(screen);
	view->SetHighColor(200, 200, 200);
	float center = (body.left + body.right) / 2;
	view->FillEllipse(BPoint(center, body.bottom - width * 0.06f),
		width * 0.03f, width * 0.03f);
	view->PopState();
}


void
DrawDeviceIcon(BView* view, BRect rect, device_kind kind)
{
	const BBitmap* icon = DeviceIcon(kind, rect.Width() + 1);
	if (icon == NULL && kind != DEVICE_KIND_PHONE)
		icon = BluetoothIcon(rect.Width() + 1);

	if (icon == NULL) {
		DrawPhone(view, rect);
		return;
	}

	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->DrawBitmap(icon, rect.LeftTop());
	view->PopState();
}


float
DeviceIconSize()
{
	return floorf(be_plain_font->Size() / 12.0f * 32.0f);
}
