/* Read NVIDIA GPU registers through nvidia_rm's development ioctl. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>

typedef struct { unsigned int offset; unsigned int value; } params_t;

int main(int argc, char **argv)
{
	int fd = open("/dev/graphics/nvidia0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	for (int i = 1; i < argc; i++) {
		params_t p = { (unsigned)strtoul(argv[i], NULL, 0), 0 };
		if (ioctl(fd, 10000 + 2, &p, sizeof(p)) != 0) { perror("ioctl"); return 1; }
		printf("%#08x = %#010x\n", p.offset, p.value);
	}
	close(fd);
	return 0;
}
