/* Decode one film on several threads at once through Haiku's media kit, the
 * way a browser process playing more than one video does, and check every
 * thread saw the same pictures.
 *
 * Each thread opens the file itself and decodes the same pictures, so with a
 * decoder that keeps its state to itself they all end with the same checksum.
 * A decoder with state shared between instances gives different checksums,
 * or crashes: the NVDEC add-on's NAL table was once shared like that
 * (fixed in 1323d41adc).
 *
 * Usage: mediadecodemt <file> [threads] [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Entry.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <OS.h>

struct Job {
	const char*	path;
	int		frames;
	int		index;
	int64		decoded;
	uint64		checksum;
	char		codec[64];
	status_t	status;
};

static int32
decode(void* data)
{
	Job* job = (Job*)data;
	job->status = B_ERROR;

	entry_ref ref;
	if (get_ref_for_path(job->path, &ref) != B_OK)
		return 1;
	BMediaFile file(&ref);
	if (file.InitCheck() != B_OK)
		return 1;
	BMediaTrack* video = NULL;
	for (int i = 0; i < file.CountTracks(); i++) {
		BMediaTrack* track = file.TrackAt(i);
		media_format format;
		if (track->EncodedFormat(&format) == B_OK
			&& format.type == B_MEDIA_ENCODED_VIDEO) {
			video = track;
			break;
		}
		file.ReleaseTrack(track);
	}
	if (video == NULL)
		return 1;

	media_format format;
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video.display.format = B_RGB32;
	if (video->DecodedFormat(&format) != B_OK)
		return 1;
	media_codec_info codec;
	if (video->GetCodecInfo(&codec) == B_OK)
		strlcpy(job->codec, codec.short_name, sizeof(job->codec));

	size_t size = (size_t)format.u.raw_video.display.bytes_per_row
		* format.u.raw_video.display.line_count;
	uint8* buffer = (uint8*)malloc(size);
	if (buffer == NULL)
		return 1;

	/* FNV-1a over every picture, in order. */
	uint64 hash = 1469598103934665603ULL;
	while (job->decoded < job->frames) {
		int64 count = 0;
		media_header header;
		if (video->ReadFrames(buffer, &count, &header) != B_OK)
			break;
		for (size_t i = 0; i < size; i += 64)
			hash = (hash ^ buffer[i]) * 1099511628211ULL;
		job->decoded += count > 0 ? count : 1;
	}
	job->checksum = hash;
	job->status = B_OK;
	free(buffer);
	return 0;
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		printf("usage: %s <file> [threads] [frames]\n", argv[0]);
		return 2;
	}
	int threads = argc > 2 ? atoi(argv[2]) : 2;
	int frames = argc > 3 ? atoi(argv[3]) : 200;
	if (threads < 1 || threads > 16)
		threads = 2;

	Job jobs[16];
	thread_id ids[16];
	for (int i = 0; i < threads; i++) {
		memset(&jobs[i], 0, sizeof(Job));
		jobs[i].path = argv[1];
		jobs[i].frames = frames;
		jobs[i].index = i;
		ids[i] = spawn_thread(decode, "video decoder", B_NORMAL_PRIORITY,
			&jobs[i]);
		resume_thread(ids[i]);
	}
	int failed = 0;
	for (int i = 0; i < threads; i++) {
		status_t result;
		wait_for_thread(ids[i], &result);
		printf("thread %d: %s, %lld pictures, checksum %016llx\n", i,
			jobs[i].codec, jobs[i].decoded,
			(unsigned long long)jobs[i].checksum);
		if (jobs[i].status != B_OK || jobs[i].checksum != jobs[0].checksum
			|| jobs[i].decoded != jobs[0].decoded)
			failed++;
	}
	printf("%s\n", failed == 0 ? "all threads agree" : "THREADS DISAGREE");
	return failed == 0 ? 0 : 1;
}
