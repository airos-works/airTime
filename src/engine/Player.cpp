/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Player.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Directory.h>
#include <Entry.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>

#include "Bitstream.h"
#include "Languages.h"


namespace airtime {

static const double kMaxContinuousRate = 3.0;
static const double kMinContinuousRate = 0.25;
static const int64 kMaxQueuedBytes = 96 * 1024 * 1024;
static const bigtime_t kAudioStartWait = 400000;
static const bigtime_t kScanStepInterval = 90000;

static bool sTrace = getenv("AIRTIME_TRACE") != NULL;

#define TRACE(...) do { if (sTrace) fprintf(stderr, __VA_ARGS__); } while (0)


static TrackInfo
make_track_info(AVStream* stream)
{
	TrackInfo info;
	AVCodecParameters* parameters = stream->codecpar;
	info.stream = stream->index;
	info.codec = codec_display_name(parameters->codec_id);
	info.bitRate = parameters->bit_rate;

	AVDictionaryEntry* tag = av_dict_get(stream->metadata, "language", NULL, 0);
	if (tag != NULL)
		info.language = tag->value;
	tag = av_dict_get(stream->metadata, "title", NULL, 0);
	if (tag == NULL)
		tag = av_dict_get(stream->metadata, "handler_name", NULL, 0);
	if (tag != NULL) {
		info.title = tag->value;
		// Container defaults that say nothing ("SoundHandler", "GPAC ISO
		// Audio Handler", "Core Media Audio", "Mainconcept MP4 Sound Media
		// Handler").
		if (info.title.IFindFirst("handler") >= 0
			|| info.title.StartsWith("Core Media")
			|| info.title.IFindFirst("produced by") >= 0)
			info.title = "";
	}

	int disposition = stream->disposition;
	info.isDefault = (disposition & AV_DISPOSITION_DEFAULT) != 0;
	info.isForced = (disposition & AV_DISPOSITION_FORCED) != 0;
	info.isHearingImpaired = (disposition & AV_DISPOSITION_HEARING_IMPAIRED) != 0;
	info.isCommentary = (disposition & AV_DISPOSITION_COMMENT) != 0;
	info.isDescription = (disposition & (AV_DISPOSITION_VISUAL_IMPAIRED
		| AV_DISPOSITION_DESCRIPTIONS)) != 0;

	switch (parameters->codec_type) {
		case AVMEDIA_TYPE_VIDEO:
		{
			info.kind = TRACK_VIDEO;
			info.width = parameters->width;
			info.height = parameters->height;
			AVRational rate = av_guess_frame_rate(NULL, stream, NULL);
			if (rate.num > 0 && rate.den > 0)
				info.frameRate = av_q2d(rate);
			break;
		}
		case AVMEDIA_TYPE_AUDIO:
			info.kind = TRACK_AUDIO;
			info.channels = parameters->ch_layout.nb_channels;
			info.sampleRate = parameters->sample_rate;
			break;
		default:
		{
			info.kind = TRACK_SUBTITLE;
			const AVCodecDescriptor* descriptor
				= avcodec_descriptor_get(parameters->codec_id);
			info.isBitmap = descriptor != NULL
				&& (descriptor->props & AV_CODEC_PROP_BITMAP_SUB) != 0;
			info.isCaptions = parameters->codec_id == AV_CODEC_ID_EIA_608;
			break;
		}
	}
	return info;
}


Player::Player(const BMessenger& target)
	:
	fTarget(target),
	fFormat(NULL),
	fStartTime(0),
	fDuration(0),
	fFileSize(0),
	fVideoStream(-1),
	fAudioStream(-1),
	fSubtitleSelected(-1),
	fVideoWidth(0),
	fVideoHeight(0),
	fSampleAspect(AVRational{1, 1}),
	fCaptionSource(CAPTIONS_H264),
	fCaptionLengthSize(0),
	fCaptionStream(-1),
	fCaptionDecoder(NULL),
	fCaptionsFound(false),
	fCaptionsEnabled(false),
	fVideoDecoder(NULL),
	fHardwareDecoding(true),
	fAudioDecoder(NULL),
	fAudioOutput(NULL),
	fQuit(false),
	fPrerollTarget(0),
	fPrerollRequested(false),
	fPlaying(false),
	fRate(1.0),
	fScanning(false),
	fLooping(false),
	fSeekRequested(false),
	fSeekTarget(0),
	fSeekAccurate(true),
	fSeekDirection(0),
	fPendingPosition(0),
	fPositionPending(false),
	fSeekFromVideoSerial(-1),
	fSeekFromAudioSerial(-1),
	fShowNextFrame(false),
	fEndOfFile(false),
	fEnded(false),
	fVideoSkipSerial(-1),
	fVideoSkipTime(0),
	fVideoFinishedSerial(-1),
	fLastShownPts(0),
	fSerialStarted(0),
	fScanPosition(0),
	fScanStepTime(0),
	fScanAwaitingFrame(false),
	fScanShownPts(kNoTime),
	fScanPacketSent(false),
	fScanHitEnd(false),
	fVolume(1.0f),
	fMuted(false),
	fSink(NULL),
	fFramesShown(0),
	fFramesDropped(0),
	fRateWindowStart(0),
	fRateWindowFrames(0),
	fDisplayRate(0),
	fLastAVOffset(0),
	fDecodeTime(0),
	fLateness(0),
	fHurry(0),
	fHurryChanged(0),
	fHurryPatience(3000000),
	fHurryEased(0),
	fDemuxPhase(0),
	fDecodePhase(0),
	fPresentPhase(0),
	fScanSeeks(0)
{
	fCaptionTrack.SetMaxOpenDuration(8000000);
}


Player::~Player()
{
	Close();
}


status_t
Player::Open(const char* path, BString* error)
{
	Close();
	fPath = path;

	int result = avformat_open_input(&fFormat, path, NULL, NULL);
	if (result < 0) {
		char text[128];
		error->SetToFormat("The file could not be opened: %s",
			av_error_string(result, text, sizeof(text)));
		fFormat = NULL;
		return B_ERROR;
	}
	result = avformat_find_stream_info(fFormat, NULL);
	if (result < 0)
		TRACE("airTime: no stream information, going on anyway\n");

	fStartTime = fFormat->start_time != AV_NOPTS_VALUE ? fFormat->start_time : 0;
	fDuration = fFormat->duration != AV_NOPTS_VALUE ? fFormat->duration : 0;
	fFileSize = fFormat->pb != NULL ? avio_size(fFormat->pb) : 0;

	// Look at every stream; keep the ones that can be played.
	int bestVideo = -1;
	int64 bestVideoArea = -1;
	int coverStream = -1;
	for (unsigned i = 0; i < fFormat->nb_streams; i++) {
		AVStream* stream = fFormat->streams[i];
		AVCodecParameters* parameters = stream->codecpar;
		stream->discard = AVDISCARD_ALL;

		if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
			if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
				if (coverStream < 0)
					coverStream = i;
				continue;
			}
			if (avcodec_find_decoder(parameters->codec_id) == NULL)
				continue;
			int64 area = (int64)parameters->width * parameters->height;
			if ((stream->disposition & AV_DISPOSITION_DEFAULT) != 0)
				area += (int64)1 << 40;
			if (area > bestVideoArea) {
				bestVideoArea = area;
				bestVideo = i;
			}
		} else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
			if (avcodec_find_decoder(parameters->codec_id) == NULL)
				continue;
			fAudioTracks.push_back(make_track_info(stream));
		} else if (parameters->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			if (parameters->codec_id == AV_CODEC_ID_EIA_608) {
				if (fCaptionStream < 0) {
					fCaptionStream = i;
					stream->discard = AVDISCARD_DEFAULT;
				}
				continue;
			}
			SubtitleSource source;
			source.info = make_track_info(stream);
			source.track = new SubtitleTrack();
			source.track->SetMaxOpenDuration(source.info.isBitmap
				? 30000000 : 10000000);
			source.decoder = new SubtitleDecoder(parameters, stream->time_base,
				fStartTime, source.track);
			BString reason;
			if (source.decoder->Init(&reason) != B_OK) {
				TRACE("airTime: subtitle stream %u: %s\n", i, reason.String());
				delete source.decoder;
				delete source.track;
				continue;
			}
			stream->discard = AVDISCARD_DEFAULT;
			fSubtitles.push_back(source);
		}
	}

	if (bestVideo >= 0) {
		fVideoStream = bestVideo;
		AVStream* stream = fFormat->streams[bestVideo];
		stream->discard = AVDISCARD_DEFAULT;
		fVideoTrack = make_track_info(stream);
		fVideoWidth = stream->codecpar->width;
		fVideoHeight = stream->codecpar->height;
		fSampleAspect = av_guess_sample_aspect_ratio(fFormat, stream, NULL);
		if (fSampleAspect.num <= 0 || fSampleAspect.den <= 0)
			fSampleAspect = AVRational{1, 1};

		switch (stream->codecpar->codec_id) {
			case AV_CODEC_ID_H264:
				fCaptionSource = CAPTIONS_H264;
				fCaptionLengthSize = nal_length_size(false,
					stream->codecpar->extradata,
					stream->codecpar->extradata_size);
				break;
			case AV_CODEC_ID_HEVC:
				fCaptionSource = CAPTIONS_HEVC;
				fCaptionLengthSize = nal_length_size(true,
					stream->codecpar->extradata,
					stream->codecpar->extradata_size);
				break;
			case AV_CODEC_ID_MPEG2VIDEO:
				fCaptionSource = CAPTIONS_MPEG2;
				break;
			default:
				fCaptionLengthSize = -1;
				break;
		}

		// Decoded pictures take memory: fewer of them when they are large.
		int64 area = (int64)fVideoWidth * fVideoHeight;
		fFrames.SetCapacity(area > 2560 * 1440 ? 4 : area > 1280 * 720 ? 6 : 8);
	} else if (coverStream >= 0) {
		_DecodeCoverArt(fFormat->streams[coverStream]);
	}

	fCaptionDecoder = new CaptionDecoder(&fCaptionTrack);
	if (fCaptionDecoder->Init() != B_OK) {
		delete fCaptionDecoder;
		fCaptionDecoder = NULL;
	}
	if (fCaptionStream >= 0)
		fCaptionsFound = true;

	for (unsigned i = 0; i < fFormat->nb_chapters; i++) {
		AVChapter* chapter = fFormat->chapters[i];
		Chapter entry;
		entry.start = to_micros(chapter->start, chapter->time_base) - fStartTime;
		entry.end = to_micros(chapter->end, chapter->time_base) - fStartTime;
		AVDictionaryEntry* tag = av_dict_get(chapter->metadata, "title", NULL, 0);
		if (tag != NULL)
			entry.title = tag->value;
		else
			entry.title.SetToFormat("Chapter %u", i + 1);
		fChapters.push_back(entry);
	}

	// Sound.
	int audioStream = _ChooseAudioStream();
	if (audioStream >= 0) {
		fAudioOutput = new AudioOutput(&fAudioClock);
		BString name(fPath);
		int32 slash = name.FindLast('/');
		if (slash >= 0)
			name.Remove(0, slash + 1);
		status_t status = fAudioOutput->Init(name.String());
		if (status != B_OK) {
			fAudioError.SetToFormat("No sound: the audio output did not "
				"start (%s).", strerror(status));
			delete fAudioOutput;
			fAudioOutput = NULL;
		} else {
			fAudioStream = audioStream;
			AVStream* stream = fFormat->streams[audioStream];
			fAudioDecoder = new AudioDecoder(stream, &fAudioQueue, fAudioOutput,
				fStartTime);
			BString reason;
			if (fAudioDecoder->Init(&reason) != B_OK) {
				fAudioError.SetToFormat("No sound: %s.", reason.String());
				delete fAudioDecoder;
				fAudioDecoder = NULL;
				fAudioStream = -1;
			} else {
				stream->discard = AVDISCARD_DEFAULT;
				fAudioOutput->SetVolume(fVolume);
				fAudioOutput->SetMuted(fMuted);
			}
		}
	}

	if (fVideoStream < 0 && fAudioStream < 0 && fCoverFrame.get() == NULL) {
		if (fAudioError.Length() > 0)
			*error = fAudioError;
		else
			error->SetTo("airTime found nothing it can play in this file.");
		Close();
		return B_ERROR;
	}

	_ChooseSubtitleTrack();

	fQuit = false;
	fPlaying = false;
	fRate = 1.0;
	fScanning = false;
	fEnded = false;
	fEndOfFile = false;
	fLastVideoPacketPts = kNoTime;
	fShowNextFrame = true;
	fLastShownPts = 0;
	fFramesShown = 0;
	fFramesDropped = 0;
	_StartThreads();
	return B_OK;
}


void
Player::Close()
{
	_StopThreads();

	delete fVideoDecoder;
	fVideoDecoder = NULL;
	delete fAudioDecoder;
	fAudioDecoder = NULL;
	delete fAudioOutput;
	fAudioOutput = NULL;
	delete fCaptionDecoder;
	fCaptionDecoder = NULL;
	for (SubtitleSource& source : fSubtitles) {
		delete source.decoder;
		delete source.track;
	}
	fSubtitles.clear();
	fCaptionTrack.Clear();
	fCaptionsFound = false;
	fAudioTracks.clear();
	fChapters.clear();
	fCoverFrame.reset();
	fFrames.Flush();
	fVideoQueue.Flush();
	fAudioQueue.Flush();
	if (fFormat != NULL)
		avformat_close_input(&fFormat);
	fFormat = NULL;
	fVideoStream = -1;
	fAudioStream = -1;
	fCaptionStream = -1;
	fSubtitleSelected = -1;
	fVideoWidth = fVideoHeight = 0;
	fHardwareNote = "";
	fAudioError = "";
}


void
Player::_StartThreads()
{
	fVideoQueue.Restart();
	fAudioQueue.Restart();
	fFrames.Restart();
	if (fAudioOutput != NULL)
		fAudioOutput->Flush(fAudioQueue.Serial());
	fSerialStarted = system_time();
	_UpdateClocksLocked();

	fDemuxThread = std::thread(&Player::_DemuxLoop, this);
	if (fVideoStream >= 0)
		fVideoThread = std::thread(&Player::_VideoDecodeLoop, this);
	_StartAudioThread();
	fPresentThread = std::thread(&Player::_PresentLoop, this);
	bool embedded = false;
	for (const SubtitleSource& source : fSubtitles) {
		if (source.decoder != NULL)
			embedded = true;
	}
	if (embedded)
		fPrerollThread = std::thread(&Player::_SubtitlePrerollLoop, this);
}


void
Player::_StopThreads()
{
	fQuit = true;
	fVideoQueue.Abort();
	fAudioQueue.Abort();
	fFrames.Abort();
	if (fAudioOutput != NULL)
		fAudioOutput->Abort();
	{
		std::lock_guard<std::mutex> lock(fLock);
		fWakeUp.notify_all();
	}
	if (fDemuxThread.joinable())
		fDemuxThread.join();
	if (fVideoThread.joinable())
		fVideoThread.join();
	if (fAudioThread.joinable())
		fAudioThread.join();
	if (fPresentThread.joinable())
		fPresentThread.join();
	{
		std::lock_guard<std::mutex> lock(fPrerollLock);
		fPrerollWake.notify_all();
	}
	if (fPrerollThread.joinable())
		fPrerollThread.join();
}


void
Player::_StartAudioThread()
{
	if (fAudioDecoder == NULL)
		return;
	fAudioThread = std::thread([this]() {
		rename_thread(find_thread(NULL), "airTime audio decoder");
		set_thread_priority(find_thread(NULL), B_URGENT_DISPLAY_PRIORITY);
		fAudioDecoder->Run();
	});
}


void
Player::_StopAudioThread()
{
	fAudioQueue.Abort();
	if (fAudioOutput != NULL)
		fAudioOutput->Abort();
	if (fAudioThread.joinable())
		fAudioThread.join();
}


// #pragma mark - transport


bool
Player::_IsScanRate(double rate) const
{
	return rate < 0 || rate > kMaxContinuousRate;
}


void
Player::_UpdateClocksLocked()
{
	bool running = fPlaying && !fScanning;
	double speed = fRate > 0 ? fRate : 1.0;
	fExternalClock.SetSpeed(speed);
	fAudioClock.SetPaused(!running);
	fExternalClock.SetPaused(!running);
	if (fAudioOutput != NULL)
		fAudioOutput->SetPaused(!running);
	if (fAudioDecoder != NULL)
		fAudioDecoder->SetSpeed(speed);
	fWakeUp.notify_all();
	fFrames.Wake();
}


void
Player::Play()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (fFormat == NULL)
			return;
		if (fEnded && !fScanning) {
			// Playing from the end starts again from the beginning.
			_RequestSeekLocked(0, true, 0);
		}
		if (fScanning) {
			fScanning = false;
			bigtime_t from = fScanShownPts != kNoTime ? fScanShownPts
				: fScanPosition;
			_RequestSeekLocked(from, true, 0);
		}
		fRate = fRate > 0 && fRate <= kMaxContinuousRate ? fRate : 1.0;
		fPlaying = true;
		fEnded = false;
		_UpdateClocksLocked();
	}
	_Notify(kMsgPlayerStateChanged);
}


void
Player::Pause()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (fScanning) {
			fScanning = false;
			fRate = 1.0;
			bigtime_t at = fScanShownPts != kNoTime ? fScanShownPts
				: fScanPosition;
			_RequestSeekLocked(at, true, 0);
			fShowNextFrame = true;
		}
		fPlaying = false;
		_UpdateClocksLocked();
	}
	_Notify(kMsgPlayerStateChanged);
}


void
Player::TogglePlaying()
{
	if (IsPlaying())
		Pause();
	else
		Play();
}


bool
Player::IsPlaying()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fPlaying;
}


void
Player::SetRate(double rate)
{
	if (rate == 0)
		rate = 1.0;
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (fFormat == NULL)
			return;
		bool scan = _IsScanRate(rate);
		if (scan && !fScanning) {
			fScanPosition = fPositionPending ? fPendingPosition : _MasterClockPosition();
			fScanShownPts = fLastShownPts;
			fScanStepTime = 0;
			fScanAwaitingFrame = false;
			fScanning = true;
			fEnded = false;
		} else if (!scan && fScanning) {
			fScanning = false;
			bigtime_t from = fScanShownPts != kNoTime ? fScanShownPts
				: fScanPosition;
			_RequestSeekLocked(from, true, 0);
			fShowNextFrame = true;
		}
		if (rate < kMinContinuousRate && rate > 0)
			rate = kMinContinuousRate;
		fRate = rate;
		if (scan)
			fPlaying = true;
		_UpdateClocksLocked();
	}
	_Notify(kMsgPlayerStateChanged);
}


double
Player::Rate()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fRate;
}


bool
Player::IsScanning()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fScanning;
}


void
Player::Seek(bigtime_t time, bool accurate)
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (fFormat == NULL)
			return;
		if (fDuration > 0 && time > fDuration)
			time = fDuration;
		if (time < 0)
			time = 0;
		if (fScanning) {
			fScanPosition = time;
			fScanStepTime = 0;
			fScanShownPts = kNoTime;
		}
		_RequestSeekLocked(time, accurate, 0);
		fShowNextFrame = true;
		fEnded = false;
	}
	_Notify(kMsgPlayerStateChanged);
}


void
Player::_RequestSeekLocked(bigtime_t time, bool accurate, int direction)
{
	fSeekRequested = true;
	fSeekTarget = time;
	fSeekAccurate = accurate;
	fSeekDirection = direction;
	fPendingPosition = time;
	fPositionPending = true;
	fSeekRequestedAt = system_time();
	fSeekFromVideoSerial = fVideoQueue.Serial();
	fSeekFromAudioSerial = fAudioQueue.Serial();
	fWakeUp.notify_all();
}


void
Player::StepFrame(int direction)
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (fFormat == NULL || fVideoStream < 0)
			return;
		if (fScanning) {
			fScanning = false;
			fRate = 1.0;
		}
		fPlaying = false;
		_UpdateClocksLocked();

		bigtime_t frameDuration = fVideoTrack.frameRate > 0
			? (bigtime_t)(1000000.0 / fVideoTrack.frameRate) : 40000;
		if (direction > 0) {
			if (fPositionPending) {
				// Still on the way to a seek target; step past it.
				_RequestSeekLocked(fPendingPosition + frameDuration, true, 0);
			}
			fShowNextFrame = true;
		} else {
			bigtime_t from = fPositionPending ? fPendingPosition : fLastShownPts;
			bigtime_t target = from - frameDuration;
			if (target < 0)
				target = 0;
			_RequestSeekLocked(target, true, 0);
			fShowNextFrame = true;
		}
		fEnded = false;
		fWakeUp.notify_all();
	}
	_Notify(kMsgPlayerStateChanged);
}


bigtime_t
Player::_MasterClockPosition()
{
	// Called with fLock held.
	if (fAudioStream >= 0 && fAudioDecoder != NULL && !fScanning) {
		int serial;
		bigtime_t time = fAudioClock.Get(&serial);
		if (serial == fAudioQueue.Serial())
			return time;
	}
	if (fVideoStream >= 0)
		return fLastShownPts;
	int serial;
	bigtime_t time = fExternalClock.Get(&serial);
	return time;
}


bigtime_t
Player::Position()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (fFormat == NULL)
		return 0;
	bigtime_t position;
	if (fScanning)
		position = fScanPosition;
	else if (fPositionPending)
		position = fPendingPosition;
	else if (!fPlaying && fVideoStream >= 0)
		position = fLastShownPts;
	else
		position = _MasterClockPosition();
	if (position < 0)
		position = 0;
	if (fDuration > 0 && position > fDuration)
		position = fDuration;
	return position;
}


bool
Player::AtEnd()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fEnded;
}


void
Player::SetLooping(bool loop)
{
	std::lock_guard<std::mutex> lock(fLock);
	fLooping = loop;
}


// #pragma mark - sound


void
Player::SetVolume(float volume)
{
	std::lock_guard<std::mutex> lock(fLock);
	fVolume = volume;
	if (fAudioOutput != NULL)
		fAudioOutput->SetVolume(volume);
}


void
Player::SetMuted(bool muted)
{
	std::lock_guard<std::mutex> lock(fLock);
	fMuted = muted;
	if (fAudioOutput != NULL)
		fAudioOutput->SetMuted(muted);
}


int
Player::CurrentAudioTrack()
{
	std::lock_guard<std::mutex> lock(fLock);
	for (size_t i = 0; i < fAudioTracks.size(); i++) {
		if (fAudioTracks[i].stream == fAudioStream)
			return (int)i;
	}
	return -1;
}


void
Player::SelectAudioTrack(int index)
{
	if (index < 0 || index >= (int)fAudioTracks.size() || fFormat == NULL)
		return;
	int stream = fAudioTracks[index].stream;
	if (stream == fAudioStream)
		return;

	bigtime_t position = Position();
	_StopAudioThread();

	{
		std::lock_guard<std::mutex> lock(fLock);
		delete fAudioDecoder;
		fAudioDecoder = NULL;
		if (fAudioStream >= 0)
			fFormat->streams[fAudioStream]->discard = AVDISCARD_ALL;
		fAudioStream = -1;

		// The output was aborted along with the thread; a new one is
		// simpler than teaching it to come back.
		delete fAudioOutput;
		fAudioOutput = new AudioOutput(&fAudioClock);
		if (fAudioOutput->Init("airTime") != B_OK) {
			delete fAudioOutput;
			fAudioOutput = NULL;
			fAudioError = "No sound: the audio output did not start.";
		} else {
			AVStream* newStream = fFormat->streams[stream];
			fAudioDecoder = new AudioDecoder(newStream, &fAudioQueue,
				fAudioOutput, fStartTime);
			fAudioDecoder->SetStartsAtBeginning(false);
			BString reason;
			if (fAudioDecoder->Init(&reason) != B_OK) {
				delete fAudioDecoder;
				fAudioDecoder = NULL;
				fAudioError.SetToFormat("That audio track cannot be played: "
					"%s.", reason.String());
			} else {
				fAudioStream = stream;
				newStream->discard = AVDISCARD_DEFAULT;
			}
			fAudioOutput->SetVolume(fVolume);
			fAudioOutput->SetMuted(fMuted);
		}
		fAudioQueue.Restart();
		if (fAudioOutput != NULL)
			fAudioOutput->Flush(fAudioQueue.Serial());
		_UpdateClocksLocked();
		// Read again from here, so the new language starts right away.
		_RequestSeekLocked(position, true, 0);
		fShowNextFrame = true;
	}
	_StartAudioThread();
	_Notify(kMsgPlayerTracksChanged);
}


// #pragma mark - subtitles


std::vector<TrackInfo>
Player::SubtitleTracks()
{
	std::lock_guard<std::mutex> lock(fLock);
	std::vector<TrackInfo> tracks;
	for (const SubtitleSource& source : fSubtitles)
		tracks.push_back(source.info);
	return tracks;
}


int
Player::CurrentSubtitleTrack()
{
	std::lock_guard<std::mutex> lock(fLock);
	return fSubtitleSelected;
}


void
Player::SelectSubtitleTrack(int index)
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (index >= (int)fSubtitles.size())
			return;
		fSubtitleSelected = index < 0 ? -1 : index;
	}
	if (index >= 0)
		_RequestSubtitlePreroll(Position());
	_Notify(kMsgPlayerTracksChanged);
}


status_t
Player::AddSubtitleFile(const char* path, BString* error)
{
	SubtitleSource source;
	source.track = new SubtitleTrack();
	source.decoder = NULL;
	BString codec, language;
	status_t status = load_subtitle_file(path, source.track, &codec, &language,
		error);
	if (status != B_OK) {
		delete source.track;
		return status;
	}
	source.info.kind = TRACK_SUBTITLE;
	source.info.codec = codec;
	source.info.language = language;
	BString name(path);
	int32 slash = name.FindLast('/');
	if (slash >= 0)
		name.Remove(0, slash + 1);
	source.info.title = name;

	{
		std::lock_guard<std::mutex> lock(fLock);
		source.info.external = (int)fSubtitles.size();
		fSubtitles.push_back(source);
		fSubtitleSelected = (int)fSubtitles.size() - 1;
	}
	_Notify(kMsgPlayerTracksChanged);
	return B_OK;
}


bool
Player::HasCaptions()
{
	return fCaptionsFound;
}


void
Player::SetCaptionsEnabled(bool enabled)
{
	fCaptionsEnabled = enabled;
	_Notify(kMsgPlayerTracksChanged);
}


bool
Player::CaptionsEnabled()
{
	return fCaptionsEnabled;
}


std::vector<SubtitleEventPtr>
Player::ActiveSubtitles(bigtime_t time)
{
	std::vector<SubtitleEventPtr> events;
	SubtitleTrack* track = NULL;
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (fSubtitleSelected >= 0 && fSubtitleSelected < (int)fSubtitles.size())
			track = fSubtitles[fSubtitleSelected].track;
	}
	if (track != NULL)
		events = track->Active(time);
	if (fCaptionsEnabled) {
		std::vector<SubtitleEventPtr> captions = fCaptionTrack.Active(time);
		events.insert(events.end(), captions.begin(), captions.end());
	}
	return events;
}


void
Player::SetPreferredLanguages(const char* audio, const char* subtitles)
{
	fPreferredAudio = audio != NULL ? audio : "";
	fPreferredSubtitles = subtitles != NULL ? subtitles : "";
}


int
Player::_ChooseAudioStream()
{
	if (fAudioTracks.empty())
		return -1;
	if (fPreferredAudio.Length() > 0) {
		int match = -1;
		for (const TrackInfo& track : fAudioTracks) {
			if (!same_language(track.language.String(), fPreferredAudio.String())
				|| track.isCommentary) {
				continue;
			}
			if (match < 0 || track.isDefault)
				match = track.stream;
			if (track.isDefault)
				break;
		}
		if (match >= 0)
			return match;
	}
	for (const TrackInfo& track : fAudioTracks) {
		if (track.isDefault)
			return track.stream;
	}
	int best = av_find_best_stream(fFormat, AVMEDIA_TYPE_AUDIO, -1,
		fVideoStream, NULL, 0);
	for (const TrackInfo& track : fAudioTracks) {
		if (track.stream == best)
			return best;
	}
	return fAudioTracks[0].stream;
}


void
Player::_ChooseSubtitleTrack()
{
	fSubtitleSelected = -1;
	if (fSubtitles.empty())
		return;

	if (fPreferredSubtitles.Length() > 0 && fPreferredSubtitles != "off") {
		for (size_t i = 0; i < fSubtitles.size(); i++) {
			const TrackInfo& info = fSubtitles[i].info;
			if (same_language(info.language.String(),
					fPreferredSubtitles.String()) && !info.isForced) {
				fSubtitleSelected = (int)i;
				return;
			}
		}
	}

	// Forced subtitles translate what the film does not (signs, a scene in
	// another language): show them for the language being heard.
	BString audioLanguage;
	for (const TrackInfo& track : fAudioTracks) {
		if (track.stream == fAudioStream)
			audioLanguage = track.language;
	}
	for (size_t i = 0; i < fSubtitles.size(); i++) {
		const TrackInfo& info = fSubtitles[i].info;
		if (info.isForced && (audioLanguage.Length() == 0
				|| same_language(info.language.String(),
					audioLanguage.String()))) {
			fSubtitleSelected = (int)i;
			return;
		}
	}
}


std::vector<BString>
Player::FindSidecarSubtitles(const char* moviePath)
{
	std::vector<BString> found;
	BPath path(moviePath);
	BPath parent;
	if (path.InitCheck() != B_OK || path.GetParent(&parent) != B_OK)
		return found;

	BString base(path.Leaf());
	int32 dot = base.FindLast('.');
	if (dot > 0)
		base.Truncate(dot);

	BDirectory directory(parent.Path());
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		if (entry.GetName(name) != B_OK)
			continue;
		BString leaf(name);
		if (!leaf.StartsWith(base) || leaf.Length() <= base.Length()
			|| leaf[base.Length()] != '.') {
			continue;
		}
		BString extension(leaf);
		int32 extensionDot = extension.FindLast('.');
		extension.Remove(0, extensionDot + 1);
		extension.ToLower();
		if (extension == "srt" || extension == "ass" || extension == "ssa"
			|| extension == "vtt" || extension == "idx" || extension == "smi") {
			BPath file(parent.Path(), name);
			found.push_back(file.Path());
		}
	}
	std::sort(found.begin(), found.end(), [](const BString& a,
			const BString& b) { return a.Compare(b) < 0; });
	return found;
}


// #pragma mark - pictures


double
Player::DisplayAspect() const
{
	if (fVideoStream < 0 && fCoverFrame.get() != NULL) {
		const AVFrame* frame = fCoverFrame->frame;
		return frame->height > 0 ? (double)frame->width / frame->height : 1.0;
	}
	if (fVideoWidth <= 0 || fVideoHeight <= 0)
		return 16.0 / 9.0;
	return (double)fVideoWidth * fSampleAspect.num
		/ ((double)fVideoHeight * fSampleAspect.den);
}


void
Player::SetVideoSink(VideoSink* sink)
{
	std::lock_guard<std::mutex> lock(fLock);
	fSink = sink;
}


void
Player::SetHardwareDecoding(bool enabled)
{
	if (fHardwareDecoding == enabled)
		return;
	fHardwareDecoding = enabled;
	// The decoding thread notices and starts over with the other kind.
	std::lock_guard<std::mutex> lock(fLock);
	fWakeUp.notify_all();
}


BString
Player::VideoDecoderName()
{
	std::lock_guard<std::mutex> lock(fDecoderLock);
	if (fVideoDecoder == NULL)
		return fCoverFrame.get() != NULL ? "Still picture" : "None";
	return fVideoDecoder->Name();
}


bool
Player::VideoDecoderIsHardware()
{
	std::lock_guard<std::mutex> lock(fDecoderLock);
	return fVideoDecoder != NULL && fVideoDecoder->IsHardware();
}


BString
Player::HardwareDecoderNote()
{
	std::lock_guard<std::mutex> lock(fDecoderLock);
	return fHardwareNote;
}


BString
Player::AudioDecoderName()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (fAudioDecoder == NULL)
		return fAudioError.Length() > 0 ? fAudioError : BString("None");
	return fAudioDecoder->Name();
}


BString
Player::FormatName() const
{
	if (fFormat == NULL || fFormat->iformat == NULL)
		return "";
	return fFormat->iformat->long_name != NULL
		? fFormat->iformat->long_name : fFormat->iformat->name;
}


BString
Player::Metadata(const char* key) const
{
	if (fFormat == NULL)
		return "";
	AVDictionaryEntry* tag = av_dict_get(fFormat->metadata, key, NULL,
		AV_DICT_IGNORE_SUFFIX);
	if (tag == NULL && fAudioStream >= 0) {
		tag = av_dict_get(fFormat->streams[fAudioStream]->metadata, key, NULL,
			AV_DICT_IGNORE_SUFFIX);
	}
	return tag != NULL ? BString(tag->value) : BString();
}


int64
Player::BitRate() const
{
	return fFormat != NULL ? fFormat->bit_rate : 0;
}


PlayerStats
Player::Stats()
{
	PlayerStats stats;
	stats.framesShown = fFramesShown;
	stats.framesDropped = fFramesDropped;
	stats.hurry = fHurry;
	stats.lateness = fLateness;
	{
		std::lock_guard<std::mutex> lock(fLock);
		stats.displayRate = fDisplayRate;
		stats.avOffset = fLastAVOffset;
	}
	stats.audioBuffered = fAudioOutput != NULL ? fAudioOutput->Buffered() : 0;
	stats.videoQueued = fVideoQueue.Count();
	stats.audioQueued = fAudioQueue.Count();
	stats.framesQueued = fFrames.Count();
	stats.decodeTime = fDecodeTime;
	stats.demuxPhase = fDemuxPhase;
	stats.decodePhase = fDecodePhase;
	stats.presentPhase = fPresentPhase;
	stats.scanSeeks = fScanSeeks;
	{
		std::lock_guard<std::mutex> lock(fLock);
		stats.scanShown = fScanShownPts == kNoTime ? -1 : fScanShownPts;
		stats.scanAwaiting = fScanAwaitingFrame;
	}
	stats.composeTime = stats.drawTime = 0;
	VideoSink* sink;
	{
		std::lock_guard<std::mutex> lock(fLock);
		sink = fSink;
	}
	if (sink != NULL)
		sink->GetTimings(&stats.composeTime, &stats.drawTime);
	return stats;
}


void
Player::_DecodeCoverArt(AVStream* stream)
{
	const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (codec == NULL || stream->attached_pic.data == NULL)
		return;
	AVCodecContext* context = avcodec_alloc_context3(codec);
	if (context == NULL)
		return;
	if (avcodec_parameters_to_context(context, stream->codecpar) >= 0
		&& avcodec_open2(context, codec, NULL) >= 0) {
		VideoFramePtr frame = std::make_shared<VideoFrame>();
		if (avcodec_send_packet(context, &stream->attached_pic) >= 0) {
			avcodec_send_packet(context, NULL);
			if (avcodec_receive_frame(context, frame->frame) >= 0) {
				frame->pts = 0;
				fCoverFrame = frame;
			}
		}
	}
	avcodec_free_context(&context);
}


// #pragma mark - threads


void
Player::_Notify(uint32 what)
{
	BMessage message(what);
	fTarget.SendMessage(&message, (BHandler*)NULL, 100000);
}


bool
Player::_QueuesFull()
{
	if (fVideoQueue.Bytes() + fAudioQueue.Bytes() > kMaxQueuedBytes)
		return true;
	bool scanning;
	{
		std::lock_guard<std::mutex> lock(fLock);
		scanning = fScanning;
	}
	bool videoEnough = fVideoStream < 0;
	if (!videoEnough) {
		int count = fVideoQueue.Count();
		videoEnough = scanning ? count >= 2
			: count >= 30 && (fVideoQueue.Duration() >= 1500000 || count >= 150);
	}
	bool audioEnough = fAudioStream < 0 || scanning;
	if (!audioEnough) {
		int count = fAudioQueue.Count();
		audioEnough = count >= 30
			&& (fAudioQueue.Duration() >= 1500000 || count >= 300);
	}
	return videoEnough && audioEnough;
}


void
Player::_PerformSeek()
{
	bigtime_t target;
	bool accurate;
	int direction;
	{
		std::lock_guard<std::mutex> lock(fLock);
		target = fSeekTarget;
		accurate = fSeekAccurate;
		direction = fSeekDirection;
		fSeekRequested = false;
	}

	int64 timestamp = target + fStartTime;
	int result;
	if (direction > 0) {
		result = avformat_seek_file(fFormat, -1, timestamp, timestamp,
			INT64_MAX, 0);
	} else {
		result = avformat_seek_file(fFormat, -1, INT64_MIN, timestamp,
			timestamp, 0);
	}
	if (result < 0)
		result = av_seek_frame(fFormat, -1, timestamp, AVSEEK_FLAG_BACKWARD);
	if (result < 0 && direction > 0)
		result = av_seek_frame(fFormat, -1, timestamp, 0);
	TRACE("airTime: seek to %.3f %s%s: %d\n", target / 1e6,
		accurate ? "accurate" : "key frame",
		direction > 0 ? " forward" : direction < 0 ? " backward" : "", result);

	int videoSerial = fVideoQueue.Flush();
	int audioSerial = fAudioQueue.Flush();
	fFrames.Flush();
	fLateness = 0;

	bigtime_t frameDuration = fVideoTrack.frameRate > 0
		? (bigtime_t)(1000000.0 / fVideoTrack.frameRate) : 40000;
	bigtime_t skipBefore = accurate ? target - frameDuration / 2 : 0;
	{
		std::lock_guard<std::mutex> lock(fLock);
		fEndOfFile = false;
		fLastVideoPacketPts = kNoTime;
		fScanPacketSent = false;
		fScanHitEnd = false;
		fVideoSkipSerial = accurate ? videoSerial : -1;
		fVideoSkipTime = skipBefore;
		fVideoFinishedSerial = -1;
		fSerialStarted = system_time();
		if (fAudioDecoder != NULL)
			fAudioDecoder->SetTrim(audioSerial, accurate ? target : INT64_MIN);
	}
	if (fAudioOutput != NULL)
		fAudioOutput->Flush(audioSerial);
	fAudioClock.Invalidate();
	fExternalClock.Invalidate();
	{
		std::lock_guard<std::mutex> lock(fDecoderLock);
		if (fVideoDecoder != NULL)
			fVideoDecoder->SetSkipBefore(accurate ? skipBefore : 0);
	}
	if (fCaptionDecoder != NULL)
		fCaptionDecoder->Flush();
	for (SubtitleSource& source : fSubtitles) {
		if (source.decoder != NULL)
			source.decoder->Flush();
	}
	if (accurate)
		_RequestSubtitlePreroll(target);

	std::lock_guard<std::mutex> lock(fLock);
	fWakeUp.notify_all();
}


void
Player::_RouteVideoPacket(AVPacket* packet)
{
	AVStream* stream = fFormat->streams[fVideoStream];
	int64 timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
	bigtime_t pts = to_micros(timestamp, stream->time_base);
	if (pts != kNoTime) {
		std::lock_guard<std::mutex> lock(fLock);
		pts -= fStartTime;
		if (fLastVideoPacketPts == kNoTime || pts > fLastVideoPacketPts)
			fLastVideoPacketPts = pts;
	}

	// Closed captions ride along in the video, so they are looked for in
	// every picture, whichever decoder gets it.
	if (fCaptionDecoder != NULL && fCaptionLengthSize >= 0) {
		std::vector<uint8> captions;
		if (extract_a53_captions(fCaptionSource, packet->data, packet->size,
				fCaptionLengthSize, captions)) {
			int64 timestamp = packet->pts != AV_NOPTS_VALUE
				? packet->pts : packet->dts;
			bigtime_t pts = to_micros(timestamp, stream->time_base);
			if (pts != kNoTime) {
				fCaptionDecoder->Add(pts - fStartTime, captions.data(),
					captions.size());
			}
			if (!fCaptionsFound) {
				fCaptionsFound = true;
				_Notify(kMsgPlayerTracksChanged);
			}
		}
	}

	bigtime_t duration = packet->duration > 0
		? to_micros(packet->duration, stream->time_base) : 0;
	if (duration <= 0 && fVideoTrack.frameRate > 0)
		duration = (bigtime_t)(1000000.0 / fVideoTrack.frameRate);
	fVideoQueue.Put(packet, duration);
}


void
Player::_DemuxLoop()
{
	rename_thread(find_thread(NULL), "airTime reader");
	set_thread_priority(find_thread(NULL), B_DISPLAY_PRIORITY);

	AVPacket* packet = av_packet_alloc();
	bigtime_t lastPrune = 0;

	while (!fQuit) {
		bool seek, endOfFile, scanning, scanDone;
		{
			std::lock_guard<std::mutex> lock(fLock);
			seek = fSeekRequested;
			endOfFile = fEndOfFile;
			scanning = fScanning;
			scanDone = fScanPacketSent || fVideoStream < 0;
		}
		if (seek) {
			fDemuxPhase = 1;
			_PerformSeek();
			continue;
		}

		fDemuxPhase = 2;
		if (scanning && scanDone) {
			// The key frame for this step is on its way; nothing more to
			// read until the next seek.
			std::unique_lock<std::mutex> lock(fLock);
			if (!fSeekRequested && !fQuit)
				fWakeUp.wait_for(lock, std::chrono::milliseconds(10));
			continue;
		}
		if (endOfFile || _QueuesFull()) {
			fDemuxPhase = 3;
			std::unique_lock<std::mutex> lock(fLock);
			if (!fSeekRequested && !fQuit)
				fWakeUp.wait_for(lock, std::chrono::milliseconds(10));
			continue;
		}

		fDemuxPhase = 4;
		int result = av_read_frame(fFormat, packet);
		fDemuxPhase = 5;
		if (result < 0) {
			if (result == AVERROR_EOF || avio_feof(fFormat->pb)) {
				TRACE("airTime: end of file\n");
				if (scanning) {
					std::lock_guard<std::mutex> lock(fLock);
					fScanHitEnd = true;
					fScanPacketSent = true;
				}
				if (fVideoStream >= 0)
					fVideoQueue.PutEndOfStream(fVideoStream);
				if (fAudioStream >= 0)
					fAudioQueue.PutEndOfStream(fAudioStream);
				if (fCaptionDecoder != NULL)
					fCaptionDecoder->EndOfStream();
				std::lock_guard<std::mutex> lock(fLock);
				fEndOfFile = true;
			} else {
				// A network share that hiccupped, perhaps; try again shortly.
				snooze(10000);
			}
			continue;
		}

		int index = packet->stream_index;
		if (index == fVideoStream) {
			if (scanning && (packet->flags & AV_PKT_FLAG_KEY) == 0)
				av_packet_unref(packet);
			else if (scanning) {
				_RouteVideoPacket(packet);
				fVideoQueue.PutEndOfStream(fVideoStream);
				std::lock_guard<std::mutex> lock(fLock);
				fScanPacketSent = true;
			} else
				_RouteVideoPacket(packet);
		} else if (index == fAudioStream) {
			if (scanning) {
				av_packet_unref(packet);
			} else {
				AVStream* stream = fFormat->streams[index];
				bigtime_t duration = packet->duration > 0
					? to_micros(packet->duration, stream->time_base) : 0;
				fAudioQueue.Put(packet, duration);
			}
		} else if (index == fCaptionStream) {
			if (fCaptionDecoder != NULL) {
				fCaptionDecoder->AddPacket(packet,
					fFormat->streams[index]->time_base, fStartTime);
			}
			av_packet_unref(packet);
		} else {
			for (SubtitleSource& source : fSubtitles) {
				if (source.decoder != NULL && source.info.stream == index) {
					source.decoder->SetVideoSize(fVideoWidth, fVideoHeight);
					source.decoder->Decode(packet);
					break;
				}
			}
			av_packet_unref(packet);
		}

		// Embedded subtitles far behind the playback position will be read
		// again if anyone goes back there.
		bigtime_t now = system_time();
		if (now - lastPrune > 2000000) {
			lastPrune = now;
			bigtime_t position = Position();
			for (SubtitleSource& source : fSubtitles) {
				if (source.decoder != NULL)
					source.track->Prune(position - 120000000LL);
			}
			fCaptionTrack.Prune(position - 120000000LL);
		}
	}
	av_packet_free(&packet);
}


/*!	Whether libavcodec would be slower still than a hardware decoder that
	falls behind: on the ARM boards, pictures larger than 1080p and ten-bit
	HEVC. On the Cubie A7S a 4K Main 10 film fell from 7-20 pictures a
	second on the Cedar engine to 1-2 in software, and a 1080p Main 10 one
	manages 11 there. */
static bool
software_is_slower_still(const AVCodecParameters* parameters)
{
#if defined(__aarch64__)
	return (int64)parameters->width * parameters->height > 1920 * 1088
		|| (parameters->codec_id == AV_CODEC_ID_HEVC
			&& (parameters->profile == FF_PROFILE_HEVC_MAIN_10
				|| parameters->format == AV_PIX_FMT_YUV420P10LE));
#else
	(void)parameters;
	return false;
#endif
}


bool
Player::_CreateVideoDecoder()
{
	AVStream* stream = fFormat->streams[fVideoStream];
	VideoDecoder* decoder = NULL;
	BString note;
	if (fHardwareDecoding) {
		decoder = create_hardware_decoder(stream, &fVideoQueue, fStartTime,
			&note);
		if (decoder != NULL)
			note = "";
	} else
		note = "hardware decoding is turned off";

	if (decoder == NULL) {
		SoftwareVideoDecoder* software = new SoftwareVideoDecoder(stream,
			&fVideoQueue, fStartTime);
		BString reason;
		if (software->Init(&reason) != B_OK) {
			delete software;
			BMessage message(kMsgPlayerError);
			BString error;
			error.SetToFormat("The video cannot be decoded: %s.",
				reason.String());
			message.AddString("error", error);
			fTarget.SendMessage(&message, (BHandler*)NULL, 100000);
			return false;
		}
		decoder = software;
	}
	TRACE("airTime: video decoder %s%s%s\n", decoder->Name().String(),
		note.Length() > 0 ? ", because " : "", note.String());

	{
		std::lock_guard<std::mutex> lock(fDecoderLock);
		delete fVideoDecoder;
		fVideoDecoder = decoder;
		fHardwareNote = note;
	}
	_Notify(kMsgPlayerDecoderChanged);
	return true;
}


void
Player::_FallBackToSoftware(const char* why, bigtime_t resumeTime)
{
	fprintf(stderr, "airTime: hardware decoding stopped (%s); continuing "
		"with libavcodec\n", why);
	AVStream* stream = fFormat->streams[fVideoStream];
	SoftwareVideoDecoder* software = new SoftwareVideoDecoder(stream,
		&fVideoQueue, fStartTime);
	BString reason;
	if (software->Init(&reason) != B_OK) {
		delete software;
		software = NULL;
	}
	{
		std::lock_guard<std::mutex> lock(fDecoderLock);
		delete fVideoDecoder;
		fVideoDecoder = software;
		fHardwareNote = why;
	}
	{
		// The packets the hardware swallowed are gone: read them again.
		std::unique_lock<std::mutex> lock(fLock);
		int previousSerial = fVideoQueue.Serial();
		bigtime_t position = fPositionPending ? fPendingPosition
			: resumeTime != kNoTime ? resumeTime : fLastShownPts;
		_RequestSeekLocked(position, true, 0);
		// Reassess the new decoder at full quality; the old one's backlog
		// must not make software skip pictures for several more seconds.
		fHurry = 0;
		fHurryChanged = system_time();
		fHurryPatience = 3000000;
		fHurryEased = 0;
		fShowNextFrame = true;
		// The new decoder has no reference pictures. The demux thread must
		// finish seeking and discard the old packets before it can consume
		// anything, even when the old queue still has buffered data.
		while (!fQuit && !fVideoQueue.HasNewerSerial(previousSerial))
			fWakeUp.wait_for(lock, std::chrono::milliseconds(20));
	}
	_Notify(kMsgPlayerDecoderChanged);
}


/*!	When pictures come out of the decoder later than they are due - the
	processor is too slow for the film, or the film plays fast - the decoder
	leaves work out, a step at a time, until they come on time; then it puts
	the work back, more slowly. Falling a second behind skips to key frames
	until it has caught up.
*/
int
Player::_HurryLevel()
{
	// AIRTIME_HURRY_CYCLE=1: every level in turn, two seconds each, to
	// try the decoders with all of them.
	static bool sCycle = getenv("AIRTIME_HURRY_CYCLE") != NULL;
	if (sCycle) {
		fHurry = (int)((system_time() / 2000000) % 5);
		return fHurry;
	}

	bool active;
	{
		std::lock_guard<std::mutex> lock(fLock);
		active = fPlaying && !fScanning;
	}
	bigtime_t now = system_time();
	bigtime_t lateness = fLateness;
	bigtime_t since = now - fHurryChanged;
	int level = fHurry;
	if (!active) {
		// Paused or scanning: nothing to keep up with.
	} else if (level == 4) {
		if (lateness < 50000)
			level = 2;
	} else if (lateness > 1000000 && level >= 2 && since > 500000) {
		level = 4;
	} else if (lateness > 120000 && level < 3 && since > 500000) {
		level++;
		// Behind again soon after easing off: wait longer next time.
		if (now - fHurryEased < 10000000)
			fHurryPatience = std::min(fHurryPatience * 2, (bigtime_t)60000000);
	} else if (lateness < 20000 && level > 0 && since > fHurryPatience) {
		level--;
		fHurryEased = now;
	}
	if (level != fHurry) {
		TRACE("airTime: %.0f ms late, decoding at hurry level %d\n",
			lateness / 1000.0, level);
		fHurry = level;
		fHurryChanged = now;
	}
	return fHurry;
}


void
Player::_VideoDecodeLoop()
{
	rename_thread(find_thread(NULL), "airTime video decoder");
	set_thread_priority(find_thread(NULL), B_DISPLAY_PRIORITY);

	bool hardwareWanted = fHardwareDecoding;
	if (!_CreateVideoDecoder()) {
		std::lock_guard<std::mutex> lock(fLock);
		fVideoFinishedSerial = fVideoQueue.Serial();
		return;
	}

	VideoFramePtr lastDropped;
	bigtime_t lastDecodedPts = kNoTime;
	int lastDecodedSerial = -1;
	int speedSerial = -1;
	unsigned speedFrames = 0;
	bigtime_t speedSpent = 0, speedDuration = 0;
	auto resetSpeed = [&]() {
		speedFrames = 0; speedSpent = speedDuration = 0;
	};
	while (!fQuit) {
		if (hardwareWanted != fHardwareDecoding) {
			resetSpeed();
			hardwareWanted = fHardwareDecoding;
			_CreateVideoDecoder();
			std::unique_lock<std::mutex> lock(fLock);
			int previousSerial = fVideoQueue.Serial();
			bigtime_t position = fPositionPending ? fPendingPosition
				: fLastShownPts;
			_RequestSeekLocked(position, true, 0);
			fShowNextFrame = true;
			// In particular, a recreated hardware decoder must start at the
			// seek's random-access picture, not a buffered mid-GOP packet.
			while (!fQuit && !fVideoQueue.HasNewerSerial(previousSerial))
				fWakeUp.wait_for(lock, std::chrono::milliseconds(20));
			continue;
		}

		VideoDecoder* decoder = fVideoDecoder;
		if (decoder == NULL) {
			std::unique_lock<std::mutex> lock(fLock);
			fVideoFinishedSerial = fVideoQueue.Serial();
			fWakeUp.wait_for(lock, std::chrono::milliseconds(50));
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(fLock);
			decoder->SetKeyframesOnly(fScanning);
		}
		decoder->SetHurry(_HurryLevel());

		// A hardware decoder cannot leave work out to catch up, but it need
		// not make pictures that would only be dropped: when it is far
		// behind the clock (after a seek into a long group of pictures the
		// sound has gone on by seconds), it is asked for none until where
		// the clock will be by then. The Raspberry Pi's firmware decoder is
		// twice as fast at that, and the others save their copies.
		if (decoder->IsHardware() && lastDecodedPts != kNoTime
			&& lastDecodedSerial == fVideoQueue.Serial()) {
			bool valid = false;
			bigtime_t master = _MasterClock(&valid);
			std::lock_guard<std::mutex> lock(fLock);
			bigtime_t behind = master - lastDecodedPts;
			if (valid && fPlaying && !fScanning && behind > 250000
				&& fVideoSkipSerial != lastDecodedSerial
				&& fLastVideoPacketPts != kNoTime) {
				bigtime_t target = master + behind / 2 + 100000;
				// Never ask hardware to suppress every remaining picture.
				// Its skipped output may contain no pixels, so EOF cannot show
				// it later. A demuxed video timestamp bounds catch-up even when
				// the duration is unknown or audio continues past the video.
				target = std::min(target, fLastVideoPacketPts);
				if (target > 0 && target > lastDecodedPts) {
					fVideoSkipSerial = lastDecodedSerial;
					fVideoSkipTime = target;
					decoder->SetSkipBefore(target);
					TRACE("airTime: the decoder is %.0f ms behind, no pictures "
						"until %.3f\n", behind / 1000.0, target / 1e6);
				}
			}
		}

		VideoFramePtr frame;
		bool inputReady = fVideoQueue.Count() >= 8;
		bigtime_t decodeStart = system_time();
		fDecodePhase = 1;
		status_t status = decoder->Decode(frame);
		fDecodePhase = 2;
		if (fQuit)
			break;
		bigtime_t spent = system_time() - decodeStart;
		if (status == B_OK) {
			fDecodeTime = (fDecodeTime * 15 + spent) / 16;
		} else
			resetSpeed();

		if (status == B_OK) {
			if (frame->serial != fVideoQueue.Serial())
				continue;
			// A decoder can accept every packet yet be too slow to play the
			// film. Measure only ordinary playback with buffered input, after
			// eight warm-up pictures in this seek generation. Network waits,
			// pause, scanning and accelerated playback are not decoder tests.
			bool measureSpeed;
			{
				std::lock_guard<std::mutex> lock(fLock);
				measureSpeed = fPlaying && !fScanning && !fPositionPending
					&& fabs(fRate - 1.0) < 0.001;
			}
			measureSpeed = measureSpeed && decoder->IsHardware() && inputReady
				&& fVideoQueue.Count() >= 8 && frame->duration > 0;
			if (speedSerial != frame->serial || !measureSpeed) {
				resetSpeed();
				speedSerial = frame->serial;
			}
			if (measureSpeed && ++speedFrames > 8) {
				speedSpent += spent;
				speedDuration += frame->duration;
				if (speedFrames >= 40) {
					bool valid;
					bigtime_t master = _MasterClock(&valid);
					if (valid && master - frame->pts > 250000
						&& speedSpent > speedDuration * 5 / 4
						&& !software_is_slower_still(
							fFormat->streams[fVideoStream]->codecpar)) {
						BString why;
						why.SetToFormat("%s cannot keep up with this film",
							decoder->Name().String());
						_FallBackToSoftware(why.String(), master);
						lastDropped.reset();
						lastDecodedPts = kNoTime;
						resetSpeed();
						continue;
					}
					speedFrames = 8;
					speedSpent = speedDuration = 0;
				}
			}
			lastDecodedPts = frame->pts;
			lastDecodedSerial = frame->serial;
			if (fVideoWidth <= 0 || fVideoHeight <= 0) {
				fVideoWidth = frame->frame->width;
				fVideoHeight = frame->frame->height;
			}
			bool skip = false;
			{
				std::lock_guard<std::mutex> lock(fLock);
				if (fVideoSkipSerial == frame->serial) {
					if (frame->pts < fVideoSkipTime)
						skip = true;
					else
						fVideoSkipSerial = -1;
				}
			}
			if (skip) {
				lastDropped = frame;
				continue;
			}
			decoder->SetSkipBefore(0);
			lastDropped.reset();
			int serial = frame->serial;
			fDecodePhase = 3;
			fFrames.Push(frame, [this, serial, hardwareWanted]() {
				return !fQuit && fVideoQueue.Serial() == serial
					&& hardwareWanted == fHardwareDecoding;
			});
			continue;
		}

		if (status == B_LAST_BUFFER_ERROR) {
			int serial = decoder->Serial();
			if (lastDropped.get() != NULL && lastDropped->serial == serial
				&& !lastDropped->hardware && serial == fVideoQueue.Serial()) {
				// A seek past the last picture shows the last picture.
				fFrames.Push(lastDropped, [this, serial]() {
					return !fQuit && fVideoQueue.Serial() == serial;
				});
			}
			lastDropped.reset();
			std::unique_lock<std::mutex> lock(fLock);
			fVideoFinishedSerial = serial;
			fWakeUp.notify_all();
			while (!fQuit && fVideoQueue.Serial() == serial
				&& hardwareWanted == fHardwareDecoding) {
				fWakeUp.wait_for(lock, std::chrono::milliseconds(20));
			}
			continue;
		}
		if (status == B_INTERRUPTED)
			continue;
		if (status == B_CANCELED)
			break;

		if (decoder->IsHardware()) {
			BString why;
			why.SetToFormat("%s gave up: %s", decoder->Name().String(),
				strerror(status));
			_FallBackToSoftware(why.String());
			continue;
		}
		// The software decoder failed for good; wait for something else.
		std::unique_lock<std::mutex> lock(fLock);
		fVideoFinishedSerial = fVideoQueue.Serial();
		fWakeUp.wait_for(lock, std::chrono::milliseconds(50));
	}
}


bigtime_t
Player::_MasterClock(bool* valid)
{
	*valid = false;
	if (fAudioStream >= 0 && fAudioDecoder != NULL && fAudioOutput != NULL) {
		int audioSerial = fAudioQueue.Serial();
		bool audioDone = fAudioDecoder->Finished(audioSerial)
			&& fAudioOutput->Drained(audioSerial);
		if (!audioDone) {
			int serial;
			bigtime_t time = fAudioClock.Get(&serial);
			if (serial == audioSerial) {
				*valid = true;
				// Keep the other clock close, for when the sound ends first.
				fExternalClock.Set(time, fVideoQueue.Serial());
				return time;
			}
			return 0;
		}
	}
	int serial;
	bigtime_t time = fExternalClock.Get(&serial);
	*valid = serial == fVideoQueue.Serial();
	return time;
}


bool
Player::_Show(const VideoFramePtr& frame)
{
	VideoSink* sink;
	{
		std::lock_guard<std::mutex> lock(fLock);
		sink = fSink;
	}
	// Nobody to show it to (between files): it was not shown.
	if (sink == NULL || !sink->DisplayFrame(frame))
		return false;

	fFramesShown++;
	bigtime_t now = system_time();
	std::lock_guard<std::mutex> lock(fLock);
	fRateWindowFrames++;
	if (fRateWindowStart == 0)
		fRateWindowStart = now;
	if (now - fRateWindowStart >= 1000000) {
		fDisplayRate = fRateWindowFrames * 1000000.0 / (now - fRateWindowStart);
		fRateWindowStart = now;
		fRateWindowFrames = 0;
	}
	return true;
}


bool
Player::_MediaEnded()
{
	bool videoDone = true;
	if (fVideoStream >= 0) {
		std::lock_guard<std::mutex> lock(fLock);
		videoDone = fVideoFinishedSerial == fVideoQueue.Serial()
			&& !fSeekRequested;
	}
	if (videoDone && fFrames.Count() > 0)
		videoDone = false;
	bool audioDone = true;
	if (fAudioStream >= 0 && fAudioDecoder != NULL && fAudioOutput != NULL) {
		int serial = fAudioQueue.Serial();
		audioDone = fAudioDecoder->Finished(serial)
			&& fAudioOutput->Drained(serial);
	}
	return videoDone && audioDone;
}


bool
Player::_ScanStep()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (!fScanning)
		return false;
	bigtime_t now = system_time();
	bool hasVideo = fVideoStream >= 0;
	if (hasVideo && fScanAwaitingFrame && now - fScanStepTime < 600000
		&& !fScanHitEnd) {
		// Show the last key frame asked for before asking for another.
		return false;
	}
	if (fScanStepTime != 0 && now - fScanStepTime < kScanStepInterval)
		return false;

	bigtime_t elapsed = fScanStepTime != 0 ? now - fScanStepTime : 0;
	fScanStepTime = now;
	fScanPosition += (bigtime_t)(elapsed * fRate);

	bool atEnd = (fDuration > 0 && fScanPosition >= fDuration)
		|| (fScanHitEnd && fRate > 0);
	if (fRate > 0 && atEnd) {
		// Fast forward ran into the end: stop on the last picture.
		fScanning = false;
		fRate = 1.0;
		fPlaying = false;
		fScanPosition = fDuration;
		_RequestSeekLocked(fDuration > 0 ? fDuration : fScanShownPts, true, 0);
		fShowNextFrame = true;
		fEnded = true;
		_UpdateClocksLocked();
		return true;
	}
	if (fRate < 0 && fScanPosition <= 0) {
		// Rewound to the beginning: stop there, as QuickTime did.
		fScanning = false;
		fRate = 1.0;
		fPlaying = false;
		fScanPosition = 0;
		_RequestSeekLocked(0, true, 0);
		fShowNextFrame = true;
		_UpdateClocksLocked();
		return true;
	}

	// A long group of pictures may still have the right key frame on screen.
	if (hasVideo && fScanShownPts != kNoTime && !fScanAwaitingFrame) {
		if (fRate > 0 && fScanPosition <= fScanShownPts)
			return false;
		if (fRate < 0 && fScanPosition >= fScanShownPts)
			return false;
	}
	_RequestSeekLocked(fScanPosition, false, fRate > 0 ? 1 : -1);
	fScanSeeks++;
	// Position() reports the scan position, not the seek target.
	fPositionPending = false;
	fScanAwaitingFrame = hasVideo;
	return false;
}


void
Player::_PresentLoop()
{
	rename_thread(find_thread(NULL), "airTime presentation");
	set_thread_priority(find_thread(NULL), B_URGENT_DISPLAY_PRIORITY);

	bool coverShown = false;
	// AIRTIME_TRACE_DROPS: say why pictures are left out
	static const bool sTraceDrops = getenv("AIRTIME_TRACE_DROPS") != NULL;
	bigtime_t lastShowEnd = system_time();
	bigtime_t lastShowTime = 0;
	bool wasPending = false;

	while (!fQuit) {
		bool playing, scanning, showNext;
		{
			std::lock_guard<std::mutex> lock(fLock);
			playing = fPlaying;
			scanning = fScanning;
			showNext = fShowNextFrame;
		}
		if (scanning && _ScanStep())
			_Notify(kMsgPlayerStateChanged);

		if (fVideoStream < 0) {
			if (fCoverFrame.get() != NULL && !coverShown)
				coverShown = _Show(fCoverFrame);
			if (playing && !scanning && _MediaEnded()) {
				_HandleEnd();
				continue;
			}
			std::unique_lock<std::mutex> lock(fLock);
			if (fPositionPending && !fSeekRequested
				&& fAudioClock.Serial() == fAudioQueue.Serial()
				&& fAudioQueue.Serial() > fSeekFromAudioSerial)
				fPositionPending = false;
			if (fPositionPending && !fPlaying && !fSeekRequested
				&& fAudioQueue.Serial() > fSeekFromAudioSerial
				&& system_time() - fSerialStarted > 200000) {
				// Paused: nothing will be heard to confirm the seek.
				fPositionPending = false;
				fExternalClock.Set(fPendingPosition, fVideoQueue.Serial());
			}
			fWakeUp.wait_for(lock, std::chrono::milliseconds(20));
			continue;
		}

		if (sTraceDrops) {
			// how long a seek takes to its first picture
			std::lock_guard<std::mutex> lock(fLock);
			if (wasPending && !fPositionPending) {
				fprintf(stderr, "airTime: %.0f ms from the seek to the "
					"picture at %.3f\n",
					(system_time() - fSeekRequestedAt) / 1000.0,
					fLastShownPts / 1e6);
			}
			wasPending = fPositionPending;
		}

		VideoFramePtr frame = fFrames.Peek();
		if (frame.get() == NULL) {
			if (playing && !scanning && _MediaEnded()) {
				_HandleEnd();
				continue;
			}
			fFrames.Wait(20000);
			continue;
		}
		int serial = fVideoQueue.Serial();
		if (frame->serial != serial) {
			fFrames.Pop();
			continue;
		}
		bool beforeSeek;
		{
			std::lock_guard<std::mutex> lock(fLock);
			beforeSeek = fPositionPending && !fScanning
				&& frame->serial <= fSeekFromVideoSerial;
		}
		if (beforeSeek) {
			// The reader has not got to the seek yet; what is queued is from
			// the old position. Wait for the new pictures rather than show it.
			std::unique_lock<std::mutex> lock(fLock);
			fWakeUp.wait_for(lock, std::chrono::milliseconds(5));
			continue;
		}

		if (scanning) {
			if (_Show(frame)) {
				fFrames.Pop();
				std::lock_guard<std::mutex> lock(fLock);
				fScanShownPts = frame->pts;
				fScanAwaitingFrame = false;
				fLastShownPts = frame->pts;
			} else
				snooze(5000);
			continue;
		}

		if (!playing) {
			if (showNext) {
				if (_Show(frame)) {
					fFrames.Pop();
					std::lock_guard<std::mutex> lock(fLock);
					fShowNextFrame = false;
					fLastShownPts = frame->pts;
					fPositionPending = false;
					fExternalClock.Set(frame->pts, serial);
				} else
					snooze(5000);
			} else {
				std::unique_lock<std::mutex> lock(fLock);
				if (!fShowNextFrame && !fPlaying && !fQuit)
					fWakeUp.wait_for(lock, std::chrono::milliseconds(50));
			}
			continue;
		}

		bool valid;
		bigtime_t master = _MasterClock(&valid);
		if (!valid) {
			bool waitForSound = false;
			if (fAudioStream >= 0 && fAudioDecoder != NULL) {
				int audioSerial = fAudioQueue.Serial();
				std::lock_guard<std::mutex> lock(fLock);
				waitForSound = !fAudioDecoder->Finished(audioSerial)
					&& system_time() - fSerialStarted < kAudioStartWait;
			}
			if (waitForSound) {
				snooze(3000);
				continue;
			}
			// Run the film from its own clock, starting with this picture.
			double speed;
			{
				std::lock_guard<std::mutex> lock(fLock);
				speed = fRate > 0 ? fRate : 1.0;
			}
			fExternalClock.Set(frame->pts, serial, -1, speed);
			master = frame->pts;
		}

		bigtime_t delay = frame->pts - master;
		double rate;
		{
			std::lock_guard<std::mutex> lock(fLock);
			rate = fRate > 0 ? fRate : 1.0;
		}
		if (delay > 2000) {
			// Early. A picture far in the future (a broken time stamp) is
			// not worth waiting seconds for.
			if (delay > 2000000) {
				fExternalClock.Set(frame->pts, serial, -1, rate);
			} else {
				bigtime_t wait = (bigtime_t)(delay / rate);
				if (wait > 10000)
					wait = 10000;
				std::unique_lock<std::mutex> lock(fLock);
				if (fPlaying && !fSeekRequested)
					fWakeUp.wait_for(lock, std::chrono::microseconds(wait));
				continue;
			}
		}

		// How late pictures are coming, for the decoder to catch up.
		fLateness = (fLateness * 7 + std::max((bigtime_t)0, -delay)) / 8;

		// Late: let it go if the next one is due as well. Not after one
		// hold-up, though: when pictures have been on time and showing them
		// takes less than their duration, the film is back in step within a
		// few of them, with nothing left out. (On the Raspberry Pi 4 the
		// firmware's decoder slows all memory down several times over for 30
		// to 100 ms now and then; every such hold-up cost a picture or two.)
		// Sound that leads the picture by up to 90 ms is not noticed.
		VideoFramePtr next = fFrames.Peek(1);
		bigtime_t tolerance = std::min((bigtime_t)90000, 3 * frame->duration);
		static const bool sStrict = getenv("AIRTIME_STRICT_DROPS") != NULL;
		bool holdUp = !sStrict && fLateness < frame->duration / 2
			&& delay > -tolerance;
		if (delay < -frame->duration && !holdUp && next.get() != NULL
			&& next->serial == serial && next->pts <= master) {
			fFrames.Pop();
			fFramesDropped++;
			if (sTraceDrops) {
				fprintf(stderr, "airTime: dropped %.3f, %.1f ms late; the "
					"last picture took %.1f ms to show, and ended %.1f ms "
					"ago; next %.3f\n", frame->pts / 1e6, -delay / 1000.0,
					lastShowTime / 1000.0,
					(system_time() - lastShowEnd) / 1000.0, next->pts / 1e6);
			}
			std::lock_guard<std::mutex> lock(fLock);
			fLastShownPts = frame->pts;
			continue;
		}

		bigtime_t showStart = system_time();
		if (sTraceDrops && showStart - lastShowEnd > 50000) {
			fprintf(stderr, "airTime: %.1f ms between pictures before "
				"%.3f (%.1f ms late)\n", (showStart - lastShowEnd) / 1000.0,
				frame->pts / 1e6, -delay / 1000.0);
		}
		bool shown = _Show(frame);
		lastShowEnd = system_time();
		lastShowTime = lastShowEnd - showStart;
		if (sTraceDrops && lastShowTime > 30000) {
			fprintf(stderr, "airTime: %.1f ms to show %.3f\n",
				lastShowTime / 1000.0, frame->pts / 1e6);
		}
		if (shown) {
			fFrames.Pop();
			std::lock_guard<std::mutex> lock(fLock);
			fLastShownPts = frame->pts;
			fPositionPending = false;
			fLastAVOffset = frame->pts - master;
		} else {
			// The window was busy; try again, or drop it once it is late.
			snooze(2000);
		}
	}
}


void
Player::_RequestSubtitlePreroll(bigtime_t time)
{
	std::lock_guard<std::mutex> lock(fPrerollLock);
	fPrerollTarget = time;
	fPrerollRequested = true;
	fPrerollWake.notify_all();
}


void
Player::_SubtitlePrerollLoop()
{
	rename_thread(find_thread(NULL), "airTime subtitle preroll");
	static const bigtime_t kPreroll = 10000000;

	AVFormatContext* format = NULL;
	std::vector<SubtitleDecoder*> decoders;
	AVPacket* packet = av_packet_alloc();

	while (!fQuit) {
		bigtime_t target;
		{
			std::unique_lock<std::mutex> lock(fPrerollLock);
			while (!fPrerollRequested && !fQuit)
				fPrerollWake.wait_for(lock, std::chrono::milliseconds(200));
			if (fQuit)
				break;
			target = fPrerollTarget;
			fPrerollRequested = false;
		}

		// Only the track on screen is worth the reading.
		int stream = -1;
		SubtitleTrack* track = NULL;
		AVCodecParameters* parameters = NULL;
		AVRational timeBase;
		size_t sourceIndex = 0;
		{
			std::lock_guard<std::mutex> lock(fLock);
			if (fSubtitleSelected >= 0
				&& fSubtitleSelected < (int)fSubtitles.size()
				&& fSubtitles[fSubtitleSelected].decoder != NULL) {
				sourceIndex = fSubtitleSelected;
				stream = fSubtitles[sourceIndex].info.stream;
				track = fSubtitles[sourceIndex].track;
			}
		}
		if (stream < 0 || target <= 0)
			continue;

		if (format == NULL) {
			if (avformat_open_input(&format, fPath.String(), NULL, NULL) < 0) {
				format = NULL;
				break;
			}
			avformat_find_stream_info(format, NULL);
			decoders.resize(format->nb_streams, NULL);
		}
		if (stream >= (int)format->nb_streams)
			continue;
		// Every stream is read, so the reading can stop once anything is
		// past the target; a sparse subtitle track alone could keep it
		// reading for minutes.
		parameters = format->streams[stream]->codecpar;
		timeBase = format->streams[stream]->time_base;
		if (decoders[stream] == NULL) {
			decoders[stream] = new SubtitleDecoder(parameters, timeBase,
				fStartTime, track);
			BString reason;
			if (decoders[stream]->Init(&reason) != B_OK) {
				delete decoders[stream];
				decoders[stream] = NULL;
				continue;
			}
			decoders[stream]->SetVideoSize(fVideoWidth, fVideoHeight);
		}
		SubtitleDecoder* decoder = decoders[stream];
		decoder->Flush();

		int64 from = std::max((bigtime_t)0, target - kPreroll) + fStartTime;
		if (avformat_seek_file(format, -1, INT64_MIN, from, from, 0) < 0)
			continue;
		bigtime_t until = target + fStartTime + 1000000;
		int read = 0;
		while (!fQuit && av_read_frame(format, packet) >= 0) {
			if (packet->stream_index == stream)
				decoder->Decode(packet);
			int64 timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts
				: packet->pts;
			bool past = timestamp != AV_NOPTS_VALUE
				&& packet->stream_index < (int)format->nb_streams
				&& to_micros(timestamp,
					format->streams[packet->stream_index]->time_base) > until;
			av_packet_unref(packet);
			// Another seek asked for meanwhile: start over there.
			bool again;
			{
				std::lock_guard<std::mutex> lock(fPrerollLock);
				again = fPrerollRequested;
			}
			if (past || again || ++read > 20000)
				break;
		}
		TRACE("airTime: subtitle preroll to %.3f, %d packets, %d events\n",
			target / 1e6, read, track->Count());
		_Notify(kMsgPlayerTracksChanged);
	}

	for (SubtitleDecoder* decoder : decoders)
		delete decoder;
	av_packet_free(&packet);
	if (format != NULL)
		avformat_close_input(&format);
}


void
Player::_HandleEnd()
{
	bool notify = false;
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (fEnded || fSeekRequested) {
			// Already handled, or something new is on its way.
		} else if (fLooping) {
			_RequestSeekLocked(0, true, 0);
		} else {
			fEnded = true;
			fPlaying = false;
			_UpdateClocksLocked();
			notify = true;
		}
	}
	if (notify)
		_Notify(kMsgPlayerEnded);
	else
		snooze(5000);
}

}	// namespace airtime
