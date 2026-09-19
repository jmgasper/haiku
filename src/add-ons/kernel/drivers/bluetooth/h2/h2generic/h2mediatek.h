/*
 * Firmware for MediaTek Bluetooth radios.
 *
 * Distributed under the terms of the MIT License.
 */
#ifndef _H2MEDIATEK_H_
#define _H2MEDIATEK_H_

#include "h2generic.h"

/* Put a MediaTek radio in a state where it will answer Bluetooth commands.
 *
 * Returns B_OK for a device this has nothing to do with, so it can be called
 * for anything that turns up.
 */
status_t mediatek_setup(bt_usb_dev* bdev);

#endif /* _H2MEDIATEK_H_ */
