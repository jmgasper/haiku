/*
 * Takes pictures from the default video input (a USB camera) for a while,
 * says how many arrived and saves the last one.
 *
 *	camgrab [width height [seconds [picture.ppm]]]
 *
 * g++ -O2 -o camgrab camgrab.cpp -lbe -lmedia
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Buffer.h>
#include <BufferConsumer.h>
#include <MediaEventLooper.h>
#include <MediaRoster.h>
#include <OS.h>
#include <TimeSource.h>


class Grabber : public BMediaEventLooper, public BBufferConsumer {
public:
	Grabber()
		:
		BMediaNode("camgrab"),
		BMediaEventLooper(),
		BBufferConsumer(B_MEDIA_RAW_VIDEO),
		fFrames(0),
		fFirst(0),
		fLast(0),
		fLongestGap(0),
		fPicture(NULL),
		fPictureSize(0)
	{
		fInput.destination.port = -1;
	}

	virtual ~Grabber()
	{
		Quit();
		free(fPicture);
	}

	virtual BMediaAddOn* AddOn(int32* id) const { return NULL; }

	virtual void NodeRegistered()
	{
		fInput.destination.port = ControlPort();
		fInput.destination.id = 0;
		fInput.source = media_source::null;
		fInput.node = Node();
		fInput.format.type = B_MEDIA_RAW_VIDEO;
		fInput.format.u.raw_video = media_raw_video_format::wildcard;
		strcpy(fInput.name, "camgrab input");
		Run();
	}

	virtual status_t AcceptFormat(const media_destination& destination,
		media_format* format)
	{
		if (format->type != B_MEDIA_RAW_VIDEO)
			return B_MEDIA_BAD_FORMAT;
		return B_OK;
	}

	virtual status_t GetNextInput(int32* cookie, media_input* _input)
	{
		if (*cookie != 0)
			return B_BAD_INDEX;
		*_input = fInput;
		(*cookie)++;
		return B_OK;
	}

	virtual void DisposeInputCookie(int32 cookie) {}

	virtual void BufferReceived(BBuffer* buffer)
	{
		bigtime_t now = system_time();
		if (fFrames == 0)
			fFirst = now;
		else if (now - fLast > fLongestGap)
			fLongestGap = now - fLast;
		fLast = now;
		fFrames++;

		size_t size = buffer->SizeUsed();
		if (size > fPictureSize) {
			fPicture = (uint8*)realloc(fPicture, size);
			fPictureSize = size;
		}
		if (fPicture != NULL)
			memcpy(fPicture, buffer->Data(), size);
		buffer->Recycle();
	}

	virtual void ProducerDataStatus(const media_destination& destination,
		int32 status, bigtime_t when) {}

	virtual status_t GetLatencyFor(const media_destination& destination,
		bigtime_t* _latency, media_node_id* _timeSource)
	{
		*_latency = 10000;
		*_timeSource = TimeSource()->ID();
		return B_OK;
	}

	virtual status_t Connected(const media_source& source,
		const media_destination& destination, const media_format& format,
		media_input* _input)
	{
		fInput.source = source;
		fInput.format = format;
		*_input = fInput;
		return B_OK;
	}

	virtual void Disconnected(const media_source& source,
		const media_destination& destination)
	{
		fInput.source = media_source::null;
	}

	virtual status_t FormatChanged(const media_source& source,
		const media_destination& destination, int32 tag,
		const media_format& format)
	{
		return B_ERROR;
	}

	virtual void HandleEvent(const media_timed_event* event,
		bigtime_t lateness, bool realTimeEvent) {}

	virtual status_t HandleMessage(int32 message, const void* data,
		size_t size)
	{
		return B_ERROR;
	}

	int32		fFrames;
	bigtime_t	fFirst;
	bigtime_t	fLast;
	bigtime_t	fLongestGap;
	uint8*		fPicture;
	size_t		fPictureSize;
	media_input	fInput;
};


int
main(int argc, char** argv)
{
	uint32 width = argc > 2 ? atoi(argv[1]) : 320;
	uint32 height = argc > 2 ? atoi(argv[2]) : 240;
	int seconds = argc > 3 ? atoi(argv[3]) : 5;
	const char* path = argc > 4 ? argv[4] : NULL;

	BApplication app("application/x-vnd.airOS-camgrab");
	status_t status;
	BMediaRoster* roster = BMediaRoster::Roster(&status);
	if (roster == NULL) {
		printf("no media roster: %s\n", strerror(status));
		return 1;
	}

	media_node producer;
	status = roster->GetVideoInput(&producer);
	if (status != B_OK) {
		printf("no video input: %s\n", strerror(status));
		return 1;
	}

	media_node timeSource;
	roster->GetTimeSource(&timeSource);

	Grabber* grabber = new Grabber();
	roster->RegisterNode(grabber);

	media_output output;
	media_input input;
	int32 count = 0;
	status = roster->GetFreeOutputsFor(producer, &output, 1, &count,
		B_MEDIA_RAW_VIDEO);
	if (status != B_OK || count < 1) {
		printf("the video input has no free output (in use?): %s\n",
			strerror(status));
		return 1;
	}
	count = 0;
	roster->GetFreeInputsFor(grabber->Node(), &input, 1, &count,
		B_MEDIA_RAW_VIDEO);

	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = media_raw_video_format::wildcard;
	format.u.raw_video.display.format = B_RGB32;
	format.u.raw_video.display.line_width = width;
	format.u.raw_video.display.line_count = height;

	status = roster->Connect(output.source, input.destination, &format,
		&output, &input);
	if (status != B_OK) {
		printf("connecting failed: %s\n", strerror(status));
		return 1;
	}
	printf("connected: %" B_PRIu32 "x%" B_PRIu32 ", %g per second, \"%s\"\n",
		format.u.raw_video.display.line_width,
		format.u.raw_video.display.line_count, format.u.raw_video.field_rate,
		output.name);

	roster->SetTimeSourceFor(producer.node, timeSource.node);
	roster->SetTimeSourceFor(grabber->ID(), timeSource.node);

	BTimeSource* source = roster->MakeTimeSourceFor(producer);
	bigtime_t start = source->Now() + 50000;
	source->Release();
	bigtime_t asked = system_time();
	roster->StartNode(grabber->Node(), start);
	roster->StartNode(producer, start);

	snooze(seconds * 1000000LL);

	int32 frames = grabber->fFrames;
	printf("%d frames in %d s; first after %.2f s, then %.2f per second, "
		"longest gap %.0f ms\n", (int)frames, seconds,
		frames > 0 ? (grabber->fFirst - asked) / 1e6 : 0.0,
		frames > 1 ? (frames - 1) * 1e6 / (grabber->fLast - grabber->fFirst)
			: 0.0,
		grabber->fLongestGap / 1e3);

	roster->StopNode(producer, 0, true);
	roster->StopNode(grabber->Node(), 0, true);
	roster->Disconnect(producer.node, output.source, grabber->ID(),
		input.destination);

	if (path != NULL && grabber->fPicture != NULL) {
		width = format.u.raw_video.display.line_width;
		height = format.u.raw_video.display.line_count;
		FILE* file = fopen(path, "wb");
		if (file != NULL) {
			fprintf(file, "P6\n%u %u\n255\n", (unsigned)width,
				(unsigned)height);
			for (uint32 i = 0; i < width * height; i++) {
				const uint8* pixel = grabber->fPicture + i * 4;
				fputc(pixel[2], file);
				fputc(pixel[1], file);
				fputc(pixel[0], file);
			}
			fclose(file);
			printf("saved %s\n", path);
		}
	}

	roster->ReleaseNode(producer);
	roster->UnregisterNode(grabber);
	grabber->Release();
	return frames > 0 ? 0 : 1;
}
