/* Does the display tell us when it starts a new frame?
 *
 * Presenting into the screen while it is being scanned out tears, and the cure
 * is to know when the scan is between frames. Resman can say so: a
 * GF100_DISP_SW object takes a request to be notified at the next vertical
 * blank, and delivers it through an operating system event, which on Haiku
 * means the driver wakes anyone selecting on the file.
 *
 * This counts those notifications. At 60 Hz a two second run should see about
 * 120 of them, evenly spaced about 16.7 ms apart; anything else means the
 * notification is not coming from the display.
 *
 * It does not work yet: resman will only allocate GF100_DISP_SW underneath a
 * channel (see the resource list in the kernel modules: its parent is
 * KernelChannel), and the accelerant has none - it drives the display through
 * NVKMS and never touches a channel. The two ways on from here are to give the
 * accelerant a channel of its own purely to hang this object off, or to use
 * NVKMS's own vblank semaphore control, which writes a counter into a surface
 * at each blank and needs no channel. The latter also lets the GPU wait for
 * the blank itself rather than the processor waiting and then submitting.
 *
 * usage: nvvblank [seconds] [head]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

extern "C" {
#include "nv-haiku.h"
#include "nvos.h"
#include "class/cl0005.h"
#include "class/cl9072.h"
#include "class/cl9072_notification.h"
#include "ctrl/ctrl9072.h"
}


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	double seconds = argc > 1 ? atof(argv[1]) : 2.0;
	uint32 head = argc > 2 ? (uint32)atoi(argv[2]) : 0;

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		NV9072_ALLOCATION_PARAMETERS dispSwParams = {
			.logicalHeadId = head,
			.displayMask = 0,
			.caps = 0,
		};
		NvRmObject dispSw = rmDev.Device().Alloc(GF100_DISP_SW, &dispSwParams);
		printf("display software object %#" B_PRIx32 " on head %" B_PRIu32 "\n",
			dispSw.Get(), head);

		// The event is delivered on its own file: resman is told about the
		// file, and the driver wakes whoever is selecting on it.
		FileDesc eventFd(open("/dev/" NVIDIA_CONTROL_DEVICE_NAME,
			O_RDWR | O_CLOEXEC));
		CheckErrno(eventFd.Get());
		rm.AllocOsEvent(eventFd.Get());

		NV0005_ALLOC_PARAMETERS eventParams = {
			.hParentClient = rm.Client().Get(),
			.hSrcResource = dispSw.Get(),
			.hClass = NV01_EVENT_OS_EVENT,
			.notifyIndex = NV9072_NOTIFIERS_NOTIFY_ON_VBLANK,
			.data = (NvP64)(uintptr_t)eventFd.Get(),
		};
		NvRmObject event = dispSw.Alloc(NV01_EVENT_OS_EVENT, &eventParams);
		printf("event object %#" B_PRIx32 ", waiting on fd %d\n",
			event.Get(), eventFd.Get());

		bigtime_t start = system_time();
		bigtime_t end = start + (bigtime_t)(seconds * 1000000);
		bigtime_t previous = 0;
		bigtime_t shortest = 0, longest = 0;
		int64 total = 0;
		int64 count = 0;
		int timeouts = 0;

		while (system_time() < end) {
			// Ask to be told at the next blank; the request is for one frame,
			// so it goes in again each time round.
			NV9072_CTRL_CMD_NOTIFY_ON_VBLANK_PARAMS notifyParams = {
				.data = 0,
				.bHeadDisabled = false,
			};
			dispSw.Control(NV9072_CTRL_CMD_NOTIFY_ON_VBLANK, &notifyParams,
				sizeof(notifyParams));

			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(eventFd.Get(), &readSet);
			struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
			int ready = select(eventFd.Get() + 1, &readSet, NULL, NULL,
				&timeout);
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				perror("select");
				break;
			}
			if (ready == 0) {
				timeouts++;
				printf("[!] nothing for a whole second\n");
				if (timeouts > 2)
					break;
				continue;
			}

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

		if (count < 2) {
			printf("only %" B_PRId64 " notifications: the display is not"
				" telling us anything\n", count + 1);
			return 1;
		}

		double average = (double)total / count;
		printf("%" B_PRId64 " notifications: one every %.2f ms"
			" (%.1f a second), shortest %.2f, longest %.2f\n",
			count + 1, average / 1000.0, 1000000.0 / average,
			shortest / 1000.0, longest / 1000.0);
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] %s\n", ex.what());
		return 1;
	}

	return 0;
}
