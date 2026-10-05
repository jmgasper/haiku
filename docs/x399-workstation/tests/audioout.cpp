/* List the machine's audio outputs, and choose which one the system uses.
 *
 * With both the motherboard's codec and the graphics card's HDMI codec
 * working, there are two, and something has to say which one sound goes to.
 * Run with no arguments to see them; with a number to make that one the
 * system's output.
 *
 * usage: audioout [index]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <MediaRoster.h>
#include <MediaAddOn.h>
#include <OS.h>


int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	BApplication app("application/x-vnd.x399-audioout");
	BMediaRoster *roster = BMediaRoster::Roster();
	if (roster == NULL) {
		fprintf(stderr, "[!] no media server\n");
		return 1;
	}

	// The outputs are already running, so ask for live nodes rather than
	// trying to start a dormant one.
	live_node_info nodes[32];
	int32 count = 32;
	status_t status = roster->GetLiveNodes(nodes, &count, NULL, NULL, NULL,
		B_PHYSICAL_OUTPUT);
	if (status != B_OK || count == 0) {
		fprintf(stderr, "[!] asking for the outputs: %s\n", strerror(status));
		return 1;
	}

	media_node current;
	int32 currentId = -1;
	if (roster->GetAudioOutput(&current) == B_OK)
		currentId = current.node;

	for (int32 i = 0; i < count; i++) {
		printf("%" B_PRId32 ": %-24s node %" B_PRId32 "%s\n", i, nodes[i].name,
			nodes[i].node.node,
			nodes[i].node.node == currentId ? "  <- in use" : "");
	}

	if (argc > 1) {
		int32 want = atoi(argv[1]);
		if (want < 0 || want >= count) {
			fprintf(stderr, "[!] there is no output %" B_PRId32 "\n", want);
			return 1;
		}

		status = roster->SetAudioOutput(nodes[want].node);
		if (status != B_OK) {
			fprintf(stderr, "[!] choosing %s: %s\n", nodes[want].name,
				strerror(status));
			return 1;
		}
		printf("sound now goes to %s (node %" B_PRId32 ")\n", nodes[want].name,
			nodes[want].node.node);
	}

	return 0;
}
