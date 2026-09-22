/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 * Media Kit smoke test for the Rock 5 ITX movie sample.
 */
#include <File.h>
#include <MediaFile.h>
#include <MediaTrack.h>

#include <stdio.h>
#include <vector>

int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s movie.mp4\n", argv[0]);
		return 2;
	}
	BFile file(argv[1], B_READ_ONLY);
	if (file.InitCheck() != B_OK) {
		fprintf(stderr, "file open failed: %d\n", file.InitCheck());
		return 1;
	}
	BMediaFile movie(&file);
	printf("media init=%d tracks=%d\n", movie.InitCheck(), movie.CountTracks());
	if (movie.InitCheck() != B_OK || movie.CountTracks() < 2)
		return 1;

	int decodedTracks = 0;
	int seekedTracks = 0;
	for (int index = 0; index < movie.CountTracks(); index++) {
		BMediaTrack* track = movie.TrackAt(index);
		media_format encoded = {};
		status_t status = track->EncodedFormat(&encoded);
		printf("track %d encoded=%d type=%d duration=%lld\n", index,
			status, encoded.type, static_cast<long long>(track->Duration()));
		if (status != B_OK)
			continue;
		media_format decoded = {};
		decoded.type = encoded.IsVideo() ? B_MEDIA_RAW_VIDEO : B_MEDIA_RAW_AUDIO;
		if (encoded.IsVideo())
			decoded.u.raw_video.display.format = B_RGB32;
		status = track->DecodedFormat(&decoded);
		printf("track %d decoded=%d type=%d", index, status, decoded.type);
		if (status != B_OK) {
			puts("");
			continue;
		}
		if (encoded.IsVideo())
			printf(" %ux%u bytes_per_row=%u", decoded.Width(),
				decoded.Height(), decoded.u.raw_video.display.bytes_per_row);
		else
			printf(" channels=%u rate=%.0f bytes=%zu",
				decoded.u.raw_audio.channel_count, decoded.u.raw_audio.frame_rate,
				decoded.u.raw_audio.buffer_size);
		puts("");
		size_t bytes = encoded.IsVideo()
			? (size_t)decoded.u.raw_video.display.bytes_per_row * decoded.Height()
			: decoded.u.raw_audio.buffer_size;
		if (bytes == 0 || bytes > 64 * 1024 * 1024) {
			fprintf(stderr, "invalid output buffer size: %zu\n", bytes);
			continue;
		}
		std::vector<unsigned char> buffer(bytes);
		int64 frames = 1;
		media_header header = {};
		status = track->ReadFrames(buffer.data(), &frames, &header);
		printf("track %d first read=%d frames=%lld start=%lld\n", index,
			status, static_cast<long long>(frames),
			static_cast<long long>(header.start_time));
		if (status == B_OK && frames > 0)
			decodedTracks++;
		bigtime_t seek = 10000000;
		status = track->SeekToTime(&seek);
		printf("track %d seek=%d actual=%lld\n", index, status,
			static_cast<long long>(seek));
		if (status == B_OK) {
			frames = 1;
			header = {};
			status = track->ReadFrames(buffer.data(), &frames, &header);
			printf("track %d seek read=%d frames=%lld start=%lld\n", index,
				status, static_cast<long long>(frames),
				static_cast<long long>(header.start_time));
			if (status == B_OK && frames > 0)
				seekedTracks++;
		}
	}
	return decodedTracks == 2 && seekedTracks == 2 ? 0 : 1;
}
