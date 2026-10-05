/* Lab tool: exercise and observe the kernel's x86 suspend support.
 *   x86suspend restart <cpu>      park a CPU and restart it via the trampoline
 *   x86suspend s3 [flags [cp]]    enter S3 (flags: 1 = power off after resume,
 *                                 2 = skip devices, 4 = log every step with
 *                                 pauses, 8 = skip driver power hooks,
 *                                 16 = skip the device tree); with a checkpoint
 *                                 the resume path powers off when reaching it
 *   x86suspend trace              print the trace of the last suspend
 *   x86suspend watch <host> <port> [interval [file]]
 *                                 stay resident and report the trace whenever
 *                                 it changes, to host:port and to a file (on a
 *                                 USB stick, say). Started before suspending,
 *                                 this reports how resuming went even when the
 *                                 network or the boot disk do not come back.
 */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <SupportDefs.h>

extern status_t _kern_generic_syscall(const char *subsystem, uint32 function,
	void *buffer, size_t bufferSize);

#define SUBSYSTEM "x86_suspend"

static status_t
get_trace(char *buffer, size_t size)
{
	memset(buffer, 0, size);
	return _kern_generic_syscall(SUBSYSTEM, 3, buffer, size);
}

static void
send_trace(const char *host, int port, const char *trace)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return;

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = inet_addr(host);

	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
		write(fd, trace, strlen(trace));

	close(fd);
}

int
main(int argc, char **argv)
{
	status_t status;

	if (argc >= 3 && strcmp(argv[1], "restart") == 0) {
		int32 cpu = atoi(argv[2]);
		status = _kern_generic_syscall(SUBSYSTEM, 1, &cpu, sizeof(cpu));
	} else if (argc >= 2 && strcmp(argv[1], "s3") == 0) {
		struct { uint32 flags; uint32 checkpoint; } args;
		args.flags = argc >= 3 ? strtoul(argv[2], NULL, 0) : 0;
		args.checkpoint = argc >= 4 ? strtoul(argv[3], NULL, 0) : 0;
		status = _kern_generic_syscall(SUBSYSTEM, 2, &args, sizeof(args));
	} else if (argc >= 2 && strcmp(argv[1], "trace") == 0) {
		static char trace[4096];
		status = get_trace(trace, sizeof(trace));
		if (status == B_OK)
			fputs(trace, stdout);
	} else if (argc >= 4 && strcmp(argv[1], "watch") == 0) {
		static char trace[4096], last[4096];
		int port = atoi(argv[3]);
		int interval = argc >= 5 ? atoi(argv[4]) : 5;
		const char *path = argc >= 6 ? argv[5] : NULL;

		// Touch everything this needs while the disk still works: after a bad
		// resume nothing can be loaded from disk any more.
		get_trace(trace, sizeof(trace));
		send_trace(argv[2], port, "watching\n");
		strcpy(last, trace);

		while (1) {
			sleep(interval);
			if (get_trace(trace, sizeof(trace)) != B_OK)
				continue;
			if (strcmp(trace, last) == 0)
				continue;
			strcpy(last, trace);
			send_trace(argv[2], port, trace);

			// Opened per write: the volume may only appear after resuming.
			if (path != NULL) {
				int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (fd >= 0) {
					write(fd, trace, strlen(trace));
					fsync(fd);
					close(fd);
				}
			}
		}
	} else {
		fprintf(stderr, "usage: %s restart <cpu> | s3 [flags [checkpoint]] | "
			"trace | watch <host> <port> [interval]\n", argv[0]);
		return 2;
	}

	printf("result: %s (%d)\n", strerror(status), (int)status);
	return status == B_OK ? 0 : 1;
}
