/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	What M2 needs of drm/imagination's pvr_power.c: the device-lost state,
	the reset entry point (not there yet: the GPU is reported lost), and
	the power-off handshake before the firmware is stopped, written after
	pvr_power_request_idle(), pvr_power_request_pwr_off() and
	pvr_power_send_command(). The GPU stays powered: no runtime PM. */


#include "pvr_ccb.h"
#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_haiku_device.h"
#include "pvr_power.h"
#include "pvr_rogue_fwif.h"


#define POWER_SYNC_TIMEOUT_US	1000000


void
pvr_device_lost(struct pvr_device* pvr_dev)
{
	if (!pvr_dev->lost) {
		pvr_dev->lost = true;
		drm_err(from_pvr_device(pvr_dev), "GPU device lost\n");
	}
}


int
pvr_power_reset(struct pvr_device* pvr_dev, bool hard_reset)
{
	// The firmware asks for a restart (FWCCB) or a flush failed: no reset
	// path before M4, so the device is given up and the state logged.
	drm_err(from_pvr_device(pvr_dev), "%s reset requested, not implemented"
		" yet\n", hard_reset ? "hard" : "soft");
	pvr_haiku_dump(pvr_dev, "reset requested", 64);
	pvr_device_lost(pvr_dev);
	return -EIO;
}


static int
pvr_haiku_power_send_command(struct pvr_device* pvr_dev,
	struct rogue_fwif_kccb_cmd* command)
{
	struct pvr_fw_device* fw_dev = &pvr_dev->fw_dev;
	u32 slot;
	u32 value;

	WRITE_ONCE(*fw_dev->power_sync, 0);

	int error = pvr_kccb_send_cmd_powered(pvr_dev, command, &slot);
	if (error != 0)
		return error;

	// the firmware acknowledges through the power sync object
	return readl_poll_timeout(fw_dev->power_sync, value, value != 0, 100,
		POWER_SYNC_TIMEOUT_US);
}


/*!	Forced idle, then power off, as before a runtime suspend: afterwards
	pvr_fw_stop() finds the LAYOUT_MARS core's CPU idle.
*/
int
pvr_haiku_power_off(struct pvr_device* pvr_dev)
{
	struct rogue_fwif_kccb_cmd command = {};

	command.cmd_type = ROGUE_FWIF_KCCB_CMD_POW;
	command.cmd_data.pow_data.pow_type = ROGUE_FWIF_POW_FORCED_IDLE_REQ;
	command.cmd_data.pow_data.power_req_data.pow_request_type
		= ROGUE_FWIF_POWER_FORCE_IDLE;
	int error = pvr_haiku_power_send_command(pvr_dev, &command);
	if (error != 0) {
		drm_err(from_pvr_device(pvr_dev), "forced idle request: %d\n",
			error);
		return error;
	}

	memset(&command, 0, sizeof(command));
	command.cmd_type = ROGUE_FWIF_KCCB_CMD_POW;
	command.cmd_data.pow_data.pow_type = ROGUE_FWIF_POW_OFF_REQ;
	command.cmd_data.pow_data.power_req_data.forced = true;
	error = pvr_haiku_power_send_command(pvr_dev, &command);
	if (error != 0)
		drm_err(from_pvr_device(pvr_dev), "power off request: %d\n", error);
	return error;
}
