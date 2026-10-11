/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_PLAYER_H
#define AIRTIME_PLAYER_H


#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <Messenger.h>
#include <String.h>

#include "AudioDecoder.h"
#include "AudioOutput.h"
#include "Bitstream.h"
#include "Clock.h"
#include "PacketQueue.h"
#include "Subtitles.h"
#include "Tracks.h"
#include "VideoDecoder.h"
#include "VideoFrame.h"


namespace airtime {

// Notifications the player posts to its target.
enum {
	kMsgPlayerStateChanged		= 'pSTA',	// playing, rate, position jumps
	kMsgPlayerEnded				= 'pEND',
	kMsgPlayerTracksChanged		= 'pTRK',	// captions found, files added
	kMsgPlayerDecoderChanged	= 'pDEC',
	kMsgPlayerError				= 'pERR'	// "error" string
};


/*!	Whoever shows the pictures. DisplayFrame() is called on the player's
	presentation thread and must not block for long: return false if the
	picture could not be shown right now. */
class VideoSink {
public:
	virtual						~VideoSink() {}
	virtual	bool				DisplayFrame(const VideoFramePtr& frame) = 0;
	// Average microseconds spent making a picture and putting it on screen.
	virtual	void				GetTimings(bigtime_t* compose,
									bigtime_t* draw) { *compose = *draw = 0; }
};


struct PlayerStats {
	int64			framesShown;
	int64			framesDropped;
	double			displayRate;	// pictures per second, recently
	bigtime_t		audioBuffered;
	bigtime_t		avOffset;		// video minus audio, last picture
	int				videoQueued;	// packets
	int				audioQueued;
	int				framesQueued;
	bigtime_t		decodeTime;		// average per picture
	bigtime_t		composeTime;
	bigtime_t		drawTime;
	int				demuxPhase;
	int				decodePhase;
	int				presentPhase;
	int				scanSeeks;
	bigtime_t		scanShown;
	bool			scanAwaiting;
	int				hurry;			// see VideoDecoder::SetHurry()
	bigtime_t		lateness;		// how late pictures come, smoothed
};


class Player {
public:
								Player(const BMessenger& target);
								~Player();

			status_t			Open(const char* path, BString* error);
			void				Close();
			bool				IsOpen() const { return fFormat != NULL; }

			// Playback.
			void				Play();
			void				Pause();
			void				TogglePlaying();
			bool				IsPlaying();
			// 1 is normal speed. 0.5 to 3 play with sound; faster than that,
			// and every negative rate, scan through key frames.
			void				SetRate(double rate);
			double				Rate();
			bool				IsScanning();
			void				Seek(bigtime_t time, bool accurate = true);
			void				StepFrame(int direction);
			bigtime_t			Position();
			bigtime_t			Duration() const { return fDuration; }
			bool				AtEnd();
			void				SetLooping(bool loop);
			bool				IsLooping() const { return fLooping; }

			// Sound.
			void				SetVolume(float volume);
			void				SetMuted(bool muted);
			const std::vector<TrackInfo>& AudioTracks() const
									{ return fAudioTracks; }
			int					CurrentAudioTrack();
			void				SelectAudioTrack(int index);

			// Subtitles and captions.
			std::vector<TrackInfo> SubtitleTracks();
			int					CurrentSubtitleTrack();
			void				SelectSubtitleTrack(int index);
			status_t			AddSubtitleFile(const char* path,
									BString* error);
			bool				HasCaptions();
			void				SetCaptionsEnabled(bool enabled);
			bool				CaptionsEnabled();
			std::vector<SubtitleEventPtr> ActiveSubtitles(bigtime_t time);
			void				SetPreferredLanguages(const char* audio,
									const char* subtitles);

			// Pictures.
			bool				HasVideo() const { return fVideoStream >= 0
									|| fCoverFrame.get() != NULL; }
			bool				HasMovingVideo() const
									{ return fVideoStream >= 0; }
			bool				HasAudio() const { return fAudioStream >= 0; }
			int					VideoWidth() const { return fVideoWidth; }
			int					VideoHeight() const { return fVideoHeight; }
			// Width over height as shown, pixel aspect included.
			double				DisplayAspect() const;
			void				SetVideoSink(VideoSink* sink);
			void				SetHardwareDecoding(bool enabled);
			bool				HardwareDecoding() const
									{ return fHardwareDecoding; }
			BString				VideoDecoderName();
			bool				VideoDecoderIsHardware();
			BString				HardwareDecoderNote();
			BString				AudioDecoderName();
			const TrackInfo&	VideoTrack() const { return fVideoTrack; }
			VideoFramePtr		CoverArt() const { return fCoverFrame; }

			// About the file.
			const std::vector<Chapter>& Chapters() const { return fChapters; }
			BString				Path() const { return fPath; }
			BString				FormatName() const;
			BString				Metadata(const char* key) const;
			int64				FileSize() const { return fFileSize; }
			int64				BitRate() const;
			PlayerStats			Stats();

			// Subtitle files next to the movie with the same name.
	static	std::vector<BString> FindSidecarSubtitles(const char* moviePath);

private:
			struct SubtitleSource {
				TrackInfo				info;
				SubtitleTrack*			track;
				SubtitleDecoder*		decoder;
			};

			void				_DemuxLoop();
			void				_VideoDecodeLoop();
			int					_HurryLevel();
			void				_PresentLoop();
			void				_SubtitlePrerollLoop();
			void				_RequestSubtitlePreroll(bigtime_t time);

			void				_PerformSeek();
			void				_RouteVideoPacket(AVPacket* packet);
			bool				_QueuesFull();
			void				_StartThreads();
			void				_StopThreads();
			void				_StartAudioThread();
			void				_StopAudioThread();
			bool				_CreateVideoDecoder();
			void				_FallBackToSoftware(const char* why,
								bigtime_t resumeTime = kNoTime);

			bigtime_t			_MasterClock(bool* valid);
			bigtime_t			_MasterClockPosition();
			bool				_Show(const VideoFramePtr& frame);
			bool				_ScanStep();
			void				_HandleEnd();
			bool				_MediaEnded();
			void				_Notify(uint32 what);
			void				_RequestSeekLocked(bigtime_t time,
									bool accurate, int direction);
			void				_UpdateClocksLocked();
			bool				_IsScanRate(double rate) const;
			void				_DecodeCoverArt(AVStream* stream);
			int					_ChooseAudioStream();
			void				_ChooseSubtitleTrack();

			BMessenger			fTarget;
			BString				fPath;
			AVFormatContext*	fFormat;
			bigtime_t			fStartTime;
			bigtime_t			fDuration;
			int64				fFileSize;

			// Streams
			int					fVideoStream;
			bigtime_t			fLastVideoPacketPts = kNoTime;
			int					fAudioStream;
			TrackInfo			fVideoTrack;
			std::vector<TrackInfo> fAudioTracks;
			std::vector<SubtitleSource> fSubtitles;
			int					fSubtitleSelected;
			BString				fPreferredAudio;
			BString				fPreferredSubtitles;
			std::vector<Chapter> fChapters;
			int					fVideoWidth;
			int					fVideoHeight;
			AVRational			fSampleAspect;
			caption_source		fCaptionSource;
			int					fCaptionLengthSize;
			int					fCaptionStream;		// a c608 track
			SubtitleTrack		fCaptionTrack;
			CaptionDecoder*		fCaptionDecoder;
			std::atomic<bool>	fCaptionsFound;
			std::atomic<bool>	fCaptionsEnabled;
			VideoFramePtr		fCoverFrame;

			// Queues and decoders
			PacketQueue			fVideoQueue;
			PacketQueue			fAudioQueue;
			FrameQueue			fFrames;
			std::mutex			fDecoderLock;
			VideoDecoder*		fVideoDecoder;
			BString				fHardwareNote;
			std::atomic<bool>	fHardwareDecoding;
			AudioDecoder*		fAudioDecoder;
			AudioOutput*		fAudioOutput;
			BString				fAudioError;
			Clock				fAudioClock;
			Clock				fExternalClock;

			// Threads
			std::thread			fDemuxThread;
			std::thread			fVideoThread;
			std::thread			fAudioThread;
			std::thread			fPresentThread;
			std::thread			fPrerollThread;
			std::atomic<bool>	fQuit;

			// Subtitles that began before a seek target are read again by a
			// reader of their own, from a little before it.
			std::mutex			fPrerollLock;
			std::condition_variable	fPrerollWake;
			bigtime_t			fPrerollTarget;
			bool				fPrerollRequested;

			// State, under fLock
			std::mutex			fLock;
			std::condition_variable	fWakeUp;
			bool				fPlaying;
			double				fRate;
			bool				fScanning;
			bool				fLooping;
			bool				fSeekRequested;
			bigtime_t			fSeekTarget;
			bool				fSeekAccurate;
			int					fSeekDirection;	// keyframe seeks: 1 or -1
			bigtime_t			fPendingPosition;	// until the seek shows
			bool				fPositionPending;
			bigtime_t			fSeekRequestedAt = 0;
			// The queue serials when the last seek was asked for: pictures
			// and sound from them or before belong to the old position.
			int					fSeekFromVideoSerial;
			int					fSeekFromAudioSerial;
			bool				fShowNextFrame;
			bool				fEndOfFile;
			bool				fEnded;
			int					fVideoSkipSerial;
			bigtime_t			fVideoSkipTime;
			int					fVideoFinishedSerial;
			bigtime_t			fLastShownPts;
			bigtime_t			fSerialStarted;	// when the last seek began
			bigtime_t			fScanPosition;
			bigtime_t			fScanStepTime;
			bool				fScanAwaitingFrame;
			bigtime_t			fScanShownPts;
			// Scanning reads one key frame per seek, then ends the stream so
			// the decoder lets go of it at once.
			bool				fScanPacketSent;
			bool				fScanHitEnd;
			float				fVolume;
			bool				fMuted;

			VideoSink*			fSink;
			std::atomic<int64>	fFramesShown;
			std::atomic<int64>	fFramesDropped;
			bigtime_t			fRateWindowStart;
			int64				fRateWindowFrames;
			double				fDisplayRate;
			bigtime_t			fLastAVOffset;
			std::atomic<bigtime_t> fDecodeTime;
			// How late pictures are when their turn comes, smoothed, and
			// how much the decoder leaves out to catch up.
			std::atomic<bigtime_t> fLateness;
			int					fHurry;
			bigtime_t			fHurryChanged;
			// How long pictures must be on time before less is left out;
			// doubled whenever that turned out too soon.
			bigtime_t			fHurryPatience;
			bigtime_t			fHurryEased;
			// What the threads are doing, for diagnosis.
			std::atomic<int>	fDemuxPhase;
			std::atomic<int>	fDecodePhase;
			std::atomic<int>	fPresentPhase;
			std::atomic<int>	fScanSeeks;
};


}	// namespace airtime

#endif	// AIRTIME_PLAYER_H
