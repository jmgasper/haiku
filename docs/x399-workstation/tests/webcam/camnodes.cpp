// Lists the live video input nodes and the default video input.
// g++ -o camnodes camnodes.cpp -lbe -lmedia
#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <MediaRoster.h>


int
main()
{
	BApplication app("application/x-vnd.airOS-camnodes");
	status_t status;
	BMediaRoster* roster = BMediaRoster::Roster(&status);
	if (roster == NULL) {
		printf("no media roster: %s\n", strerror(status));
		return 1;
	}

	live_node_info info[16];
	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	int32 count = 16;
	status = roster->GetLiveNodes(info, &count, NULL, &format, NULL,
		B_BUFFER_PRODUCER | B_PHYSICAL_INPUT);
	printf("live raw video inputs: %s, %d\n", strerror(status), (int)count);
	for (int32 i = 0; i < count; i++)
		printf("  node %d \"%s\"\n", (int)info[i].node.node, info[i].name);

	count = 16;
	status = roster->GetLiveNodes(info, &count, NULL, NULL, NULL,
		B_BUFFER_PRODUCER | B_PHYSICAL_INPUT);
	printf("live physical inputs: %s, %d\n", strerror(status), (int)count);
	for (int32 i = 0; i < count; i++) {
		printf("  node %d \"%s\"\n", (int)info[i].node.node, info[i].name);
		media_output outputs[4];
		int32 outputCount = 0;
		roster->GetAllOutputsFor(info[i].node, outputs, 4, &outputCount);
		for (int32 j = 0; j < outputCount; j++) {
			printf("    output \"%s\" type %d\n", outputs[j].name,
				(int)outputs[j].format.type);
		}
	}

	media_node node;
	status = roster->GetVideoInput(&node);
	printf("default video input: %s, node %d\n", strerror(status),
		(int)node.node);
	return 0;
}
