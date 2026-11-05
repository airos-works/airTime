/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_VIDEO_VIEW_H
#define AIRTIME_VIDEO_VIEW_H


#include <atomic>
#include <mutex>

#include <DirectWindow.h>
#include <Messenger.h>
#include <String.h>
#include <View.h>

#include "FrameRenderer.h"
#include "Player.h"
#include "SubtitleRenderer.h"


class BBitmap;


namespace airtime {

/*!	Where the film is shown. Pictures arrive on the player's presentation
	thread, are converted to the size they are shown at, get their subtitles
	drawn in, and are put on screen from there; Draw() only repaints what is
	already made (and, while paused, makes it again at a new size). */
class VideoView : public BView, public VideoSink {
public:
								VideoView(BRect frame,
									const BMessenger& target);
	virtual						~VideoView();

			void				SetPlayer(Player* player);
			void				SetAudioOnly(bool audioOnly);
			void				SetInfo(const char* title,
									const char* detail);
			void				SetSubtitleScale(float scale);
			void				ShowMessage(const char* text);
			// A button over the picture's bottom right corner ("Skip
			// Intro"); clicking it sends kMsgSkipSegment. NULL or "" hides it.
			void				SetSkipButton(const char* label);
			// The button stays above this line (the full screen controller's
			// top, whether it is shown or not); -1 for none.
			void				SetSkipButtonFloor(float top);
			// Draw the last picture again (subtitles or size changed).
			void				Refresh();
			void				SetFullScreen(bool fullScreen);
			// The part of the view covered by the full screen controller.
			void				SetCoveredBottom(float top);

			// From the window's DirectConnected(): where the frame buffer
			// is and which parts of the window are visible in it.
			void				DirectConnected(direct_buffer_info* info);
			void				SetDirectAllowed(bool allowed);
			// Where the view sits in its window; called after layout.
			void				UpdateWindowOrigin();
			bool				DrawsDirectly() const { return fDirectUsed; }
			BRect				VideoFrame();

	// VideoSink, on the presentation thread
	virtual	bool				DisplayFrame(const VideoFramePtr& frame);
	virtual	void				GetTimings(bigtime_t* compose,
									bigtime_t* draw);

	// BView
	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				KeyDown(const char* bytes, int32 count);
	virtual	void				MessageReceived(BMessage* message);

private:
			bool				_DrawDirect(BBitmap* bitmap, BRect rect);
			bool				_RenderDirect(const VideoFramePtr& frame,
									BRect rect, int width, int height);
			float				_DirectScale();
			bool				_DirectStillConnected();
			static void			_DeviceRect(BRect rect, BPoint origin,
									float scale, int* left, int* top,
									int* width, int* height);
			BRect				_VideoRectFor(BRect bounds) const;
			bool				_Compose(const VideoFramePtr& frame,
									int index, int width, int height);
			void				_DrawEmpty(BRect updateRect);
			void				_DrawAudioOnly(BRect updateRect);
			void				_Post(uint32 what);
			bool				_SkipButtonShown();
			bool				_InSkipButton(BPoint where);
			void				_ComposeSkipButton(uint8* bits,
									int32 bytesPerRow, int width,
									int height);

			BMessenger			fTarget;
			Player*				fPlayer;

			std::mutex			fGeometryLock;
			BRect				fVideoRect;

			std::mutex			fRenderLock;
			FrameRenderer		fRenderer;
			SubtitleRenderer	fSubtitles;
			BBitmap*			fBitmaps[2];
			std::atomic<int>	fCurrent;
			VideoFramePtr		fLastFrame;
			std::atomic<bool>	fDirty;

			std::mutex			fMessageLock;
			BString				fMessage;
			bigtime_t			fMessageUntil;

			// The skip button, guarded by fMessageLock; its overlay is made
			// with fRenderLock held.
			BString				fSkipLabel;
			bool				fSkipHighlighted;
			BRect				fSkipFrame;		// in view coordinates
			bigtime_t			fSkipClicked;
			Overlay				fSkipOverlay;
			BString				fSkipOverlayLabel;
			bool				fSkipOverlayHighlighted;
			int					fSkipOverlayWidth;
			int					fSkipOverlayHeight;
			int					fSkipOverlayInset;
			float				fSkipFloor;

			bool				fAudioOnly;
			bool				fFullScreen;
			BString				fTitle;
			BString				fDetail;
			BPoint				fLastMouse;
			std::atomic<bigtime_t> fComposeTime;
			bigtime_t			fTraceRender = 0;
			bigtime_t			fTraceOverlays = 0;
			std::atomic<bigtime_t> fDrawTime;

			// Direct frame buffer access, guarded by fDirectLock; the window
			// waits for a copy in progress before the buffer goes away.
			std::timed_mutex	fDirectLock;
			// DirectConnected() could not wait for the lock: what is known
			// of the clipping may be out of date until the next one.
			std::atomic<bool>	fDirectStale;
			bool				fDirectConnected;
			std::atomic<bool>	fDirectAllowed;
			std::atomic<bool>	fDirectUsed;
			bool				fCovered;
			uint8*				fDirectBits;
			int32				fDirectBytesPerRow;
			clipping_rect		fDirectWindowBounds;
			std::vector<clipping_rect> fDirectClips;
			BPoint				fWindowOrigin;
			// On a desktop drawn at a higher density (B_DIRECT_DEVICE_PIXELS)
			// the window is placed and clipped in frame buffer pixels, the
			// picture is made at that density, and app_server's own copy of
			// the screen has to be drawn into as well as the frame buffer.
			float				fDirectScale;
			area_id				fDrawingSource;
			area_id				fDrawingClone;
			uint8*				fDrawingBits;
			int32				fDrawingBytesPerRow;
			// The thread that calls DirectConnected(): BDirectWindow's
			// daemon. If it is gone, the app_server has ended the access
			// without a stop.
			thread_id			fDirectDaemon;
};

}	// namespace airtime

#endif	// AIRTIME_VIDEO_VIEW_H
