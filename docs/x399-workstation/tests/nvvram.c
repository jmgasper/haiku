/* Read VRAM words through nvidia_rm's PRAMIN development ioctl. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>

typedef struct { unsigned long long offset; unsigned int values[16]; } params_t;

int main(int argc, char **argv)
{
	int fd = open("/dev/graphics/nvidia0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	unsigned long long offset = strtoull(argv[1], NULL, 0);
	int words = argc > 2 ? atoi(argv[2]) : 16;
	for (int done = 0; done < words; done += 16) {
		params_t p = { offset + done * 4 };
		if (ioctl(fd, 10000 + 3, &p, sizeof(p)) != 0) { perror("ioctl"); return 1; }
		printf("%#llx:", p.offset);
		for (int i = 0; i < 16 && done + i < words; i++)
			printf(" %08x", p.values[i]);
		printf("\n");
	}
	return 0;
}
