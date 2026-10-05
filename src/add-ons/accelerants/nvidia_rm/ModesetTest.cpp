#include <stdio.h>

#include <Application.h>
#include <Screen.h>

#include <NvKmsApi.h>
#include <NvKmsDevice.h>


static display_mode ToHaikuMode(const NvKmsMode &nvKmsMode) {
	display_mode haikuMode {
		.timing = {
			.pixel_clock  = nvKmsMode.timings.pixelClockHz / 1000,
			.h_display    = nvKmsMode.timings.hVisible,
			.h_sync_start = nvKmsMode.timings.hSyncStart,
			.h_sync_end   = nvKmsMode.timings.hSyncEnd,
			.h_total      = nvKmsMode.timings.hTotal,
			.v_display    = nvKmsMode.timings.vVisible,
			.v_sync_start = nvKmsMode.timings.vSyncStart,
			.v_sync_end   = nvKmsMode.timings.vSyncEnd,
			.v_total      = nvKmsMode.timings.vTotal,
			.flags        = (nvKmsMode.timings.hSyncPos ? B_POSITIVE_HSYNC : 0) | (nvKmsMode.timings.vSyncPos ? B_POSITIVE_VSYNC : 0),
		},
		.space = B_RGB32,
		.virtual_width  = nvKmsMode.timings.hVisible,
		.virtual_height = nvKmsMode.timings.vVisible,
	};
	return haikuMode;
}

static NvKmsMode ToNvKmsMode(const display_mode &haikuMode) {
	NvKmsMode nvKmsMode {
		.timings = {
			.RRx1k        = (uint32)(1000000LL * haikuMode.timing.pixel_clock / ((uint64)haikuMode.timing.h_total * haikuMode.timing.v_total)),
			.pixelClockHz = haikuMode.timing.pixel_clock * 1000,
			.hVisible     = haikuMode.timing.h_display,
			.hSyncStart   = haikuMode.timing.h_sync_start,
			.hSyncEnd     = haikuMode.timing.h_sync_end,
			.hTotal       = haikuMode.timing.h_total,
			.vVisible     = haikuMode.timing.v_display,
			.vSyncStart   = haikuMode.timing.v_sync_start,
			.vSyncEnd     = haikuMode.timing.v_sync_end,
			.vTotal       = haikuMode.timing.v_total,
			.hSyncPos     = (B_POSITIVE_HSYNC & haikuMode.timing.flags) != 0,
			.hSyncNeg     = (B_POSITIVE_HSYNC & haikuMode.timing.flags) == 0,
			.vSyncPos     = (B_POSITIVE_VSYNC & haikuMode.timing.flags) != 0,
			.vSyncNeg     = (B_POSITIVE_VSYNC & haikuMode.timing.flags) == 0,
		},
	};
	return nvKmsMode;
}


int main()
{
	BApplication app("application/x-vnd.Test.ModesetTest");

	BScreen screen;

	NvKmsMode nvKmsMode {
		.timings = {
			.RRx1k        =     60000, // 60.00Hz
			.pixelClockHz = 148500000, // 148.500MHz, 2200 * 1125 * 60
			.hVisible     =      1920,
			.hSyncStart   =      2008,
			.hSyncEnd     =      2052,
			.hTotal       =      2200,
			.vVisible     =      1080,
			.vSyncStart   =      1082,
			.vSyncEnd     =      1087,
			.vTotal       =      1125,
			.hSyncPos     = true,
			.vSyncPos     = true,
		},
		.name = "1920x1080",
	};

	display_mode mode {
		.timing = {
			.pixel_clock  = 148500,
			.h_display    =   1920,
			.h_sync_start =   2008,
			.h_sync_end   =   2052,
			.h_total      =   2200,
			.v_display    =   1080,
			.v_sync_start =   1084,
			.v_sync_end   =   1089,
			.v_total      =   1125,
			.flags        = B_POSITIVE_HSYNC | B_POSITIVE_VSYNC,
		},
		.space = B_RGB32,
		.virtual_width = 1920,
		.virtual_height = 1080,
	};
	display_mode mode2 {
		.timing = {
			.pixel_clock  = 148350,
			.h_display    =   1920,
			.h_sync_start =   2008,
			.h_sync_end   =   2052,
			.h_total      =   2200,
			.v_display    =   1080,
			.v_sync_start =   1084,
			.v_sync_end   =   1089,
			.v_total      =   1125,
			.flags        = B_POSITIVE_HSYNC | B_POSITIVE_VSYNC,
		},
		.space = B_RGB32,
		.virtual_width = 1920,
		.virtual_height = 1080,
	};
	status_t res = screen.SetMode(&mode, true);
	if (res < B_OK) {
		printf("[!] %s\n", strerror(res));
		return 1;
	}

	printf("[WAIT]"); fgetc(stdin);

	return 0;
}
