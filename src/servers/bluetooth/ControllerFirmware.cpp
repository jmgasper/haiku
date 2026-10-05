/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "ControllerFirmware.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include <Autolock.h>
#include <Locker.h>
#include <String.h>

#include "BluetoothServer.h"


extern char** environ;


namespace ControllerFirmware {


// Intel controllers start in ROM or bootloader mode and Realtek ones without
// their patch; bt_firmware brings them up (MediaTek radios are set up by
// h2generic itself).
static const char* kLoaderPath = "/bin/bt_firmware";

static BLocker sLock("controller firmware");
static std::set<std::string> sPrepared;
	// usb_raw paths of the controllers given to bt_firmware already


/*!	Runs bt_firmware with \a arguments. Its standard output is returned in
	\a output when that is given, and goes to the syslog otherwise, as does
	its standard error.
*/
static status_t
RunLoader(const std::vector<std::string>& arguments, std::string* output,
	int* _exitStatus)
{
	std::vector<char*> argv;
	argv.push_back((char*)"bt_firmware");
	for (size_t i = 0; i < arguments.size(); i++)
		argv.push_back((char*)arguments[i].c_str());
	argv.push_back(NULL);

	int pipeFDs[2] = { -1, -1 };
	if (output != NULL && pipe(pipeFDs) != 0)
		return errno;

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
		O_RDONLY, 0);
	if (output != NULL) {
		posix_spawn_file_actions_adddup2(&actions, pipeFDs[1], STDOUT_FILENO);
		posix_spawn_file_actions_addclose(&actions, pipeFDs[0]);
		posix_spawn_file_actions_addclose(&actions, pipeFDs[1]);
	} else {
		posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
			"/dev/dprintf", O_WRONLY, 0);
	}
	posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/dprintf",
		O_WRONLY, 0);

	pid_t child;
	int error = posix_spawn(&child, kLoaderPath, &actions, NULL, argv.data(),
		environ);
	posix_spawn_file_actions_destroy(&actions);
	if (output != NULL)
		close(pipeFDs[1]);
	if (error != 0) {
		if (output != NULL)
			close(pipeFDs[0]);
		return error;
	}

	if (output != NULL) {
		char buffer[512];
		ssize_t bytes;
		while ((bytes = read(pipeFDs[0], buffer, sizeof(buffer))) > 0
				|| (bytes < 0 && errno == B_INTERRUPTED)) {
			if (bytes > 0)
				output->append(buffer, bytes);
		}
		close(pipeFDs[0]);
	}

	int status;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != B_INTERRUPTED)
			return errno;
	}
	*_exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return B_OK;
}


void
PrepareNewControllers()
{
	BAutolock locker(sLock);
	if (access(kLoaderPath, X_OK) != 0)
		return;

	// "--list" prints "<usb_raw path>  <description>" for each Intel or
	// Realtek controller, and fails when there is none.
	std::string list;
	int exitStatus;
	if (RunLoader(std::vector<std::string>(1, "--list"), &list, &exitStatus)
			!= B_OK || exitStatus != 0) {
		sPrepared.clear();
		return;
	}

	std::set<std::string> attached;
	std::vector<std::string> added;
	size_t start = 0;
	while (start < list.size()) {
		size_t end = list.find('\n', start);
		if (end == std::string::npos)
			end = list.size();
		std::string line = list.substr(start, end - start);
		start = end + 1;
		std::string path = line.substr(0, line.find(' '));
		if (path.compare(0, 13, "/dev/bus/usb/") != 0)
			continue;
		attached.insert(path);
		if (sPrepared.find(path) == sPrepared.end())
			added.push_back(path);
	}

	// A path that went away may come back as another device.
	for (std::set<std::string>::iterator iterator = sPrepared.begin();
			iterator != sPrepared.end();) {
		if (attached.find(*iterator) == attached.end())
			sPrepared.erase(iterator++);
		else
			iterator++;
	}
	if (added.empty())
		return;

	for (size_t i = 0; i < added.size(); i++) {
		LogStartup("preparing the controller at %s", added[i].c_str());
		sPrepared.insert(added[i]);
	}
	status_t status = RunLoader(added, NULL, &exitStatus);
	if (status != B_OK)
		LogStartup("bt_firmware could not run: %s", strerror(status));
	else if (exitStatus != 0)
		LogStartup("bt_firmware failed (exit status %d)", exitStatus);
	else
		LogStartup("controller firmware ready");
}


}	// namespace ControllerFirmware
