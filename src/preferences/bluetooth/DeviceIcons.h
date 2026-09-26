/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef DEVICE_ICONS_H
#define DEVICE_ICONS_H


#include "DeviceModel.h"


class BBitmap;
class BView;


// Returns a cached icon for the device kind at the given size, or NULL when
// no vector icon is available for it (the caller draws a fallback).
const BBitmap*	DeviceIcon(device_kind kind, float size);

// The Bluetooth application icon.
const BBitmap*	BluetoothIcon(float size);

// Draws the icon for a kind into rect, with a drawn fallback for phones.
void			DrawDeviceIcon(BView* view, BRect rect, device_kind kind);

// Icon edge length for list rows, scaled with the plain font.
float			DeviceIconSize();


#endif	// DEVICE_ICONS_H
