/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "TopBar.h"

#include <math.h>

#include <Font.h>
#include <Message.h>
#include <Window.h>

#include "Messages.h"


namespace airtime {


static const char* kLabels[] = {NULL, "Open…", "Audio", "Subtitles", "CC"};


static rgb_color
text_color(button_state state)
{
	switch (state) {
		case BUTTON_HOVER:		return look::color(255, 255, 255);
		case BUTTON_PRESSED:	return look::color(150, 170, 200);
		case BUTTON_DISABLED:	return look::color(110, 110, 114);
		default:				return look::color(232, 232, 236);
	}
}


TopBar::TopBar(BRect frame, const BMessenger& target)
	:
	BView(frame, "top bar", B_FOLLOW_NONE,
		B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fTarget(target),
	fScale(1.0f),
	fHasMedia(false),
	fAudioTracks(0),
	fSubtitleChoices(0),
	fSubtitlesShown(false),
	fLabels(true),
	fPressed(PART_NONE),
	fHover(PART_NONE),
	fInside(false)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	_Layout();
}


float
TopBar::Height() const
{
	return roundf(PreferredHeight() * fScale);
}


void
TopBar::SetScale(float scale)
{
	if (scale == fScale)
		return;
	fScale = scale;
	_Layout();
	Invalidate();
}


void
TopBar::SetTitle(const char* title)
{
	if (fTitle == title)
		return;
	fTitle = title;
	Invalidate(fTitleFrame);
}


void
TopBar::SetTracks(bool hasMedia, int audioTracks, int subtitleChoices,
	bool subtitlesShown)
{
	if (hasMedia == fHasMedia && audioTracks == fAudioTracks
		&& subtitleChoices == fSubtitleChoices
		&& subtitlesShown == fSubtitlesShown) {
		return;
	}
	fHasMedia = hasMedia;
	fAudioTracks = audioTracks;
	fSubtitleChoices = subtitleChoices;
	fSubtitlesShown = subtitlesShown;
	Invalidate();
}


void
TopBar::AttachedToWindow()
{
	BView::AttachedToWindow();
	_Layout();
}


void
TopBar::FrameResized(float width, float height)
{
	_Layout();
	Invalidate();
}


void
TopBar::_Layout()
{
	BRect bounds = Bounds();
	const float s = fScale;
	float height = roundf(30 * s);
	float top = roundf((bounds.Height() - height) / 2);
	float margin = roundf(10 * s);
	float gap = roundf(6 * s);
	float glyph = roundf(18 * s);
	float padding = roundf(10 * s);

	BFont font(be_bold_font);
	font.SetSize(12 * s);
	auto width = [&](part which, bool labels) {
		if (which == PART_CAPTIONS)
			return roundf(font.StringWidth("CC") + 2 * padding + 8 * s);
		if (!labels)
			return height + 6 * s;
		return roundf(glyph + 6 * s + font.StringWidth(kLabels[which])
			+ 2 * padding);
	};

	// Labels when there is room for them and some of the title.
	fLabels = true;
	float needed = 2 * margin + 4 * gap + 120 * s;
	for (int which = PART_OPEN; which < PART_COUNT; which++)
		needed += width((part)which, true);
	if (needed > bounds.Width())
		fLabels = false;

	float x = margin;
	fFrames[PART_OPEN].Set(x, top, x + width(PART_OPEN, fLabels), top + height);
	float right = bounds.right - margin;
	for (int which = PART_CAPTIONS; which >= PART_AUDIO; which--) {
		float w = width((part)which, fLabels);
		fFrames[which].Set(right - w, top, right, top + height);
		right -= w + gap;
	}
	fTitleFrame.Set(fFrames[PART_OPEN].right + 2 * gap, bounds.top + 2,
		right - gap, bounds.bottom - 2);
}


TopBar::part
TopBar::_PartAt(BPoint where) const
{
	for (int which = PART_OPEN; which < PART_COUNT; which++) {
		// A little more than the button, for fingers.
		if (fFrames[which].InsetByCopy(-3 * fScale, -6 * fScale)
				.Contains(where)) {
			return (part)which;
		}
	}
	return PART_NONE;
}


bool
TopBar::_Enabled(part which) const
{
	switch (which) {
		case PART_OPEN:			return true;
		case PART_AUDIO:		return fHasMedia && fAudioTracks > 0;
		case PART_SUBTITLES:	return fHasMedia;
		case PART_CAPTIONS:		return fHasMedia && fSubtitleChoices > 0;
		default:				return false;
	}
}


button_state
TopBar::_StateOf(part which) const
{
	if (!_Enabled(which))
		return BUTTON_DISABLED;
	if (fPressed == which && fInside)
		return BUTTON_PRESSED;
	if (fHover == which && fPressed == PART_NONE)
		return BUTTON_HOVER;
	return BUTTON_NORMAL;
}


void
TopBar::Draw(BRect updateRect)
{
	SetHighColor(0, 0, 0);
	FillRect(updateRect);
	look::hud_panel(this, Bounds().InsetByCopy(1, 1));

	for (int which = PART_OPEN; which < PART_COUNT; which++) {
		if (updateRect.Intersects(fFrames[which].InsetByCopy(-2, -2)))
			_DrawButton((part)which);
	}

	if (fTitle.Length() > 0 && fTitleFrame.Width() > 30
		&& updateRect.Intersects(fTitleFrame)) {
		BFont font(be_bold_font);
		font.SetSize(13 * fScale);
		SetFont(&font);
		BString title(fTitle);
		font.TruncateString(&title, B_TRUNCATE_MIDDLE, fTitleFrame.Width());
		font_height height;
		font.GetHeight(&height);
		float baseline = roundf((fTitleFrame.top + fTitleFrame.bottom) / 2
			+ (height.ascent - height.descent) / 2);
		PushState();
		SetDrawingMode(B_OP_OVER);
		SetHighColor(look::color(210, 210, 216));
		DrawString(title.String(), BPoint(roundf((fTitleFrame.left
			+ fTitleFrame.right - font.StringWidth(title.String())) / 2),
			baseline));
		PopState();
	}
}


void
TopBar::_DrawButton(part which)
{
	BRect frame = fFrames[which];
	button_state state = _StateOf(which);
	rgb_color color = text_color(state);
	float radius = 6 * fScale;

	PushState();
	SetDrawingMode(B_OP_ALPHA);
	SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	if (state == BUTTON_HOVER || state == BUTTON_PRESSED) {
		SetHighColor(255, 255, 255, state == BUTTON_PRESSED ? 50 : 28);
		FillRoundRect(frame, radius, radius);
	}

	BFont font(be_bold_font);
	font.SetSize(12 * fScale);
	SetFont(&font);
	font_height height;
	font.GetHeight(&height);
	float baseline = roundf((frame.top + frame.bottom) / 2
		+ (height.ascent - height.descent) / 2);

	if (which == PART_CAPTIONS) {
		// A CC badge: outlined when off, lit when subtitles are shown.
		BRect badge = frame.InsetByCopy(5 * fScale, 6 * fScale);
		bool on = fSubtitlesShown && state != BUTTON_DISABLED;
		float badgeRadius = 4 * fScale;
		if (on) {
			SetHighColor(color);
			FillRoundRect(badge, badgeRadius, badgeRadius);
		} else {
			SetPenSize(fmaxf(1.0f, roundf(1.5f * fScale)));
			SetHighColor(color);
			StrokeRoundRect(badge, badgeRadius, badgeRadius);
		}
		SetDrawingMode(B_OP_OVER);
		SetHighColor(on ? look::color(24, 24, 28) : color);
		const char* label = kLabels[which];
		DrawString(label, BPoint(roundf((frame.left + frame.right
			- font.StringWidth(label)) / 2), baseline));
		PopState();
		return;
	}

	glyph_kind kind = which == PART_OPEN ? GLYPH_OPEN
		: which == PART_AUDIO ? GLYPH_AUDIO_TRACK : GLYPH_SUBTITLES;
	float glyphSize = roundf(18 * fScale);
	float x = fLabels ? frame.left + roundf(10 * fScale)
		: roundf((frame.left + frame.right - glyphSize) / 2);
	float y = roundf((frame.top + frame.bottom - glyphSize) / 2);
	look::glyph(this, kind, BRect(x, y, x + glyphSize, y + glyphSize), color);
	if (fLabels) {
		SetDrawingMode(B_OP_OVER);
		SetHighColor(color);
		DrawString(kLabels[which], BPoint(x + glyphSize + roundf(6 * fScale),
			baseline));
	}
	PopState();
}


void
TopBar::_Activity()
{
	BMessage activity(kMsgControlsActivity);
	fTarget.SendMessage(&activity);
}


void
TopBar::_Invoke(part which)
{
	BMessage message;
	switch (which) {
		case PART_OPEN:
			// Another film for this window, in place of the one playing.
			message.what = kMsgOpenFile;
			message.AddBool("replace", true);
			break;
		case PART_AUDIO:
			message.what = kMsgShowAudioTracks;
			break;
		case PART_SUBTITLES:
			message.what = kMsgShowSubtitleTracks;
			break;
		case PART_CAPTIONS:
			message.what = kMsgToggleSubtitlesShown;
			break;
		default:
			return;
	}
	message.AddRect("button", ConvertToScreen(fFrames[which]));
	fTarget.SendMessage(&message);
}


void
TopBar::MouseDown(BPoint where)
{
	_Activity();
	part which = _PartAt(where);
	if (which == PART_NONE || !_Enabled(which))
		return;
	fPressed = which;
	fInside = true;
	SetMouseEventMask(B_POINTER_EVENTS,
		B_LOCK_WINDOW_FOCUS | B_NO_POINTER_HISTORY);
	Invalidate(fFrames[which].InsetByCopy(-2, -2));
}


void
TopBar::MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage)
{
	_Activity();
	if (fPressed != PART_NONE) {
		bool inside = _PartAt(where) == fPressed;
		if (inside != fInside) {
			fInside = inside;
			Invalidate(fFrames[fPressed].InsetByCopy(-2, -2));
		}
		return;
	}
	part hover = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW
		? PART_NONE : _PartAt(where);
	if (hover != fHover) {
		if (fHover != PART_NONE)
			Invalidate(fFrames[fHover].InsetByCopy(-2, -2));
		fHover = hover;
		if (hover != PART_NONE)
			Invalidate(fFrames[hover].InsetByCopy(-2, -2));
	}
}


void
TopBar::MouseUp(BPoint where)
{
	part which = fPressed;
	bool inside = fInside && _PartAt(where) == which;
	fPressed = PART_NONE;
	fInside = false;
	if (which == PART_NONE)
		return;
	Invalidate(fFrames[which].InsetByCopy(-2, -2));
	if (inside)
		_Invoke(which);
}


// #pragma mark - HudButton


HudButton::HudButton(BRect frame, const char* label, glyph_kind glyph,
	BMessage* message, const BMessenger& target)
	:
	BView(frame, label, B_FOLLOW_NONE, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fLabel(label),
	fGlyph(glyph),
	fMessage(message),
	fTarget(target),
	fPressed(false),
	fInside(false),
	fHover(false)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


HudButton::~HudButton()
{
	delete fMessage;
}


BFont
HudButton::_Font() const
{
	BFont font(be_bold_font);
	font.SetSize(roundf(Bounds().Height() * 0.32f));
	return font;
}


BSize
HudButton::PreferredSize(float scale) const
{
	float height = roundf(60 * scale);
	BFont font(be_bold_font);
	font.SetSize(roundf(height * 0.32f));
	float width = roundf(height * 0.45f) + roundf(height * 0.2f)
		+ font.StringWidth(fLabel.String()) + 2 * roundf(height * 0.5f);
	return BSize(width, height);
}


void
HudButton::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	SetHighColor(0, 0, 0);
	FillRect(updateRect);

	// The controller's glass.
	PushState();
	SetDrawingMode(B_OP_ALPHA);
	SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	// The radius of look::hud_panel().
	float radius = 10;
	BRect pill = bounds.InsetByCopy(1, 1);
	look::hud_panel(this, pill);
	if (fPressed && fInside) {
		SetHighColor(255, 255, 255, 50);
		FillRoundRect(pill, radius, radius);
	} else if (fHover) {
		SetHighColor(255, 255, 255, 24);
		FillRoundRect(pill, radius, radius);
	}
	PopState();

	button_state state = fPressed && fInside ? BUTTON_PRESSED
		: fHover ? BUTTON_HOVER : BUTTON_NORMAL;
	rgb_color color = text_color(state);
	float height = bounds.Height();
	float glyphSize = roundf(height * 0.45f);
	BFont font = _Font();
	float labelWidth = font.StringWidth(fLabel.String());
	float contentWidth = glyphSize + roundf(height * 0.2f) + labelWidth;
	float x = roundf((bounds.Width() - contentWidth) / 2);
	float y = roundf((height - glyphSize) / 2);
	look::glyph(this, fGlyph, BRect(x, y, x + glyphSize, y + glyphSize),
		color);

	SetFont(&font);
	font_height metrics;
	font.GetHeight(&metrics);
	PushState();
	SetDrawingMode(B_OP_OVER);
	SetHighColor(color);
	DrawString(fLabel.String(), BPoint(x + glyphSize + roundf(height * 0.2f),
		roundf(height / 2 + (metrics.ascent - metrics.descent) / 2)));
	PopState();
}


void
HudButton::MouseDown(BPoint where)
{
	fPressed = true;
	fInside = true;
	SetMouseEventMask(B_POINTER_EVENTS,
		B_LOCK_WINDOW_FOCUS | B_NO_POINTER_HISTORY);
	Invalidate();
}


void
HudButton::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	bool inside = Bounds().Contains(where);
	if (fPressed) {
		if (inside != fInside) {
			fInside = inside;
			Invalidate();
		}
		return;
	}
	bool hover = inside && transit != B_EXITED_VIEW
		&& transit != B_OUTSIDE_VIEW;
	if (hover != fHover) {
		fHover = hover;
		Invalidate();
	}
}


void
HudButton::MouseUp(BPoint where)
{
	bool invoke = fPressed && Bounds().Contains(where);
	fPressed = false;
	fInside = false;
	Invalidate();
	if (invoke && fMessage != NULL)
		fTarget.SendMessage(fMessage);
}

}	// namespace airtime
