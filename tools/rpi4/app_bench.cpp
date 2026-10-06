/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Measure a GUI application's registration, first visible window and reply
// from its application looper. Refuse to measure/quit an already running app.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Rect.h>
#include <Roster.h>
#include <WindowInfo.h>


static bool
has_visible_window(team_id team)
{
	int32 count = 0;
	int32* tokens = get_token_list(team, &count);
	bool visible = false;
	for (int32 i = 0; i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info != NULL) {
			visible |= info->show_hide_level == 0 && !info->is_mini;
			free(info);
		}
	}
	free(tokens);
	return visible;
}


int
main(int argc, char** argv)
{
	if (argc != 3 || atoi(argv[1]) < 1 || atoi(argv[1]) > 100) {
		fprintf(stderr, "usage: %s <rounds 1..100> <application path>\n", argv[0]);
		return 2;
	}
	BApplication application("application/x-vnd.airOS-app-bench");
	entry_ref ref;
	status_t status = get_ref_for_path(argv[2], &ref);
	if (status != B_OK) {
		fprintf(stderr, "%s: %s\n", argv[2], strerror(status));
		return 1;
	}

	printf("round,registered_ms,window_ms,responsive_ms,team,application\n");
	for (int round = 1; round <= atoi(argv[1]); round++) {
		team_id team = -1;
		bigtime_t start = system_time();
		status = be_roster->Launch(&ref, (BMessage*)NULL, &team);
		bigtime_t registered = system_time();
		if (status != B_OK) {
			fprintf(stderr, "launch: %s (team %" B_PRId32 ")\n", strerror(status), team);
			return 1;
		}

		bigtime_t window = 0;
		while (system_time() - start < 30000000) {
			if (has_visible_window(team)) {
				window = system_time();
				break;
			}
			snooze(2000);
		}
		BMessenger messenger(NULL, team);
		BMessage request(B_GET_SUPPORTED_SUITES), reply;
		status = messenger.SendMessage(&request, &reply, 1000000, 5000000);
		bigtime_t responsive = system_time();
		bool success = window != 0 && status == B_OK;
		if (success) {
			printf("%d,%.3f,%.3f,%.3f,%" B_PRId32 ",%s\n", round,
				(registered - start) / 1000., (window - start) / 1000.,
				(responsive - start) / 1000., team, argv[2]);
		} else {
			fprintf(stderr, "no window or looper reply: window=%lld status=%s\n",
				(long long)window, strerror(status));
		}
		fflush(stdout);
		BMessage quit(B_QUIT_REQUESTED);
		messenger.SendMessage(&quit, (BHandler*)NULL, 1000000);
		bigtime_t deadline = system_time() + 10000000;
		team_info info;
		while (get_team_info(team, &info) == B_OK && system_time() < deadline)
			snooze(10000);
		if (get_team_info(team, &info) == B_OK) {
			fprintf(stderr, "team %" B_PRId32 " did not quit; stopping benchmark\n", team);
			return 1;
		}
		if (!success)
			return 1;
		snooze(250000);
	}
	return 0;
}
