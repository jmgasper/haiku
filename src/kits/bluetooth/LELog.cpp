/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <LELog.h>

#include <driver_settings.h>
#include <FindDirectory.h>
#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>


namespace Bluetooth {

static const off_t kMaximumLogSize = 1024 * 1024;
static const bigtime_t kSettingsRecheckInterval = 2000000;

static pthread_mutex_t sLogLock = PTHREAD_MUTEX_INITIALIZER;
static int32 sLevel = LE_LOG_INFO;
static bigtime_t sLevelCheckedAt = -1;
static char sLogPath[PATH_MAX];


static bool
SettingsPath(char* path, size_t capacity)
{
	char settings[PATH_MAX];
	if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, false, settings,
			sizeof(settings)) != B_OK)
		return false;
	int length = snprintf(path, capacity, "%s/bluetooth/le_logging", settings);
	return length > 0 && (size_t)length < capacity;
}


static int32
ClampLevel(long level)
{
	if (level < LE_LOG_ERROR)
		return LE_LOG_ERROR;
	if (level > LE_LOG_TRACE)
		return LE_LOG_TRACE;
	return (int32)level;
}


static int32
ReadLevel()
{
	const char* environment = getenv("BT_LE_LOG");
	if (environment != NULL && environment[0] != '\0')
		return ClampLevel(strtol(environment, NULL, 10));

	char path[PATH_MAX];
	if (!SettingsPath(path, sizeof(path)))
		return LE_LOG_INFO;
	void* handle = load_driver_settings(path);
	if (handle == NULL)
		return LE_LOG_INFO;
	const char* value = get_driver_parameter(handle, "level", NULL, NULL);
	int32 level = value != NULL ? ClampLevel(strtol(value, NULL, 10))
		: LE_LOG_INFO;
	unload_driver_settings(handle);
	return level;
}


int32
LELogLevel()
{
	pthread_mutex_lock(&sLogLock);
	bigtime_t now = system_time();
	if (sLevelCheckedAt < 0 || now - sLevelCheckedAt > kSettingsRecheckInterval) {
		sLevel = ReadLevel();
		sLevelCheckedAt = now;
	}
	int32 level = sLevel;
	pthread_mutex_unlock(&sLogLock);
	return level;
}


status_t
LESetLogLevel(int32 level)
{
	level = ClampLevel(level);
	char path[PATH_MAX];
	if (!SettingsPath(path, sizeof(path)))
		return B_ERROR;
	char* slash = strrchr(path, '/');
	if (slash != NULL) {
		*slash = '\0';
		mkdir(path, 0755);
		*slash = '/';
	}

	char temporary[PATH_MAX];
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	FILE* file = fopen(temporary, "w");
	if (file == NULL)
		return errno;
	fprintf(file,
		"# Bluetooth LE diagnostics: 0 errors, 1 steps, 2 debug + kernel "
			"traces, 3 packet dumps\n"
		"level %" B_PRId32 "\n", level);
	bool ok = fflush(file) == 0;
	ok = fclose(file) == 0 && ok;
	if (!ok || rename(temporary, path) != 0) {
		unlink(temporary);
		return B_IO_ERROR;
	}

	pthread_mutex_lock(&sLogLock);
	sLevel = level;
	sLevelCheckedAt = system_time();
	pthread_mutex_unlock(&sLogLock);
	LELog(LE_LOG_ERROR, "log", "log level set to %" B_PRId32, level);
	return B_OK;
}


const char*
LELogFilePath()
{
	pthread_mutex_lock(&sLogLock);
	if (sLogPath[0] == '\0') {
		char directory[PATH_MAX];
		if (find_directory(B_USER_LOG_DIRECTORY, -1, true, directory,
				sizeof(directory)) != B_OK) {
			strlcpy(directory, "/boot/home/config/var/log", sizeof(directory));
			mkdir("/boot/home/config/var", 0755);
			mkdir(directory, 0755);
		}
		snprintf(sLogPath, sizeof(sLogPath), "%s/bluetooth_le.log", directory);
	}
	pthread_mutex_unlock(&sLogLock);
	return sLogPath;
}


static void
WriteLine(const char* line)
{
	const char* path = LELogFilePath();
	pthread_mutex_lock(&sLogLock);
	struct stat info;
	if (stat(path, &info) == 0 && info.st_size > kMaximumLogSize) {
		char old[PATH_MAX];
		snprintf(old, sizeof(old), "%s.old", path);
		rename(path, old);
	}
	int descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (descriptor >= 0) {
		size_t length = strlen(line);
		while (length > 0) {
			ssize_t written = write(descriptor, line, length);
			if (written <= 0)
				break;
			line += written;
			length -= written;
		}
		close(descriptor);
	}
	pthread_mutex_unlock(&sLogLock);
}


static void
VLog(int32 level, const char* component, const char* format, va_list args)
{
	char message[1024];
	vsnprintf(message, sizeof(message), format, args);

	char timestamp[32];
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);

	static const char* kLevelNames[] = { "ERR", "INF", "DBG", "TRC" };
	char line[1200];
	snprintf(line, sizeof(line), "%s.%03d [%" B_PRId32 "/%" B_PRId32
		"] %s %s: %s\n", timestamp, (int)((system_time() / 1000) % 1000),
		getpid(), find_thread(NULL), kLevelNames[ClampLevel(level)],
		component != NULL ? component : "le", message);
	WriteLine(line);

	// Mirror failures, and the pairing steps, into syslog next to the
	// kernel's traces; routine reconnect/scan chatter stays in the file.
	bool pairing = component != NULL && (strcmp(component, "pair") == 0
		|| strcmp(component, "smp") == 0);
	if (level == LE_LOG_ERROR || (level == LE_LOG_INFO && pairing)) {
		syslog(level == LE_LOG_ERROR ? LOG_ERR : LOG_INFO, "bluetooth-le %s: %s",
			component != NULL ? component : "le", message);
	}
}


void
LELog(int32 level, const char* component, const char* format, ...)
{
	if (level > LELogLevel())
		return;
	va_list args;
	va_start(args, format);
	VLog(level, component, format, args);
	va_end(args);
}


void
LELogHex(int32 level, const char* component, const char* label,
	const void* data, size_t length)
{
	if (level > LELogLevel())
		return;
	const uint8* bytes = (const uint8*)data;
	const size_t shown = length < 1024 ? length : 1024;
	if (shown == 0) {
		LELog(level, component, "%s (0 bytes)", label);
		return;
	}
	for (size_t line = 0; line < shown; line += 32) {
		char hex[3 * 32 + 1];
		size_t offset = 0;
		for (size_t i = line; i < shown && i < line + 32; i++)
			offset += snprintf(hex + offset, sizeof(hex) - offset, "%02x ",
				bytes[i]);
		if (offset > 0)
			hex[offset - 1] = '\0';
		if (line == 0) {
			LELog(level, component, "%s (%zu bytes%s): %s", label, length,
				shown < length ? ", truncated" : "", hex);
		} else
			LELog(level, component, "  +%04zx: %s", line, hex);
	}
}


const char*
LEAddressString(const uint8 address[6], char buffer[18])
{
	if (address == NULL) {
		strlcpy(buffer, "(null)", 18);
		return buffer;
	}
	snprintf(buffer, 18, "%02X:%02X:%02X:%02X:%02X:%02X", address[5],
		address[4], address[3], address[2], address[1], address[0]);
	return buffer;
}

} // namespace Bluetooth
