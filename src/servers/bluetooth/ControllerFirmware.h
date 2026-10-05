/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CONTROLLER_FIRMWARE_H
#define CONTROLLER_FIRMWARE_H


namespace ControllerFirmware {

// Loads firmware into the USB Bluetooth controllers that appeared since the
// last call, with bt_firmware, before the server opens them. Controllers the
// server may already be using are left alone: bt_firmware talks to a
// controller over usb_raw, and its commands and events would mix with
// h2generic's.
void PrepareNewControllers();

}


#endif	// CONTROLLER_FIRMWARE_H
