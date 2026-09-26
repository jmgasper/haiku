/* Proves that a program other than the accelerant can reach the screen's frame
 * buffer in video memory.
 *
 * The accelerant publishes the frame buffer through the driver; this asks for
 * it, duplicates the memory object into its own resman client, maps it and
 * paints a band across the screen. What appears on the monitor is written
 * straight into video memory, which is the point: a program rendering with the
 * GPU can put finished frames there without sending them over the bus.
 *
 * usage: nvscanout [--info] [--band y height] [--dump file]
 *   --dump writes the frame buffer's pixels to a file, row by row, as they
 *   are in memory (B_RGB32: blue, green, red, pad), for looking at the
 *   picture the card scans out without any screenshot code in between.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <OS.h>
#include <GraphicsDefs.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

extern "C" {
#include "nv-haiku.h"
}


static const char *ColorSpaceName(uint32 colorSpace)
{
	switch (colorSpace) {
		case B_RGB32: return "B_RGB32";
		case B_RGBA32: return "B_RGBA32";
		case B_RGB16: return "B_RGB16";
		case B_RGB15: return "B_RGB15";
		default: return "?";
	}
}


int main(int argc, char **argv)
{
	bool infoOnly = false;
	int bandY = -1, bandHeight = 64;
	const char *dumpPath = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--info") == 0) {
			infoOnly = true;
		} else if (strcmp(argv[i], "--band") == 0 && i + 2 < argc) {
			bandY = atoi(argv[++i]);
			bandHeight = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			dumpPath = argv[++i];
		} else {
			fprintf(stderr, "usage: nvscanout [--info] [--band y height] [--dump file]\n");
			return 1;
		}
	}

	int ctlFd = open("/dev/" NVIDIA_CONTROL_DEVICE_NAME, O_RDWR | O_CLOEXEC);
	if (ctlFd < 0) {
		perror("opening the control device");
		return 1;
	}

	nv_haiku_scanout_info info {};
	if (ioctl(ctlFd, NV_HAIKU_BASE + NV_HAIKU_GET_SCANOUT, &info,
			sizeof(info)) < 0) {
		perror("asking for the frame buffer");
		fprintf(stderr, "the accelerant has not published one yet\n");
		return 1;
	}

	printf("frame buffer: %" B_PRIu32 "x%" B_PRIu32 ", %" B_PRIu32
		" bytes per row, %s, %" B_PRIu64 " bytes\n",
		info.width, info.height, info.bytes_per_row,
		ColorSpaceName(info.color_space), info.size);
	printf("owner: client %#" B_PRIx32 ", memory %#" B_PRIx32 "\n",
		info.client, info.memory);
	if (infoOnly)
		return 0;

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		bigtime_t start = system_time();
		NvRmObject framebuffer(rm, rm.DupObject(rmDev.Device().Get(),
			info.client, info.memory));
		printf("duplicated as %#" B_PRIx32 " in %" B_PRId64 " us\n",
			framebuffer.Get(), system_time() - start);

		NvRmMemoryMapping mapping = rmDev.MapMemory(framebuffer.Get(), false, 0,
			info.size, 0);
		uint8 *bits = (uint8*)mapping.Address();
		printf("mapped at %p\n", bits);

		if (dumpPath != NULL) {
			FILE *file = fopen(dumpPath, "wb");
			if (file == NULL) {
				perror("opening the dump file");
				return 1;
			}
			start = system_time();
			for (uint32 y = 0; y < info.height; y++) {
				fwrite(bits + (uint64)y * info.bytes_per_row, 4, info.width,
					file);
			}
			fclose(file);
			printf("dumped %" B_PRIu32 "x%" B_PRIu32 " pixels to %s in %"
				B_PRId64 " ms\n", info.width, info.height, dumpPath,
				(system_time() - start) / 1000);
			return 0;
		}

		if (bandY < 0)
			bandY = info.height / 2 - bandHeight / 2;
		if (bandY + bandHeight > (int)info.height)
			bandHeight = info.height - bandY;

		// A horizontal gradient band, so that both the placement and the byte
		// order are visible on the monitor.
		start = system_time();
		for (int y = bandY; y < bandY + bandHeight; y++) {
			uint32 *row = (uint32*)(bits + (uint64)y * info.bytes_per_row);
			for (uint32 x = 0; x < info.width; x++) {
				uint8 r = 255 * x / info.width;
				uint8 g = 255 * (y - bandY) / bandHeight;
				row[x] = ((uint32)r << 16) | ((uint32)g << 8) | 0x40;
			}
		}
		bigtime_t elapsed = system_time() - start;
		double bytes = (double)bandHeight * info.width * 4;
		printf("painted %d rows at y=%d in %" B_PRId64 " us (%.1f GB/s)\n",
			bandHeight, bandY, elapsed, bytes / elapsed / 1000.0);
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] %s\n", ex.what());
		return 1;
	}

	return 0;
}
