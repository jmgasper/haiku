/* Does the display engine show a hardware cursor, and where?
 *
 * app_server draws a software cursor into the frame buffer when the
 * accelerant has no cursor hooks. A program that draws into the frame buffer
 * itself (a direct window) then paints over the cursor, and app_server puts
 * back pixels from its own, older copy of the screen when the cursor moves.
 * NVKMS has a cursor plane that is not in the frame buffer at all; this
 * checks that it is composited, as another NVKMS client next to app_server
 * (cursor calls need no ownership of the display).
 *
 * It reads each head's compositor CRC, puts a 64 x 64 square on the cursor
 * plane of the head at x, y (head pixels), reads the CRCs again, takes the
 * square away and reads them a last time. The CRC is of the composited
 * picture, so a cursor that is shown changes it, and taking it away brings the
 * first value back if nothing else on the screen changed in between.
 *
 * usage: nvcursortest [head] [x] [y] [seconds shown]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>
#include <NvKmsSurface.h>

#include "NvKmsBitmap.h"
#include "NvUtils.h"


static void
PrintCrcs(NvKmsApi& kms, NvKmsDevice& kmsDev, const char* when)
{
	printf("%s:", when);
	for (NvU32 head = 0; head < 4; head++) {
		NvKmsQueryDpyCRC32Params params {};
		params.request.deviceHandle = kmsDev.Get();
		params.request.dispHandle = kmsDev.Info().dispHandles[0];
		params.request.head = head;
		if (kms.Control(NVKMS_IOCTL_QUERY_DPY_CRC32, &params, sizeof(params)) != B_OK)
			continue;
		printf(" head %" B_PRIu32 " compositor %08" B_PRIx32 "%s", (uint32)head,
			(uint32)params.reply.compositorCrc32.value,
			params.reply.compositorCrc32.supported ? "" : " (unsupported)");
	}
	printf("\n");
}


static status_t
SetCursor(NvKmsApi& kms, NvKmsDevice& kmsDev, NvU32 head, NvKmsSurfaceHandle surface)
{
	NvKmsSetCursorImageParams params {};
	params.request.deviceHandle = kmsDev.Get();
	params.request.dispHandle = kmsDev.Info().dispHandles[0];
	params.request.head = head;
	params.request.common.surfaceHandle[NVKMS_LEFT] = surface;
	params.request.common.cursorCompParams.colorKeySelect = NVKMS_COMPOSITION_COLOR_KEY_SELECT_DISABLE;
	params.request.common.cursorCompParams.blendingMode[0] = NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA;
	params.request.common.cursorCompParams.blendingMode[1] = NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA;
	return kms.Control(NVKMS_IOCTL_SET_CURSOR_IMAGE, &params, sizeof(params));
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	NvU32 head = argc > 1 ? (NvU32)atoi(argv[1]) : 0;
	int x = argc > 2 ? atoi(argv[2]) : 200;
	int y = argc > 3 ? atoi(argv[3]) : 200;
	double seconds = argc > 4 ? atof(argv[4]) : 1.0;

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);
		NvKmsApi kms;
		NvKmsDevice kmsDev(kms, rmDev.DeviceId());

		PrintCrcs(kms, kmsDev, "before");

		// Orange inside, blue border, opaque: premultiplied ARGB.
		NvKmsBitmap cursor(rmDev, kmsDev, 64, 64, B_RGBA32);
		for (int32 row = 0; row < 64; row++) {
			uint32* line = (uint32*)((uint8*)cursor.Bits() + row * cursor.BytesPerRow());
			for (int32 column = 0; column < 64; column++) {
				bool border = column < 6 || column >= 58 || row < 6 || row >= 58;
				line[column] = border ? 0xff0099ff : 0xffff9900;
			}
		}

		NvKmsMoveCursorParams move {};
		move.request.deviceHandle = kmsDev.Get();
		move.request.dispHandle = kmsDev.Info().dispHandles[0];
		move.request.head = head;
		move.request.common.x = (NvS16)x;
		move.request.common.y = (NvS16)y;
		status_t status = kms.Control(NVKMS_IOCTL_MOVE_CURSOR, &move, sizeof(move));
		printf("move cursor on head %" B_PRIu32 " to %d,%d: %s\n", (uint32)head, x, y, strerror(status));
		status = SetCursor(kms, kmsDev, head, cursor.Surface().Get());
		printf("set cursor image: %s\n", strerror(status));
		snooze(100000);
		PrintCrcs(kms, kmsDev, "shown");
		snooze((bigtime_t)(seconds * 1000000));

		move.request.common.x = (NvS16)(x + 100);
		kms.Control(NVKMS_IOCTL_MOVE_CURSOR, &move, sizeof(move));
		snooze(100000);
		PrintCrcs(kms, kmsDev, "moved 100 right");

		status = SetCursor(kms, kmsDev, head, 0);
		printf("cursor taken away: %s\n", strerror(status));
		snooze(100000);
		PrintCrcs(kms, kmsDev, "after");
	} catch (const std::system_error& ex) {
		printf("[!] %s\n", ex.what());
		return 1;
	}
	return 0;
}
