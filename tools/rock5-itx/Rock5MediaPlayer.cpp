/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * A small Media Kit movie player for Rock 5 ITX decoder qualification.
 */
#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <File.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <MessageRunner.h>
#include <Slider.h>
#include <SoundPlayer.h>
#include <StringView.h>
#include <View.h>
#include <Window.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32 kToggle = 'togp';
constexpr uint32 kSeek = 'seek';
constexpr uint32 kTick = 'tick';

class VideoView : public BView {
public:
	VideoView(BRect frame)
		: BView(frame, "movie", B_FOLLOW_LEFT_TOP, B_WILL_DRAW),
		  fBitmap(BRect(0, 0, frame.Width(), frame.Height()), B_RGB32)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void Draw(BRect) override { DrawBitmap(&fBitmap); }

	void Present(const void* pixels, size_t bytesPerRow)
	{
		const uint8* source = static_cast<const uint8*>(pixels);
		uint8* target = static_cast<uint8*>(fBitmap.Bits());
		size_t rowBytes = std::min(bytesPerRow, (size_t)fBitmap.BytesPerRow());
		for (int row = 0; row < fBitmap.Bounds().IntegerHeight() + 1; row++)
			memcpy(target + row * fBitmap.BytesPerRow(), source + row * bytesPerRow,
				rowBytes);
		Invalidate();
	}

private:
	BBitmap fBitmap;
};

class SeekSlider : public BSlider {
public:
	using BSlider::BSlider;
	bool Tracking() const { return IsTracking(); }
};

class PlayerWindow : public BWindow {
public:
	PlayerWindow(const char* path)
		: BWindow(BRect(80, 80, 780, 580), "Rock 5 media player",
			B_DOCUMENT_WINDOW, B_QUIT_ON_WINDOW_CLOSE),
		  fPath(path), fFile(path, B_READ_ONLY), fMovie(&fFile)
	{
		if (fFile.InitCheck() != B_OK || fMovie.InitCheck() != B_OK) {
			fprintf(stderr, "Cannot open movie: %s\n", path);
			return;
		}
		for (int32 index = 0; index < fMovie.CountTracks(); index++) {
			BMediaTrack* track = fMovie.TrackAt(index);
			media_format encoded = {};
			if (track->EncodedFormat(&encoded) != B_OK)
				continue;
			if (encoded.IsVideo() && fVideo == nullptr)
				fVideo = track;
			else if (encoded.IsAudio() && fAudio == nullptr)
				fAudio = track;
		}
		if (fVideo == nullptr) {
			fprintf(stderr, "Movie has no video track\n");
			return;
		}
		fVideoFormat.type = B_MEDIA_RAW_VIDEO;
		fVideoFormat.u.raw_video.display.format = B_RGB32;
		if (fVideo->DecodedFormat(&fVideoFormat) != B_OK) {
			fprintf(stderr, "Cannot decode video track\n");
			return;
		}
		int width = fVideoFormat.Width();
		int height = fVideoFormat.Height();
		if (width < 1 || height < 1 || width > 4096 || height > 2160)
			return;
		fVideoBytes = (size_t)fVideoFormat.u.raw_video.display.bytes_per_row * height;
		fFrame.resize(fVideoBytes);
		fDuration = fVideo->Duration();

		if (fAudio != nullptr) {
			fAudioFormat.type = B_MEDIA_RAW_AUDIO;
			fAudioFormat.u.raw_audio = media_raw_audio_format::wildcard;
			fAudioFormat.u.raw_audio.format = media_raw_audio_format::B_AUDIO_SHORT;
			fAudioFormat.u.raw_audio.channel_count = 2;
			fAudioFormat.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
			if (fAudio->DecodedFormat(&fAudioFormat) == B_OK) {
				fRate = fAudioFormat.u.raw_audio.frame_rate;
				fDuration = std::max(fDuration, fAudio->Duration());
				fAudioBuffer.resize(std::max((size_t)8192,
					fAudioFormat.u.raw_audio.buffer_size));
				fRing.resize((size_t)fRate * 2);
				media_raw_audio_format output = fAudioFormat.u.raw_audio;
				output.buffer_size = 4096;
				fSound = new BSoundPlayer(&output, "Rock 5 movie",
					PlayAudio, nullptr, this);
				if (fSound->InitCheck() != B_OK) {
					fprintf(stderr, "Audio output unavailable: %d\n",
						fSound->InitCheck());
					delete fSound;
					fSound = nullptr;
				}
			}
		}

		ResizeTo(width - 1, height + 79);
		fView = new VideoView(BRect(0, 0, width - 1, height - 1));
		AddChild(fView);
		fButton = new BButton(BRect(8, height + 7, 95, height + 36),
			"toggle", "Pause", new BMessage(kToggle));
		AddChild(fButton);
		fSlider = new SeekSlider(BRect(102, height + 4, width - 8, height + 37),
			"seek", "Seek", new BMessage(kSeek), 0, 1000);
		AddChild(fSlider);
		fStatus = new BStringView(BRect(10, height + 44, width - 10, height + 72),
			"status", fSound != nullptr ? "Audio ready" : "Audio output unavailable");
		AddChild(fStatus);
		fTicker = new BMessageRunner(BMessenger(this), new BMessage(kTick), 100000);
		fReady = true;
		fRunning = true;
		fPlaying = true;
		fWallOrigin = system_time();
		if (fSound != nullptr) {
			fSound->SetHasData(true);
			fSound->Start();
		}
		fWorker = std::thread([this] { DecodeLoop(); });
	}

	~PlayerWindow() override
	{
		fRunning = false;
		if (fWorker.joinable())
			fWorker.join();
		delete fTicker;
		if (fSound != nullptr) {
			fSound->Stop();
			delete fSound;
		}
	}

	bool Ready() const { return fReady; }

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case kToggle:
				fPlaying = !fPlaying;
				if (fPlaying)
					fWallOrigin = system_time() - fPosition;
				fButton->SetLabel(fPlaying ? "Pause" : "Play");
				break;
			case kSeek:
				fSeekRequest = fDuration * fSlider->Value() / 1000;
				break;
			case kTick: {
				fButton->SetLabel(fPlaying ? "Pause" : "Play");
				if (!fSlider->Tracking() && fSeekRequest < 0 && fDuration > 0)
					fSlider->SetValue((int32)(fPosition * 1000 / fDuration));
				char label[120];
				int seconds = fPosition / 1000000;
				snprintf(label, sizeof(label), "%02d:%02d / %02d:%02d   %s   %s",
					seconds / 60, seconds % 60,
					(int)(fDuration / 60000000), (int)(fDuration / 1000000 % 60),
					fPlaying ? "Playing" : "Paused",
					fSound != nullptr ? "audio" : "no audio output");
				fStatus->SetText(label);
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	bool ReopenTracks()
	{
		// Some reader/decoder combinations report a successful seek after EOF
		// but continue returning B_LAST_BUFFER_ERROR. A new extractor avoids
		// keeping that exhausted FFmpeg context alive across seeks.
		fSeekMovie.reset();
		fSeekFile.reset();
		fSeekFile.reset(new BFile(fPath.c_str(), B_READ_ONLY));
		if (fSeekFile->InitCheck() != B_OK)
			return false;
		fSeekMovie.reset(new BMediaFile(fSeekFile.get()));
		if (fSeekMovie->InitCheck() != B_OK)
			return false;
		fVideo = nullptr;
		fAudio = nullptr;
		for (int32 index = 0; index < fSeekMovie->CountTracks(); index++) {
			BMediaTrack* track = fSeekMovie->TrackAt(index);
			media_format encoded = {};
			if (track->EncodedFormat(&encoded) != B_OK)
				continue;
			if (encoded.IsVideo() && fVideo == nullptr)
				fVideo = track;
			else if (encoded.IsAudio() && fAudio == nullptr)
				fAudio = track;
		}
		if (fVideo == nullptr || (fSound != nullptr && fAudio == nullptr))
			return false;
		media_format videoFormat = {};
		videoFormat.type = B_MEDIA_RAW_VIDEO;
		videoFormat.u.raw_video.display.format = B_RGB32;
		if (fVideo->DecodedFormat(&videoFormat) != B_OK
			|| videoFormat.Width() != fVideoFormat.Width()
			|| videoFormat.Height() != fVideoFormat.Height())
			return false;
		media_format audioFormat = {};
		if (fSound != nullptr) {
			audioFormat.type = B_MEDIA_RAW_AUDIO;
			audioFormat.u.raw_audio = media_raw_audio_format::wildcard;
			audioFormat.u.raw_audio.format = media_raw_audio_format::B_AUDIO_SHORT;
			audioFormat.u.raw_audio.channel_count = 2;
			audioFormat.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
			if (fAudio->DecodedFormat(&audioFormat) != B_OK
				|| audioFormat.u.raw_audio.frame_rate
					!= fAudioFormat.u.raw_audio.frame_rate)
				return false;
		}
		fVideoFormat = videoFormat;
		if (fSound != nullptr)
			fAudioFormat = audioFormat;
		fprintf(stderr, "reopened tracks video_rate=%.3f audio_rate=%.0f\n",
			fVideoFormat.u.raw_video.field_rate,
			fAudioFormat.u.raw_audio.frame_rate);
		return true;
	}

	static void PlayAudio(void* cookie, void* buffer, size_t bytes,
		const media_raw_audio_format&)
	{
		PlayerWindow* self = static_cast<PlayerWindow*>(cookie);
		int16* output = static_cast<int16*>(buffer);
		size_t frames = bytes / (sizeof(int16) * 2);
		memset(output, 0, bytes);
		if (!self->fPlaying || self->fPriming)
			return;
		std::lock_guard<std::mutex> guard(self->fRingLock);
		for (size_t i = 0; i < frames && self->fRingCount > 0; i++) {
			output[i * 2] = self->fRing[self->fRingRead * 2];
			output[i * 2 + 1] = self->fRing[self->fRingRead * 2 + 1];
			self->fRingRead = (self->fRingRead + 1) % (self->fRing.size() / 2);
			self->fRingCount--;
		}
		self->fConsumed += frames;
	}

	void DecodeLoop()
	{
		bool pendingVideo = false;
		bool videoEnd = false;
		bool audioEnd = false;
		bigtime_t videoTime = 0;
		bigtime_t nextVideoTime = 0;
		bigtime_t discardBefore = 0;
		int64 audioSkipFrames = 0;
		bool priming = false;
		bigtime_t primeTarget = 0;
		float frameRate = fVideoFormat.u.raw_video.field_rate;
		bigtime_t frameDuration = frameRate > 0
			? (bigtime_t)(1000000.0f / frameRate + 0.5f) : 33333;
		while (fRunning) {
			bigtime_t request = fSeekRequest.exchange(-1);
			if (request >= 0) {
				if (!ReopenTracks()) {
					fprintf(stderr, "Cannot reopen media tracks for seek\n");
					fPlaying = false;
					continue;
				}
				// Media Kit's FFmpeg reader can claim a successful seek at EOF
				// while returning no subsequent packets. Decode this short sample
				// from a fresh extractor and discard its preroll instead.
				nextVideoTime = 0;
				discardBefore = request;
				audioSkipFrames = fSound != nullptr
					? (int64)request * fRate / 1000000 : 0;
				priming = true;
				primeTarget = request;
				fPriming = true;
				fprintf(stderr, "seek requested=%lld mode=decode-preroll\n",
					(long long)request);
				{
					std::lock_guard<std::mutex> guard(fRingLock);
					fRingRead = fRingWrite = fRingCount = 0;
					fConsumed = 0;
					fClockBase = request;
				}
				fWallOrigin = system_time() - request;
				fPosition = request;
				pendingVideo = videoEnd = audioEnd = false;
			}
		if (!fPlaying && !priming) {
			snooze(10000);
			continue;
		}
			if (fSound != nullptr && !audioEnd) {
				size_t buffered;
				{
					std::lock_guard<std::mutex> guard(fRingLock);
					buffered = fRingCount;
				}
				if (buffered < fRing.size() / 4) {
					int64 frames = 0;
					media_header header = {};
					status_t status = fAudio->ReadFrames(fAudioBuffer.data(),
						&frames, &header);
					if (status != B_OK || frames <= 0) {
						fprintf(stderr, "audio end status=%d skip=%lld now=%lld\n",
							status, (long long)audioSkipFrames,
							(long long)(system_time() - fWallOrigin));
						audioEnd = true;
					}
					else {
						const int16* samples =
							reinterpret_cast<const int16*>(fAudioBuffer.data());
						int64 skip = std::min(frames, audioSkipFrames);
						samples += skip * 2;
						frames -= skip;
						audioSkipFrames -= skip;
						std::lock_guard<std::mutex> guard(fRingLock);
						for (int64 i = 0; i < frames && fRingCount < fRing.size() / 2; i++) {
							fRing[fRingWrite * 2] = samples[i * 2];
							fRing[fRingWrite * 2 + 1] = samples[i * 2 + 1];
							fRingWrite = (fRingWrite + 1) % (fRing.size() / 2);
							fRingCount++;
						}
					}
				}
			}
			bigtime_t now = priming ? primeTarget : system_time() - fWallOrigin;
			fPosition = std::min(now, fDuration);
			if (!pendingVideo && !videoEnd) {
				int64 frames = 1;
				media_header header = {};
				status_t status = fVideo->ReadFrames(fFrame.data(), &frames, &header);
				if (status != B_OK || frames <= 0) {
					fprintf(stderr, "video end status=%d next=%lld now=%lld\n",
						status, (long long)nextVideoTime, (long long)now);
					videoEnd = true;
				}
				else {
					// The current FFmpeg add-on restarts decoded-frame timestamps
					// near zero after a seek. This sample is fixed frame rate, so
					// pace the output from the negotiated rate and seek origin.
					videoTime = nextVideoTime;
					nextVideoTime += frameDuration;
					pendingVideo = true;
				}
			}
			if (pendingVideo && videoTime + 15000 < discardBefore)
				pendingVideo = false;
			if (pendingVideo && videoTime <= now + 15000) {
				if (Lock()) {
					fView->Present(fFrame.data(),
						fVideoFormat.u.raw_video.display.bytes_per_row);
					Unlock();
				}
				pendingVideo = false;
			}
		if (priming && nextVideoTime >= discardBefore
			&& audioSkipFrames == 0) {
			fWallOrigin = system_time() - primeTarget;
			priming = false;
			fPriming = false;
			fprintf(stderr, "seek ready target=%lld decoded=%lld playing=%d\n",
				(long long)primeTarget, (long long)nextVideoTime,
				(int)fPlaying.load());
		}
			if (!priming && videoEnd && (!pendingVideo)
				&& (audioEnd || fSound == nullptr)
				&& now >= fDuration)
				fPlaying = false;
			if (!priming)
				snooze(3000);
		}
	}

	std::string fPath;
	BFile fFile;
	BMediaFile fMovie;
	std::unique_ptr<BFile> fSeekFile;
	std::unique_ptr<BMediaFile> fSeekMovie;
	BMediaTrack* fVideo = nullptr;
	BMediaTrack* fAudio = nullptr;
	media_format fVideoFormat = {};
	media_format fAudioFormat = {};
	VideoView* fView = nullptr;
	BButton* fButton = nullptr;
	SeekSlider* fSlider = nullptr;
	BStringView* fStatus = nullptr;
	BMessageRunner* fTicker = nullptr;
	BSoundPlayer* fSound = nullptr;
	std::thread fWorker;
	std::vector<uint8> fFrame;
	std::vector<uint8> fAudioBuffer;
	std::vector<int16> fRing;
	std::mutex fRingLock;
	size_t fVideoBytes = 0;
	size_t fRingRead = 0;
	size_t fRingWrite = 0;
	size_t fRingCount = 0;
	std::atomic<int64> fConsumed{0};
	std::atomic<bigtime_t> fClockBase{0};
	std::atomic<bigtime_t> fWallOrigin{0};
	std::atomic<bigtime_t> fPosition{0};
	std::atomic<bigtime_t> fSeekRequest{-1};
	std::atomic<bool> fPlaying{false};
	std::atomic<bool> fPriming{false};
	std::atomic<bool> fRunning{false};
	bigtime_t fDuration = 0;
	int fRate = 0;
	bool fReady = false;
};

} // namespace

int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s movie.mp4\n", argv[0]);
		return 2;
	}
	BApplication app("application/x-vnd.jmgasper-Rock5MediaPlayer");
	PlayerWindow window(argv[1]);
	if (!window.Ready())
		return 1;
	window.Show();
	app.Run();
	return 0;
}
