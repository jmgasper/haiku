// Lists the live video input nodes, the default video input and its controls.
// g++ -o camnodes camnodes.cpp -lbe -lmedia
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>


int
main(int argc, char** argv)
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
	if (status != B_OK)
		return 1;

	// its controls; "camnodes <name> <value>" sets one
	BParameterWeb* web = NULL;
	if (roster->GetParameterWebFor(node, &web) == B_OK && web != NULL) {
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			bigtime_t when;
			if (parameter->Type() == BParameter::B_CONTINUOUS_PARAMETER) {
				BContinuousParameter* continuous
					= static_cast<BContinuousParameter*>(parameter);
				float value = 0;
				size_t size = sizeof(value);
				if (argc > 2 && strcmp(argv[1], parameter->Name()) == 0) {
					value = atof(argv[2]);
					printf("  setting: %s\n", strerror(parameter->SetValue(
						&value, sizeof(value), system_time())));
				}
				status = parameter->GetValue(&value, &size, &when);
				printf("  \"%s\": %g (%g to %g) %s\n", parameter->Name(),
					value, continuous->MinValue(), continuous->MaxValue(),
					status == B_OK ? "" : strerror(status));
			} else if (parameter->Type() == BParameter::B_DISCRETE_PARAMETER) {
				int32 value = 0;
				size_t size = sizeof(value);
				if (argc > 2 && strcmp(argv[1], parameter->Name()) == 0) {
					value = atoi(argv[2]);
					printf("  setting: %s\n", strerror(parameter->SetValue(
						&value, sizeof(value), system_time())));
				}
				status = parameter->GetValue(&value, &size, &when);
				printf("  \"%s\": %d %s\n", parameter->Name(), (int)value,
					status == B_OK ? "" : strerror(status));
			} else if (parameter->Type() == BParameter::B_TEXT_PARAMETER) {
				char text[256] = "";
				size_t size = sizeof(text);
				status = parameter->GetValue(text, &size, &when);
				printf("  \"%s\": \"%s\"\n", parameter->Name(), text);
			}
		}
		delete web;
	}
	roster->ReleaseNode(node);
	return 0;
}
