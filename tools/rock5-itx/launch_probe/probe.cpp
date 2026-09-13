/*
 * Exercise the launch daemon's actual environment reader while an unrelated
 * executable is started at the pipe-inheritance boundary.
 * Distributed under the terms of the MIT License.
 */

#define _DEFAULT_SOURCE
#include "../../../src/servers/launch/BaseJob.h"

#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


static const char* sExecutable;
static int sControl[2] = {-1, -1};
static pid_t sHolder = -1;
static bool sArmed;
static bool sClearCloseOnExec;
static int sPipeFlags[2];
static int sInjectionError;
static sem_id sCompleted;
static BStringList sEnvironment;

extern "C" int __real_pipe(int*);
extern "C" int __real_pipe2(int*, int);


static void
StartUnrelatedExecutable(int* descriptors)
{
	if (!sArmed)
		return;
	sArmed = false;
	for (int i = 0; i < 2; i++) {
		sPipeFlags[i] = fcntl(descriptors[i], F_GETFD);
		if (sPipeFlags[i] < 0) {
			sInjectionError = errno;
			return;
		}
		if (sClearCloseOnExec
			&& fcntl(descriptors[i], F_SETFD,
				sPipeFlags[i] & ~FD_CLOEXEC) != 0) {
			sInjectionError = errno;
			return;
		}
	}

	struct stat info;
	if (fstat(descriptors[1], &info) != 0) {
		sInjectionError = errno;
		return;
	}
	char readFD[32], writeFD[32], device[32], node[32];
	snprintf(readFD, sizeof(readFD), "%d", descriptors[0]);
	snprintf(writeFD, sizeof(writeFD), "%d", descriptors[1]);
	snprintf(device, sizeof(device), "%lld", (long long)info.st_dev);
	snprintf(node, sizeof(node), "%lld", (long long)info.st_ino);
	const char* arguments[] = {sExecutable, "--holder", readFD, writeFD,
		device, node, NULL};

	posix_spawn_file_actions_t actions;
	sInjectionError = posix_spawn_file_actions_init(&actions);
	if (sInjectionError != 0)
		return;
	sInjectionError = posix_spawn_file_actions_adddup2(&actions, sControl[0],
		STDIN_FILENO);
	if (sInjectionError == 0) {
		sInjectionError = posix_spawn(&sHolder, sExecutable, &actions, NULL,
			(char* const*)arguments, NULL);
	}
	posix_spawn_file_actions_destroy(&actions);
}


extern "C" int
__wrap_pipe(int* descriptors)
{
	int result = __real_pipe(descriptors);
	if (result == 0)
		StartUnrelatedExecutable(descriptors);
	return result;
}


extern "C" int
__wrap_pipe2(int* descriptors, int flags)
{
	int result = __real_pipe2(descriptors, flags);
	if (result == 0)
		StartUnrelatedExecutable(descriptors);
	return result;
}


class EnvironmentJob : public BaseJob {
public:
	EnvironmentJob() : BaseJob("environment inheritance probe") {}
	virtual status_t Execute() { return B_OK; }
};


static int32
ReadEnvironment(void* path)
{
	EnvironmentJob job;
	job.EnvironmentSourceFiles().Add((const char*)path);
	job.GetSourceFilesEnvironment(sEnvironment);
	release_sem(sCompleted);
	return B_OK;
}


static int
HoldDescriptors(char** arguments)
{
	dev_t device = (dev_t)strtoll(arguments[4], NULL, 10);
	ino_t node = (ino_t)strtoll(arguments[5], NULL, 10);
	bool inherited = false;
	for (int i = 2; i < 4; i++) {
		struct stat info;
		if (fstat(atoi(arguments[i]), &info) == 0
			&& info.st_dev == device && info.st_ino == node) {
			inherited = true;
		}
	}
	char release;
	ssize_t count;
	do {
		count = read(STDIN_FILENO, &release, 1);
	} while (count < 0 && errno == EINTR);
	return count == 1 && release == 'x' ? (inherited ? 1 : 0) : 2;
}


int
main(int argc, char** argv)
{
	if (argc == 6 && strcmp(argv[1], "--holder") == 0)
		return HoldDescriptors(argv);
	bool expectLeak = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--expect-leak") == 0)
			expectLeak = true;
		else if (strcmp(argv[i], "--clear-cloexec") == 0)
			sClearCloseOnExec = true;
		else
			return 2;
	}
	sExecutable = argv[0];
	char path[] = "/boot/home/rock5-lab/launch-env-XXXXXX";
	int script = mkstemp(path);
	const char contents[] = "export ROCK5_LAUNCH_ENV_PROBE=ok\n";
	if (script < 0)
		return 2;
	bool prepared = write(script, contents, sizeof(contents) - 1)
		== sizeof(contents) - 1;
	close(script);
	if (!prepared || __real_pipe2(sControl, O_CLOEXEC) != 0) {
		unlink(path);
		return 2;
	}
	sCompleted = create_sem(0, "environment read completed");
	sArmed = true;
	thread_id reader = spawn_thread(ReadEnvironment, "environment reader",
		B_NORMAL_PRIORITY, path);
	if (sCompleted < 0 || reader < 0 || resume_thread(reader) != B_OK)
		return 2;
	status_t waited = acquire_sem_etc(sCompleted, 1, B_RELATIVE_TIMEOUT, 5000000);
	bool delayed = waited == B_TIMED_OUT;
	int holderStatus = -1;
	bool alive = sHolder > 0 && waitpid(sHolder, &holderStatus, WNOHANG) == 0;
	bool released = write(sControl[1], "x", 1) == 1;
	close(sControl[0]);
	close(sControl[1]);
	status_t readerStatus = B_ERROR;
	bool joined = wait_for_thread_etc(reader, B_RELATIVE_TIMEOUT, 5000000,
		&readerStatus) == B_OK;
	bigtime_t deadline = system_time() + 5000000;
	pid_t reaped = -1;
	if (sHolder > 0) {
		do {
			reaped = waitpid(sHolder, &holderStatus, WNOHANG);
			if (reaped != 0)
				break;
			snooze(1000);
		} while (system_time() < deadline);
		if (reaped == 0) {
			kill(sHolder, SIGKILL);
			waitpid(sHolder, &holderStatus, 0);
		}
	}
	bool childValid = reaped == sHolder && WIFEXITED(holderStatus)
		&& WEXITSTATUS(holderStatus) <= 1;
	bool inherited = childValid && WEXITSTATUS(holderStatus) == 1;
	bool value = joined && sEnvironment.HasString("ROCK5_LAUNCH_ENV_PROBE=ok");
	bool passed = sInjectionError == 0 && !sArmed && alive && released
		&& joined && readerStatus == B_OK && value && childValid
		&& (waited == B_OK || waited == B_TIMED_OUT)
		&& delayed == expectLeak && inherited == expectLeak;
	printf("ROCK5_LAUNCH_ENV_RESULT flags=%d,%d inherited=%d delayed=%d "
		"value=%d alive=%d joined=%d error=%d expect_leak=%d status=%s\n",
		sPipeFlags[0], sPipeFlags[1], inherited, delayed, value, alive, joined,
		sInjectionError, expectLeak, passed ? "pass" : "fail");
	unlink(path);
	delete_sem(sCompleted);
	return passed ? 0 : 1;
}
