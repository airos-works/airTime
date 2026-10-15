/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * airTime's Quick Look add-on: Tracker's space bar plays films and songs
 * with airTime's own engine (hardware decoding included) in the Quick Look
 * panel, so that the player exists once. Built from the same sources as
 * the application and shipped in its package.
 */


#include <math.h>
#include <stdio.h>

#include <MessageRunner.h>
#include <Path.h>
#include <Roster.h>
#include <Window.h>

#include <add-ons/quick_look/QuickLook.h>

#include "AppInfo.h"
#include "ControlBar.h"
#include "Messages.h"
#include "Metal.h"
#include "Player.h"
#include "Settings.h"
#include "VideoView.h"


using namespace airtime;


static const float kBorder = 8;
static const bigtime_t kPulseInterval = 100000;
static const bigtime_t kSkipTime = 10 * 1000000LL;


class MoviePreview : public BQuickLookPreview {
public:
	MoviePreview(const entry_ref& ref)
		:
		BQuickLookPreview("airTime preview", B_WILL_DRAW | B_FRAME_EVENTS
			| B_FULL_UPDATE_ON_RESIZE),
		fRef(ref),
		fPlayer(NULL),
		fVideo(NULL),
		fControls(NULL),
		fPulse(NULL),
		fStarted(false),
		fScrubbing(false),
		fScrubWasPlaying(false),
		fScanHeld(false),
		fScanWasPlaying(false)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		fPlayer = new Player(BMessenger());
		fPlayer->SetHardwareDecoding(settings().hardwareDecoding);
		fPlayer->SetVolume(settings().volume);
		fPlayer->SetMuted(settings().muted);
	}

	virtual ~MoviePreview()
	{
		Stop();
		delete fPulse;
		delete fPlayer;
	}

	status_t Open(BString* error)
	{
		BPath path(&fRef);
		if (path.InitCheck() != B_OK) {
			*error = "The file could not be found.";
			return B_ENTRY_NOT_FOUND;
		}
		fPlayer->SetPreferredLanguages(settings().audioLanguage.String(),
			settings().subtitleLanguage.String());
		return fPlayer->Open(path.Path(), error);
	}

	// BQuickLookPreview

	virtual BSize PreferredContentSize()
	{
		float controls = ControlBar::PreferredHeight(false);
		if (fPlayer->HasVideo()) {
			double aspect = fPlayer->DisplayAspect();
			float height = fPlayer->VideoHeight();
			float width = aspect > 0 ? floorf(height * aspect)
				: fPlayer->VideoWidth();
			if (!fPlayer->HasMovingVideo()) {
				// Cover art of a song: not larger than a window's worth.
				float scale = std::min(1.0f, 480 / std::max(width, height));
				width = floorf(width * scale);
				height = floorf(height * scale);
			}
			return BSize(std::max(width, ControlBar::MinimumWidth(false))
				+ 2 * kBorder, height + kBorder + controls);
		}
		return BSize(std::max(520.0f, ControlBar::MinimumWidth(false)),
			150 + controls);
	}

	virtual BString Summary()
	{
		BString summary;
		if (fPlayer->HasMovingVideo()) {
			const TrackInfo& track = fPlayer->VideoTrack();
			summary << track.codec;
			if (summary.Length() > 0)
				summary << " ";
			summary << fPlayer->VideoWidth() << " \xC3\x97 "
				<< fPlayer->VideoHeight();
			if (track.frameRate > 0) {
				BString rate;
				rate.SetToFormat(" %.4g fps", track.frameRate);
				summary << rate;
			}
			if (fPlayer->VideoDecoderIsHardware()) {
				summary << " \xE2\x80\xA2 " << "hardware decoded ("
					<< fPlayer->VideoDecoderName() << ")";
			} else {
				summary << " \xE2\x80\xA2 " << "software decoded";
			}
		} else if (fPlayer->HasAudio()) {
			const std::vector<TrackInfo>& tracks = fPlayer->AudioTracks();
			int current = fPlayer->CurrentAudioTrack();
			if (current >= 0 && current < (int)tracks.size()) {
				const TrackInfo& track = tracks[current];
				summary << track.codec;
				if (track.channels > 0) {
					summary << " \xE2\x80\xA2 " << track.channels
						<< (track.channels == 1 ? " channel" : " channels");
				}
				if (track.sampleRate > 0) {
					BString rate;
					rate.SetToFormat(" \xE2\x80\xA2 %.1f kHz",
						track.sampleRate / 1000.0);
					summary << rate;
				}
				if (track.bitRate > 0) {
					BString rate;
					rate.SetToFormat(" \xE2\x80\xA2 %d kbit/s",
						(int)(track.bitRate / 1000));
					summary << rate;
				}
			}
		}
		bigtime_t duration = fPlayer->Duration();
		if (duration > 0) {
			int seconds = (int)(duration / 1000000);
			BString time;
			if (seconds >= 3600) {
				time.SetToFormat(" \xE2\x80\xA2 %d:%02d:%02d", seconds / 3600,
					(seconds / 60) % 60, seconds % 60);
			} else
				time.SetToFormat(" \xE2\x80\xA2 %d:%02d", seconds / 60, seconds % 60);
			summary << time;
		}
		return summary;
	}

	virtual void Start()
	{
		if (fStarted || fVideo == NULL)
			return;
		fStarted = true;
		fPlayer->Play();
		_UpdateControls();
	}

	virtual void Stop()
	{
		if (!fStarted)
			return;
		fStarted = false;
		fPlayer->Pause();
		fPlayer->SetVideoSink(NULL);
		fPlayer->Close();
		if (fVideo != NULL)
			fVideo->SetPlayer(NULL);
	}

	virtual void DirectConnected(direct_buffer_info* info)
	{
		if (fVideo != NULL)
			fVideo->DirectConnected(info);
	}

	// BView

	virtual void AttachedToWindow()
	{
		BQuickLookPreview::AttachedToWindow();
		// The views and the player's messenger need the window's looper.
		fPlayer->SetTarget(BMessenger(this));
		BRect bounds = Bounds();
		float controlsHeight = ControlBar::PreferredHeight(false);
		fVideo = new VideoView(BRect(kBorder, kBorder, bounds.right - kBorder,
			bounds.bottom - controlsHeight), BMessenger(this));
		AddChild(fVideo);
		fControls = new ControlBar(BRect(0, bounds.bottom - controlsHeight + 1,
			bounds.right, bounds.bottom), false, BMessenger(this));
		AddChild(fControls);

		fVideo->SetPlayer(fPlayer);
		fVideo->SetSubtitleScale(settings().subtitleScale);
		fPlayer->SetVideoSink(fVideo);

		bool hasVideo = fPlayer->HasVideo();
		fVideo->SetAudioOnly(!hasVideo);
		BString title = fPlayer->Metadata("title");
		if (title.Length() == 0)
			title = fRef.name;
		BString detail = fPlayer->Metadata("artist");
		BString album = fPlayer->Metadata("album");
		if (album.Length() > 0) {
			if (detail.Length() > 0)
				detail << " \xE2\x80\x94 ";
			detail << album;
		}
		fVideo->SetInfo(title.String(), detail.String());
		fControls->SetMedia(true, fPlayer->HasMovingVideo(),
			fPlayer->HasAudio());
		fControls->SetVolume(settings().volume, settings().muted);
		fControls->SetTimeDisplay(settings().timeDisplay);
		fControls->SetTitle(title.String());
		std::vector<bigtime_t> chapters;
		for (size_t i = 0; i < fPlayer->Chapters().size(); i++)
			chapters.push_back(fPlayer->Chapters()[i].start);
		fControls->SetChapters(chapters);

		_Layout();
		BMessage pulse(kMsgPulse);
		fPulse = new BMessageRunner(BMessenger(this), &pulse, kPulseInterval);
	}

	virtual void DetachedFromWindow()
	{
		delete fPulse;
		fPulse = NULL;
		BQuickLookPreview::DetachedFromWindow();
	}

	virtual void FrameResized(float width, float height)
	{
		BQuickLookPreview::FrameResized(width, height);
		_Layout();
	}

	virtual void FrameMoved(BPoint origin)
	{
		BQuickLookPreview::FrameMoved(origin);
		if (fVideo != NULL)
			fVideo->UpdateWindowOrigin();
	}

	virtual void Draw(BRect updateRect)
	{
		metal::fill(this, updateRect);
		metal::sheen(this, updateRect, Bounds());
		if (fVideo != NULL && fPlayer->HasVideo())
			metal::inset_frame(this, fVideo->Frame());
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgPulse:
				_UpdateControls();
				break;

			case kMsgPlayerStateChanged:
			case kMsgPlayerEnded:
				_UpdateControls();
				break;
			case kMsgPlayerDecoderChanged:
				NotifyChanged();
				break;
			case kMsgPlayerError:
			{
				const char* error;
				if (message->FindString("error", &error) == B_OK
					&& fVideo != NULL)
					fVideo->ShowMessage(error);
				break;
			}

			case kMsgTogglePlay:
				fPlayer->TogglePlaying();
				_UpdateControls();
				break;
			case kMsgPlay:
				fPlayer->Play();
				break;
			case kMsgPause:
				fPlayer->Pause();
				break;
			case kMsgGoToStart:
				fPlayer->Seek(0);
				break;
			case kMsgGoToEnd:
				fPlayer->Seek(fPlayer->Duration());
				break;
			case kMsgSeekBy:
				fPlayer->Seek(std::max((bigtime_t)0, fPlayer->Position()
					+ message->GetInt64("delta", 0)));
				break;

			case kMsgScanPress:
				_HandleScanPress(message->GetInt32("direction", 1));
				break;
			case kMsgScanRelease:
				_HandleScanRelease(message->GetInt32("direction", 1),
					message->GetInt64("held", 0));
				break;

			case kMsgSeekTo:
			{
				bigtime_t time = message->GetInt64("time", 0);
				bool final = message->GetBool("final", true);
				if (!final) {
					if (!fScrubbing) {
						fScrubbing = true;
						fScrubWasPlaying = fPlayer->IsPlaying()
							&& !fPlayer->IsScanning();
						if (fScrubWasPlaying)
							fPlayer->Pause();
					}
					fPlayer->Seek(time, false);
				} else {
					fPlayer->Seek(time, true);
					if (fScrubbing && fScrubWasPlaying)
						fPlayer->Play();
					fScrubbing = false;
				}
				break;
			}

			case kMsgSetVolume:
			{
				float volume = message->GetFloat("volume", 1.0f);
				fPlayer->SetVolume(volume);
				fPlayer->SetMuted(false);
				fControls->SetVolume(volume, false);
				if (message->GetBool("final", true)) {
					settings().volume = volume;
					settings().muted = false;
					settings().Save();
				}
				break;
			}
			case kMsgToggleMute:
			{
				bool muted = !settings().muted;
				settings().muted = muted;
				settings().Save();
				fPlayer->SetMuted(muted);
				fControls->SetVolume(settings().volume, muted);
				break;
			}

			case kMsgVideoClicked:
				if (message->GetInt32("clicks", 1) >= 2)
					_OpenInAirTime();
				else {
					fPlayer->TogglePlaying();
					_UpdateControls();
				}
				break;

			case kMsgToggleFullScreen:
				// The panel has no full screen: that is what airTime is for.
				_OpenInAirTime();
				break;

			case kMsgControlsActivity:
			case kMsgToggleTimeDisplay:
				break;

			default:
				BQuickLookPreview::MessageReceived(message);
		}
	}

private:
	void _Layout()
	{
		if (fVideo == NULL)
			return;
		BRect bounds = Bounds();
		float controlsHeight = ControlBar::PreferredHeight(false);
		fControls->MoveTo(0, bounds.bottom - controlsHeight + 1);
		fControls->ResizeTo(bounds.Width(), controlsHeight - 1);
		BRect video;
		if (fPlayer->HasVideo()) {
			video.Set(kBorder, kBorder, bounds.right - kBorder,
				bounds.bottom - controlsHeight);
		} else
			video.Set(0, 0, bounds.right, bounds.bottom - controlsHeight);
		fVideo->MoveTo(video.LeftTop());
		fVideo->ResizeTo(video.Width(), video.Height());
		fVideo->UpdateWindowOrigin();
		Invalidate();
	}

	void _UpdateControls()
	{
		if (fControls == NULL || !fPlayer->IsOpen())
			return;
		if (!fScrubbing)
			fControls->SetPosition(fPlayer->Position(), fPlayer->Duration());
		fControls->SetPlaying(fPlayer->IsPlaying());
		fControls->SetRate(fPlayer->Rate(), fPlayer->IsScanning());
	}

	void _HandleScanPress(int direction)
	{
		fScanHeld = true;
		fScanWasPlaying = fPlayer->IsPlaying() && !fPlayer->IsScanning();
		fPlayer->SetRate(2.0 * direction);
		if (!fPlayer->IsPlaying())
			fPlayer->Play();
	}

	void _HandleScanRelease(int direction, bigtime_t held)
	{
		if (!fScanHeld)
			return;
		fScanHeld = false;
		fPlayer->SetRate(1.0);
		if (held < 300000) {
			// A click skips ten seconds.
			bigtime_t target = fPlayer->Position() + direction * kSkipTime;
			fPlayer->Seek(std::max((bigtime_t)0,
				std::min(target, fPlayer->Duration())));
		}
		if (fScanWasPlaying)
			fPlayer->Play();
		else
			fPlayer->Pause();
	}

	void _OpenInAirTime()
	{
		entry_ref ref = fRef;
		BMessage refs(B_REFS_RECEIVED);
		refs.AddRef("refs", &ref);
		be_roster->Launch(kAppSignature, &refs);
		if (Window() != NULL)
			Window()->PostMessage(B_QUIT_REQUESTED);
	}

	entry_ref		fRef;
	Player*			fPlayer;
	VideoView*		fVideo;
	ControlBar*		fControls;
	BMessageRunner*	fPulse;
	bool			fStarted;
	bool			fScrubbing;
	bool			fScrubWasPlaying;
	bool			fScanHeld;
	bool			fScanWasPlaying;
};


class AirTimeAddOn : public BQuickLookAddOn {
public:
	AirTimeAddOn(image_id image)
		:
		BQuickLookAddOn(image)
	{
		settings().Load();
	}

	virtual float Supports(const entry_ref& ref, const char* mimeType)
	{
		// Films first; songs when nothing more at home with them is there.
		if (strncmp(mimeType, "video/", 6) == 0)
			return 0.9f;
		if (strncmp(mimeType, "audio/", 6) == 0)
			return 0.6f;
		if (strcmp(mimeType, "application/ogg") == 0
			|| strcmp(mimeType, "application/x-matroska") == 0
			|| strcmp(mimeType, "application/mp4") == 0
			|| strcmp(mimeType, "application/vnd.rn-realmedia") == 0
			|| strcmp(mimeType, "application/x-mpegURL") == 0)
			return 0.7f;
		return 0;
	}

	virtual BQuickLookPreview* CreatePreview(const entry_ref& ref,
		const char* mimeType, BString* error)
	{
		MoviePreview* preview = new MoviePreview(ref);
		if (preview->Open(error) != B_OK) {
			delete preview;
			return NULL;
		}
		return preview;
	}
};


extern "C" BQuickLookAddOn*
instantiate_quick_look_add_on(image_id image)
{
	return new AirTimeAddOn(image);
}
