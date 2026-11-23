/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_TOP_BAR_H
#define AIRTIME_TOP_BAR_H


#include <Messenger.h>
#include <String.h>
#include <View.h>

#include "Look.h"


namespace airtime {

/*!	The bar over the top of the picture in full screen and in the mobile
	view, in the full screen controller's dark glass: Open… on the left, the
	film's title in the middle, the audio and subtitle pickers and the CC
	switch on the right. Like the controller it only reports what the user
	did, as messages to its target: kMsgOpenFile, kMsgShowAudioTracks and
	kMsgShowSubtitleTracks (with the button's screen frame as "button"), and
	kMsgToggleSubtitlesShown. */
class TopBar : public BView {
public:
								TopBar(BRect frame, const BMessenger& target);

	static	float				PreferredHeight() { return 46; }
			float				Height() const;

			void				SetScale(float scale);
			void				SetTitle(const char* title);
			// How many audio tracks there are, how many ways to show
			// subtitles (tracks and closed captions), and whether some are
			// shown now.
			void				SetTracks(bool hasMedia, int audioTracks,
									int subtitleChoices, bool subtitlesShown);
			bool				IsTracking() const { return fPressed != PART_NONE; }

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);

private:
			enum part {
				PART_NONE,
				PART_OPEN,
				PART_AUDIO,
				PART_SUBTITLES,
				PART_CAPTIONS,
				PART_COUNT
			};

			void				_Layout();
			part				_PartAt(BPoint where) const;
			bool				_Enabled(part which) const;
			button_state		_StateOf(part which) const;
			void				_DrawButton(part which);
			void				_Invoke(part which);
			void				_Activity();

			BMessenger			fTarget;
			float				fScale;
			BString				fTitle;
			bool				fHasMedia;
			int					fAudioTracks;
			int					fSubtitleChoices;
			bool				fSubtitlesShown;
			bool				fLabels;

			BRect				fFrames[PART_COUNT];
			BRect				fTitleFrame;
			part				fPressed;
			part				fHover;
			bool				fInside;
};


/*!	One large button in the same glass, alone in the middle of the empty
	picture of the mobile view: "Open…". */
class HudButton : public BView {
public:
								HudButton(BRect frame, const char* label,
									glyph_kind glyph, BMessage* message,
									const BMessenger& target);
	virtual						~HudButton();

			BSize				PreferredSize(float scale) const;

	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);

private:
			BFont				_Font() const;

			BString				fLabel;
			glyph_kind			fGlyph;
			BMessage*			fMessage;
			BMessenger			fTarget;
			bool				fPressed;
			bool				fInside;
			bool				fHover;
};

}	// namespace airtime

#endif	// AIRTIME_TOP_BAR_H
