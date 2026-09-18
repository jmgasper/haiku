/* Does the display tell us when it starts a new frame?
 *
 * Presenting into the screen while it is being scanned out tears, and the cure
 * is to know when the scan is between frames.
 *
 * The first thing tried here was resman's GF100_DISP_SW object, which takes
 * NV9072_CTRL_CMD_NOTIFY_ON_VBLANK and answers through an operating system
 * event. Resman will not allocate that object anywhere except underneath a
 * channel, and the accelerant has none: it drives the display through NVKMS
 * and never touches a channel.
 *
 * NVKMS has its own way, and it needs no channel. A client registers a piece
 * of memory; at every vertical blank NVKMS looks at it, and if the client has
 * changed `requestCounter` since it last looked, it copies that value into
 * `semaphore` along with the frame number. So asking to be told about the next
 * blank is a write, and hearing the answer is a read.
 *
 * This counts those answers. At 60 Hz a two second run should see about 120 of
 * them, about 16.7 ms apart; anything else means the answer is not coming from
 * the display.
 *
 * usage: nvvblank [seconds] [head]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>
#include <NvKmsSurface.h>

#include "NvUtils.h"

extern "C" {
#include "nv-haiku.h"
}


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	double seconds = argc > 1 ? atof(argv[1]) : 2.0;
	uint32 head = argc > 2 ? (uint32)atoi(argv[2]) : 0;

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);
		NvKmsApi kms;
		NvKmsDevice kmsDev(kms, rmDev.DeviceId());

		printf("vblank semaphore control: %s\n",
			kmsDev.Info().supportsVblankSemControl ? "yes" : "NO");
		if (!kmsDev.Info().supportsVblankSemControl)
			return 1;

		// The memory NVKMS looks at. It has to be a registered surface, so it
		// is described as a one row image wide enough to hold the structure.
		const NvU64 kSize = B_PAGE_SIZE;
		NvRmObject memory;
		NvU8 compressible = 0;
		nvKmsKapiAllocateSystemMemory(rmDev, kmsDev, memory,
			NvKmsSurfaceMemoryLayoutPitch, kSize,
			NVKMS_KAPI_ALLOCATION_TYPE_OFFSCREEN, &compressible);

		FileDesc memoryFd = rmDev.ExportObjectToFd(rmDev.Device().Get(),
			memory.Get());

		NvKmsRegisterSurfaceParams surfaceParams {};
		surfaceParams.request.deviceHandle = kmsDev.Get();
		surfaceParams.request.useFd = true;
		surfaceParams.request.planes[0].u.fd = memoryFd.Get();
		surfaceParams.request.planes[0].offset = 0;
		surfaceParams.request.planes[0].pitch = kSize;
		surfaceParams.request.planes[0].rmObjectSizeInBytes = kSize;
		surfaceParams.request.widthInPixels = kSize / 4;
		surfaceParams.request.heightInPixels = 1;
		surfaceParams.request.layout = NvKmsSurfaceMemoryLayoutPitch;
		surfaceParams.request.format = NvKmsSurfaceMemoryFormatX8R8G8B8;
		// Nothing scans this out; it is a place for counters, so the display
		// hardware never looks at it and NVKMS maps it for the processor.
		surfaceParams.request.noDisplayHardwareAccess = true;
		surfaceParams.request.isoType = NVKMS_MEMORY_NISO;
		printf("registering the surface\n");
		CheckErrno(kms.Control(NVKMS_IOCTL_REGISTER_SURFACE, &surfaceParams,
			sizeof(surfaceParams)));
		NvKmsSurface surface(kmsDev, surfaceParams.reply.surfaceHandle);

		printf("mapping the memory\n");
		NvRmMemoryMapping mapping = rmDev.MapMemory(memory.Get(), true, 0,
			kSize, 0);
		auto *data = (volatile NvKmsVblankSemControlData *)mapping.Address();
		memset((void *)data, 0, sizeof(*data));

		NvKmsEnableVblankSemControlParams enableParams {};
		enableParams.request.deviceHandle = kmsDev.Get();
		enableParams.request.dispHandle = kmsDev.Info().dispHandles[0];
		enableParams.request.headMask = 1U << head;
		enableParams.request.surfaceHandle = surface.Get();
		enableParams.request.surfaceOffset = 0;
		printf("enabling on head %" B_PRIu32 " of disp %#" B_PRIx32 "\n",
			head, (uint32)enableParams.request.dispHandle);
		CheckErrno(kms.Control(NVKMS_IOCTL_ENABLE_VBLANK_SEM_CONTROL,
			&enableParams, sizeof(enableParams)));
		printf("watching head %" B_PRIu32 "\n", head);

		volatile NvKmsVblankSemControlDataOneHead &one = data->head[head];

		bigtime_t start = system_time();
		bigtime_t end = start + (bigtime_t)(seconds * 1000000);
		bigtime_t previous = 0;
		bigtime_t shortest = 0, longest = 0, total = 0;
		int64 count = 0;
		NvU32 request = 0;
		bool lost = false;

		while (system_time() < end) {
			// Ask about the next blank: change the counter, then wait for it
			// to be copied across.
			request++;
			one.flags = 0;			// swap interval 0: the very next blank
			one.requestCounterAccel = request;
			one.requestCounter = request;
			// NVKMS expects this counter to be written by a semaphore release
			// from a channel, not by the processor, so nothing here orders the
			// write for us: without a fence it can sit in a write buffer and
			// the display never sees the request.
			__sync_synchronize();

			bigtime_t deadline = system_time() + 1000000;
			while ((NvU32)one.semaphore != request) {
				if (system_time() > deadline) {
					printf("[!] nothing for a whole second, after %" B_PRId64
						" answers\n", count + 1);
					lost = true;
					break;
				}
				snooze(200);
			}
			if (lost)
				break;

			bigtime_t now = system_time();
			if (previous != 0) {
				bigtime_t gap = now - previous;
				if (count == 0 || gap < shortest)
					shortest = gap;
				if (gap > longest)
					longest = gap;
				total += gap;
				count++;
			}
			previous = now;
		}

		NvKmsDisableVblankSemControlParams disableParams {};
		disableParams.request.deviceHandle = kmsDev.Get();
		disableParams.request.dispHandle = kmsDev.Info().dispHandles[0];
		disableParams.request.vblankSemControlHandle
			= enableParams.reply.vblankSemControlHandle;
		kms.Control(NVKMS_IOCTL_DISABLE_VBLANK_SEM_CONTROL, &disableParams,
			sizeof(disableParams));

		if (count < 2) {
			printf("only %" B_PRId64 " answers: the display is not telling us"
				" anything\n", count + 1);
			return 1;
		}

		double average = (double)total / count;
		printf("%" B_PRId64 " answers: one every %.2f ms (%.1f a second),"
			" shortest %.2f, longest %.2f, frame %llu\n",
			count + 1, average / 1000.0, 1000000.0 / average,
			shortest / 1000.0, longest / 1000.0,
			(unsigned long long)one.vblankCount);
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] %s\n", ex.what());
		return 1;
	}

	return 0;
}
