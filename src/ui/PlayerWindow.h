/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_PLAYER_WINDOW_H
#define AIRTIME_PLAYER_WINDOW_H


#include <DirectWindow.h>
#include <Entry.h>
#include <Messenger.h>

#include "Player.h"
#include "SkipSegments.h"


class BButton;
class BGroupView;
class BListView;
class BSlider;
class BStringView;
class BFilePanel;
class BMenu;
class BMenuBar;
class BMenuItem;
class BMessageRunner;


namespace airtime {

class ControlBar;
class FrameView;
class VideoView;


class PlayerWindow : public BDirectWindow {
public:
								PlayerWindow(BRect frame);
	virtual						~PlayerWindow();

			status_t			OpenFile(const entry_ref& ref, bool play,
									BString* error);
			bool				IsEmpty() const { return !fHasFile; }
			bool				IsPlaying()
									{ return fHasFile && fPlayer->IsPlaying(); }
			bool				HasChapters() const
									{ return !fPlayer->Chapters().empty(); }
			void				SetFullScreen(bool fullScreen);
			void				SeekOnOpen(bigtime_t time)
									{ fSeekOnOpen = time; }

	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MenusBeginning();
	virtual	void				Zoom(BPoint origin, float width, float height);
	virtual	void				WindowActivated(bool active);
	virtual	void				DirectConnected(direct_buffer_info* info);

	virtual	BHandler*			ResolveSpecifier(BMessage* message, int32 index,
									BMessage* specifier, int32 what,
									const char* property);
	virtual	status_t			GetSupportedSuites(BMessage* data);

private:
			void				_BuildMenus();
			void				_UpdateMenus();
			void				_BuildTrackMenus();
			void				_BuildRecentMenu();
			void				_BuildChapterMenu();
			BMenu*				_BuildSpeedMenu(const char* name);
			void				_ShowContextMenu(BPoint where);

			void				_BuildMobileView();
			void				_SetMobileView(bool mobile);
			void				_ShowLibrary(bool show);
			void				_Layout();
			float				_MenuHeight() const;
			float				_ControlsHeight() const;
			BRect				_ScreenFrame() const;
			void				_ResizeToVideo(float scale);
			void				_SnapToAspect();
			void				_ApplySizeLimits();
			void				_SetFullScreen(bool fullScreen);
			void				_ShowHud(bool show);

			void				_Pulse();
			void				_ApplyDevicePixels();
			void				_UpdateControls();
			void				_UpdateInspector();
			void				_ShowMessage(const char* format, ...);

			void				_HandleScanPress(int direction);
			void				_HandleScanRelease(int direction,
									bigtime_t held);
			void				_HandleJKL(int key);
			void				_StepSpeed(int direction);
			void				_SetVolume(float volume);
			void				_HandleClick(int32 clicks);
			void				_HandleDrop(BMessage* message);
			void				_AddSubtitleFile(const entry_ref& ref);
			void				_CycleSubtitles();
			void				_CycleAudio();
			void				_GoToChapter(int step);
			void				_ApplyPreferences();
			bool				_HandleScripting(BMessage* message);
			void				_LookUpSkipSegments();
			void				_UpdateSkipButton();
			void				_Skip();

			Player*				fPlayer;
			entry_ref			fRef;
			bool				fHasFile;
			bigtime_t			fSeekOnOpen;

			BMenuBar*			fMenuBar;
			BMenu*				fFileMenu;
			BMenu*				fRecentMenu;
			BMenu*				fViewMenu;
			BMenu*				fPlaybackMenu;
			BMenu*				fChapterMenu;
			BMenu*				fSpeedMenu;
			BMenu*				fAudioMenu;
			BMenu*				fSubtitleMenu;
			BMenu*				fSubtitleSizeMenu;
			BMenuItem*			fPlayItem;

			bool				fMobile = false;
			bool				fDesktopFullScreen = false;
			BRect				fDesktopVideoFrame;
			BGroupView*			fMobileHeader;
			BGroupView*			fMobileControls;
			BGroupView*			fLibrary;
			BButton*			fBrowse;
			BButton*			fPlay;
			BSlider*			fTimeline;
			BListView*			fRecent;
			BStringView*		fLibraryHeading;
			std::vector<entry_ref> fRecentRefs;

			FrameView*			fFrame;
			VideoView*			fVideo;
			ControlBar*			fControls;
			ControlBar*			fHud;

			BMessageRunner*		fPulseRunner;
			int32				fPulseCount;
			bool				fFullScreen;
			BRect				fSavedFrame;
			bool				fHudShown;
			bigtime_t			fLastActivity;
			bigtime_t			fScreenSaverKicked;
			bool				fAskedForPerformance;
			bigtime_t			fCreated;
			bool				fDaemonKilled;
			bool				fAlwaysOnTop;

			int32				fIgnoreResize;
			bool				fResizePending;
			bigtime_t			fResizedAt;

			bool				fScanHeld;
			int					fScanDirection;
			bigtime_t			fScanPressedAt;
			bool				fScanWasPlaying;
			bool				fScanWasScanning;
			double				fScanStartRate;

			bool				fScrubbing;
			bool				fScrubWasPlaying;

			BMessenger			fInspector;
			BFilePanel*			fSubtitlePanel;
			bool				fWasPlayingBeforeEnd;

			// When the intro and credits are (web lookup, kMsgSkipSegmentsFound)
			// and which of them the button offers to skip now.
			SkipSegments		fSkipSegments;
			int32				fSkipGeneration;
			SkipKind			fSkipShown;
			bigtime_t			fSkipTarget;
};

}	// namespace airtime

#endif	// AIRTIME_PLAYER_WINDOW_H
