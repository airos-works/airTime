/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "VideoView.h"

#include <algorithm>
#include <chrono>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Bitmap.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Region.h>
#include <Window.h>

#include "Messages.h"
#include "Look.h"


// What an app_server that keeps direct windows connected at a higher
// density adds to direct_buffer_info: the density in percent and its own
// copy of the screen. Headers from before the fields have them as the first
// three words of the reserved space; later ones name them, and the reserved
// space begins after them, so the names have to be used where they exist.
// B_DIRECT_DEVICE_PIXELS came with the fields.
#ifdef B_DIRECT_DEVICE_PIXELS
#	define DIRECT_DEVICE_SCALE(info) ((info)->device_scale)
#	define DIRECT_DRAWING_AREA(info) ((info)->drawing_bits_area)
#	define DIRECT_DRAWING_BYTES_PER_ROW(info) ((info)->drawing_bytes_per_row)
#else
#	define DIRECT_DEVICE_SCALE(info) ((info)->_reserved1[0])
#	define DIRECT_DRAWING_AREA(info) ((area_id)(info)->_reserved1[1])
#	define DIRECT_DRAWING_BYTES_PER_ROW(info) ((int32)(info)->_reserved1[2])
#endif


namespace airtime {


VideoView::VideoView(BRect frame, const BMessenger& target)
	:
	BView(frame, "video", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS
		| B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE),
	fTarget(target),
	fPlayer(NULL),
	fCurrent(0),
	fDirty(false),
	fMessageUntil(0),
	fAudioOnly(false),
	fFullScreen(false),
	fComposeTime(0),
	fDrawTime(0),
	fDirectStale(false),
	fDirectConnected(false),
	fDirectAllowed(getenv("AIRTIME_NO_DIRECT") == NULL),
	fDirectUsed(false),
	fCovered(false),
	fDirectBits(NULL),
	fDirectBytesPerRow(0),
	fDirectScale(1.0f),
	fDrawingSource(-1),
	fDrawingClone(-1),
	fDrawingBits(NULL),
	fDrawingBytesPerRow(0),
	fDirectDaemon(-1)
{
	fBitmaps[0] = NULL;
	fBitmaps[1] = NULL;
	SetViewColor(B_TRANSPARENT_COLOR);
	SetHighColor(0, 0, 0);
	fVideoRect = Bounds();
}


VideoView::~VideoView()
{
	delete fBitmaps[0];
	delete fBitmaps[1];
	if (fDrawingClone >= 0)
		delete_area(fDrawingClone);
}


void
VideoView::AttachedToWindow()
{
	BView::AttachedToWindow();
	MakeFocus(true);
	UpdateWindowOrigin();
}


void
VideoView::SetPlayer(Player* player)
{
	if (player != fPlayer) {
		std::lock_guard<std::mutex> render(fRenderLock);
		fPlayer = player;
		fLastFrame.reset();
		delete fBitmaps[0];
		delete fBitmaps[1];
		fBitmaps[0] = fBitmaps[1] = NULL;
	}
	{
		std::lock_guard<std::mutex> lock(fGeometryLock);
		fVideoRect = _VideoRectFor(Bounds());
	}
	Invalidate();
}


void
VideoView::SetAudioOnly(bool audioOnly)
{
	fAudioOnly = audioOnly;
	Invalidate();
}


void
VideoView::SetInfo(const char* title, const char* detail)
{
	fTitle = title;
	fDetail = detail;
	if (fAudioOnly || fLastFrame.get() == NULL)
		Invalidate();
}


void
VideoView::SetSubtitleScale(float scale)
{
	{
		std::lock_guard<std::mutex> render(fRenderLock);
		fSubtitles.SetScale(scale);
	}
	Refresh();
}


void
VideoView::ShowMessage(const char* text)
{
	{
		std::lock_guard<std::mutex> lock(fMessageLock);
		fMessage = text;
		fMessageUntil = system_time() + 1600000;
	}
	Refresh();
}


void
VideoView::Refresh()
{
	fDirty = true;
	if (fPlayer == NULL || !fPlayer->IsPlaying() || fPlayer->IsScanning()
		|| !fPlayer->HasMovingVideo()) {
		Invalidate(VideoFrame());
	}
}


void
VideoView::SetFullScreen(bool fullScreen)
{
	fFullScreen = fullScreen;
	if (!fullScreen)
		SetCoveredBottom(-1);
}


void
VideoView::SetCoveredBottom(float top)
{
	BRect video = VideoFrame();
	int rows = 0;
	if (top >= 0 && top <= video.bottom)
		rows = (int)(video.bottom - top + 1);
	{
		// A child view over the picture: the frame buffer copy would paint
		// over it, so the server draws while it is there.
		std::lock_guard<std::timed_mutex> lock(fDirectLock);
		fCovered = top >= 0;
	}
	{
		std::lock_guard<std::mutex> render(fRenderLock);
		fSubtitles.SetBottomInset(rows);
	}
	Refresh();
}


BRect
VideoView::VideoFrame()
{
	std::lock_guard<std::mutex> lock(fGeometryLock);
	return fVideoRect;
}


BRect
VideoView::_VideoRectFor(BRect bounds) const
{
	float width = bounds.Width() + 1;
	float height = bounds.Height() + 1;
	if (fPlayer == NULL || !fPlayer->HasVideo() || width < 2 || height < 2)
		return bounds;
	double aspect = fPlayer->DisplayAspect();
	if (aspect <= 0)
		return bounds;
	float videoWidth = width;
	float videoHeight = height;
	if (width / height > aspect)
		videoWidth = roundf(height * aspect);
	else
		videoHeight = roundf(width / aspect);
	float left = floorf(bounds.left + (width - videoWidth) / 2);
	float top = floorf(bounds.top + (height - videoHeight) / 2);
	return BRect(left, top, left + videoWidth - 1, top + videoHeight - 1);
}


bool
VideoView::_Compose(const VideoFramePtr& frame, int index, int width,
	int height)
{
	// Called with fRenderLock held.
	BBitmap*& bitmap = fBitmaps[index];
	if (bitmap == NULL || bitmap->Bounds().IntegerWidth() + 1 != width
		|| bitmap->Bounds().IntegerHeight() + 1 != height) {
		delete bitmap;
		bitmap = new BBitmap(BRect(0, 0, width - 1, height - 1), B_RGB32);
		if (bitmap->InitCheck() != B_OK) {
			delete bitmap;
			bitmap = NULL;
			return false;
		}
	}
	uint8* bits = (uint8*)bitmap->Bits();
	int32 bytesPerRow = bitmap->BytesPerRow();
	bigtime_t traceStart = system_time();
	if (!fRenderer.Render(frame->frame, bits, bytesPerRow, width, height))
		return false;
	fTraceRender = system_time() - traceStart;
	traceStart = system_time();

	if (fPlayer != NULL && fPlayer->HasMovingVideo()) {
		std::vector<SubtitleEventPtr> events
			= fPlayer->ActiveSubtitles(frame->pts);
		const std::vector<Overlay>& overlays = fSubtitles.Render(events, width,
			height);
		for (const Overlay& overlay : overlays)
			SubtitleRenderer::Blend(overlay, bits, bytesPerRow, width, height);
	}

	BString message;
	bigtime_t until;
	{
		std::lock_guard<std::mutex> lock(fMessageLock);
		message = fMessage;
		until = fMessageUntil;
	}
	bigtime_t now = system_time();
	if (message.Length() > 0 && now < until) {
		Overlay overlay;
		if (fSubtitles.RenderMessage(message.String(), width, height,
				overlay)) {
			bigtime_t left = until - now;
			uint8 opacity = left > 400000 ? 255 : (uint8)(left * 255 / 400000);
			SubtitleRenderer::Blend(overlay, bits, bytesPerRow, width, height,
				opacity);
		}
	}
	fTraceOverlays = system_time() - traceStart;
	return true;
}


bool
VideoView::DisplayFrame(const VideoFramePtr& frame)
{
	BRect rect = VideoFrame();
	int width = rect.IntegerWidth() + 1;
	int height = rect.IntegerHeight() + 1;
	if (width < 2 || height < 2)
		return true;

	// Drawn into the frame buffer at a higher density, the picture is made
	// at that density: a film keeps the detail the screen can show.
	float scale = _DirectScale();
	if (scale != 1.0f) {
		int left, top;
		_DeviceRect(rect, BPoint(0, 0), scale, &left, &top, &width, &height);
	}

	static const bool sTrace = getenv("AIRTIME_TRACE_DROPS") != NULL;
	bigtime_t entered = system_time();
	std::lock_guard<std::mutex> render(fRenderLock);
	int next = fCurrent ^ 1;
	bigtime_t start = system_time();
	if (scale == 1.0f && _RenderDirect(frame, rect, width, height)) {
		// The bitmaps do not have this picture: should the view have to
		// draw itself, it makes it again.
		fLastFrame = frame;
		fDirty = true;
		fComposeTime = (fComposeTime * 15 + (system_time() - start)) / 16;
		fDrawTime = fDrawTime * 15 / 16;
		return true;
	}
	if (!_Compose(frame, next, width, height))
		return true;
	fLastFrame = frame;
	bigtime_t composed = system_time();
	fComposeTime = (fComposeTime * 15 + (composed - start)) / 16;

	if (_DrawDirect(fBitmaps[next], rect)) {
		fCurrent = next;
		fDirty = false;
		bigtime_t drawn = system_time();
		fDrawTime = (fDrawTime * 15 + (drawn - composed)) / 16;
		if (sTrace && drawn - entered > 30000) {
			fprintf(stderr, "airTime: slow picture: %.1f ms for the lock, "
				"%.1f ms (%.1f + %.1f) to make it, %.1f ms to draw it\n",
				(start - entered) / 1000.0, (composed - start) / 1000.0,
				fTraceRender / 1000.0, fTraceOverlays / 1000.0,
				(drawn - composed) / 1000.0);
		}
		return true;
	}

	if (LockLooperWithTimeout(40000) != B_OK)
		return false;
	bigtime_t locked = system_time();
	BRect now = VideoFrame();
	if (now == rect && scale == 1.0f)
		DrawBitmap(fBitmaps[next], rect.LeftTop());
	else {
		// Resized while this one was made: stretch it, the next fits.
		DrawBitmap(fBitmaps[next], fBitmaps[next]->Bounds(), now,
			B_FILTER_BITMAP_BILINEAR);
	}
	Sync();
	fCurrent = next;
	fDirty = false;
	UnlockLooper();
	fDrawTime = (fDrawTime * 15 + (system_time() - locked)) / 16;
	return true;
}


void
VideoView::DirectConnected(direct_buffer_info* info)
{
	// The app_server gives a window half a second to answer before it ends
	// its frame buffer access for good. Answer in time even if a picture is
	// still being copied: stop drawing directly until the next change.
	std::unique_lock<std::timed_mutex> lock(fDirectLock,
		std::chrono::milliseconds(200));
	if (!lock.owns_lock()) {
		fDirectStale = true;
		fprintf(stderr, "airTime: a frame buffer change came while a picture "
			"was being drawn; drawing through the app_server for now\n");
		return;
	}
	fDirectStale = false;
	fDirectDaemon = find_thread(NULL);
	switch (info->buffer_state & B_DIRECT_MODE_MASK) {
		case B_DIRECT_START:
		case B_DIRECT_MODIFY:
		{
			if (getenv("AIRTIME_TRACE") != NULL) {
				fprintf(stderr, "airTime: direct %s, %d bpp, format %#x, "
					"%" B_PRIu32 " clips, bits %p, %" B_PRId32 " bytes a row, "
					"window (%" B_PRId32 ",%" B_PRId32 ")-(%" B_PRId32 ",%"
					B_PRId32 "), scale %" B_PRIu32 ", copy area %" B_PRId32
					" with %" B_PRId32 " bytes a row\n",
					(info->buffer_state & B_DIRECT_MODE_MASK) == B_DIRECT_START
						? "start" : "modify", (int)info->bits_per_pixel,
					(unsigned)info->pixel_format, info->clip_list_count,
					info->bits, (int32)info->bytes_per_row,
					info->window_bounds.left, info->window_bounds.top,
					info->window_bounds.right, info->window_bounds.bottom,
					(uint32)DIRECT_DEVICE_SCALE(info),
					(int32)DIRECT_DRAWING_AREA(info),
					(int32)DIRECT_DRAWING_BYTES_PER_ROW(info));
			}
			bool usable = info->bits != NULL && info->bits_per_pixel == 32
				&& (info->pixel_format == B_RGB32
					|| info->pixel_format == B_RGBA32)
				&& info->layout == B_BUFFER_NONINTERLEAVED
				&& info->orientation == B_BUFFER_TOP_TO_BOTTOM;
			fDirectBits = (uint8*)info->bits;
			fDirectBytesPerRow = info->bytes_per_row;
			fDirectWindowBounds = info->window_bounds;
			fDirectClips.assign(info->clip_list,
				info->clip_list + info->clip_list_count);

			// The density and the app_server's copy of the screen; zero
			// from an app_server without them. The density is in percent.
			uint32 percent = DIRECT_DEVICE_SCALE(info);
			area_id drawing = DIRECT_DRAWING_AREA(info);
			fDirectScale = percent > 100 && percent <= 400
				? percent / 100.0f : 1.0f;
			if (fDirectScale != 1.0f) {
				if (drawing != fDrawingSource) {
					if (fDrawingClone >= 0)
						delete_area(fDrawingClone);
					void* bits = NULL;
					fDrawingClone = drawing >= 0
						? clone_area("airTime screen copy", &bits,
							B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, drawing)
						: -1;
					fDrawingSource = drawing;
					fDrawingBits = fDrawingClone >= 0 ? (uint8*)bits : NULL;
				}
				fDrawingBytesPerRow = DIRECT_DRAWING_BYTES_PER_ROW(info);
				// Without the copy, what is drawn would be erased by the
				// next copy to the screen.
				if (fDrawingBits == NULL)
					usable = false;
			}
			fDirectConnected = usable;
			break;
		}
		case B_DIRECT_STOP:
			if (getenv("AIRTIME_TRACE") != NULL)
				fprintf(stderr, "airTime: direct stop\n");
			fDirectConnected = false;
			fDirectBits = NULL;
			fDirectClips.clear();
			break;
	}
}


float
VideoView::_DirectScale()
{
	if (!fDirectAllowed)
		return 1.0f;
	std::lock_guard<std::timed_mutex> lock(fDirectLock);
	if (!_DirectStillConnected() || fCovered || fDirectBits == NULL)
		return 1.0f;
	return fDirectScale;
}


/*!	Called with fDirectLock held. When the app_server gives up on a window
	that took too long to answer it, it ends the window's daemon thread and
	sends no stop: the clipping known here is then out of date for good. */
bool
VideoView::_DirectStillConnected()
{
	if (!fDirectConnected || fDirectStale)
		return false;
	thread_info info;
	if (fDirectDaemon >= 0 && get_thread_info(fDirectDaemon, &info) != B_OK) {
		fDirectConnected = false;
		fDirectBits = NULL;
		fDirectClips.clear();
		fprintf(stderr, "airTime: frame buffer access ended without a stop; "
			"drawing through the app_server\n");
		return false;
	}
	return true;
}


/*!	A rectangle of the view in frame buffer pixels: each edge rounded to the
	nearest, so that rectangles side by side stay side by side. */
/*static*/ void
VideoView::_DeviceRect(BRect rect, BPoint origin, float scale, int* left,
	int* top, int* width, int* height)
{
	*left = (int)roundf((origin.x + rect.left) * scale);
	*top = (int)roundf((origin.y + rect.top) * scale);
	*width = (int)roundf((origin.x + rect.right + 1) * scale) - *left;
	*height = (int)roundf((origin.y + rect.bottom + 1) * scale) - *top;
}


void
VideoView::SetDirectAllowed(bool allowed)
{
	fDirectAllowed = allowed && getenv("AIRTIME_NO_DIRECT") == NULL;
}


void
VideoView::UpdateWindowOrigin()
{
	// Called with the window locked.
	BPoint origin = ConvertToScreen(BPoint(0, 0))
		- Window()->ConvertToScreen(BPoint(0, 0));
	std::lock_guard<std::timed_mutex> lock(fDirectLock);
	fWindowOrigin = origin;
}


/*!	Makes the picture straight in the frame buffer, when all of it is
	visible in one piece and nothing is to be drawn over it. Making it in a
	bitmap and copying that means twice the pixels written and once read,
	and moving memory is what a small board is slowest at: on the Raspberry
	Pi 4 this halves the time a picture takes.
	AIRTIME_NO_DIRECT_RENDER leaves it off.
*/
bool
VideoView::_RenderDirect(const VideoFramePtr& frame, BRect rect, int width,
	int height)
{
	static const bool sDisabled = getenv("AIRTIME_NO_DIRECT_RENDER") != NULL;
	if (sDisabled || !fDirectAllowed)
		return false;

	// subtitles or a message to blend in: that needs the bitmap
	if (fPlayer != NULL && fPlayer->HasMovingVideo()
		&& !fPlayer->ActiveSubtitles(frame->pts).empty()) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(fMessageLock);
		if (fMessage.Length() > 0 && system_time() < fMessageUntil)
			return false;
	}

	std::lock_guard<std::timed_mutex> lock(fDirectLock);
	if (!_DirectStillConnected() || fCovered || fDirectBits == NULL
		|| fDirectScale != 1.0f) {
		return false;
	}

	int left, top, deviceWidth, deviceHeight;
	_DeviceRect(rect, fWindowOrigin, 1.0f, &left, &top, &deviceWidth,
		&deviceHeight);
	left += fDirectWindowBounds.left;
	top += fDirectWindowBounds.top;
	if (deviceWidth != width || deviceHeight != height)
		return false;

	bool whole = false;
	for (const clipping_rect& clip : fDirectClips) {
		if (clip.left <= left && clip.top <= top
			&& clip.right >= left + width - 1
			&& clip.bottom >= top + height - 1) {
			whole = true;
			break;
		}
	}
	if (!whole)
		return false;

	if (!fRenderer.Render(frame->frame,
			fDirectBits + (size_t)top * fDirectBytesPerRow + (size_t)left * 4,
			fDirectBytesPerRow, width, height)) {
		return false;
	}
	fDirectUsed = true;
	return true;
}


bool
VideoView::_DrawDirect(BBitmap* bitmap, BRect rect)
{
	if (!fDirectAllowed) {
		fDirectUsed = false;
		return false;
	}
	std::lock_guard<std::timed_mutex> lock(fDirectLock);
	if (!_DirectStillConnected() || fCovered || fDirectBits == NULL) {
		fDirectUsed = false;
		return false;
	}

	// The picture's place on the screen, in frame buffer pixels. A picture
	// made before the density changed does not fit; the next one will.
	float scale = fDirectScale;
	int left, top, width, height;
	_DeviceRect(rect, fWindowOrigin, scale, &left, &top, &width, &height);
	left += fDirectWindowBounds.left;
	top += fDirectWindowBounds.top;
	if (bitmap->Bounds().IntegerWidth() + 1 != width
		|| bitmap->Bounds().IntegerHeight() + 1 != height) {
		fDirectUsed = false;
		return false;
	}
	const uint8* source = (const uint8*)bitmap->Bits();
	int32 sourceStride = bitmap->BytesPerRow();
	uint8* copy = scale != 1.0f ? fDrawingBits : NULL;

	for (const clipping_rect& clip : fDirectClips) {
		int x0 = std::max(left, (int)clip.left);
		int y0 = std::max(top, (int)clip.top);
		int x1 = std::min(left + width - 1, (int)clip.right);
		int y1 = std::min(top + height - 1, (int)clip.bottom);
		if (x1 < x0 || y1 < y0)
			continue;
		size_t bytes = (size_t)(x1 - x0 + 1) * 4;
		for (int y = y0; y <= y1; y++) {
			const uint8* from = source + (size_t)(y - top) * sourceStride
				+ (x0 - left) * 4;
			memcpy(fDirectBits + (size_t)y * fDirectBytesPerRow + x0 * 4,
				from, bytes);
			if (copy != NULL) {
				memcpy(copy + (size_t)y * fDrawingBytesPerRow + x0 * 4, from,
					bytes);
			}
		}
	}
	fDirectUsed = true;
	return true;
}


void
VideoView::GetTimings(bigtime_t* compose, bigtime_t* draw)
{
	*compose = fComposeTime;
	*draw = fDrawTime;
}


void
VideoView::Draw(BRect updateRect)
{
	if (fAudioOnly) {
		_DrawAudioOnly(updateRect);
		return;
	}
	BRect rect = VideoFrame();

	// Black around the picture.
	BRegion bars(Bounds());
	bars.Exclude(rect);
	SetHighColor(0, 0, 0);
	FillRegion(&bars);

	if (fPlayer == NULL || fLastFrame.get() == NULL) {
		if (fBitmaps[fCurrent] == NULL) {
			_DrawEmpty(updateRect);
			return;
		}
	}

	// While a film plays, the next picture comes soon enough; a still
	// (paused, or album art) is made again here at the new size.
	bool presenterDraws = fPlayer != NULL && fPlayer->IsPlaying()
		&& !fPlayer->IsScanning() && fPlayer->HasMovingVideo();
	if (fDirty && !presenterDraws && fLastFrame.get() != NULL
		&& fRenderLock.try_lock()) {
		int next = fCurrent ^ 1;
		if (_Compose(fLastFrame, next, rect.IntegerWidth() + 1,
				rect.IntegerHeight() + 1)) {
			fCurrent = next;
		}
		fDirty = false;
		fRenderLock.unlock();
	}

	BBitmap* bitmap = fBitmaps[fCurrent];
	if (bitmap == NULL) {
		FillRect(rect);
		return;
	}
	BRect bounds = bitmap->Bounds();
	if (bounds.Width() == rect.Width() && bounds.Height() == rect.Height())
		DrawBitmap(bitmap, rect.LeftTop());
	else
		DrawBitmap(bitmap, bounds, rect, B_FILTER_BITMAP_BILINEAR);
}


void
VideoView::_DrawEmpty(BRect updateRect)
{
	SetHighColor(0, 0, 0);
	FillRect(updateRect);
	BRect bounds = Bounds();
	if (bounds.Height() < 60)
		return;

	BFont font(be_bold_font);
	font.SetSize(15);
	SetFont(&font);
	SetDrawingMode(B_OP_OVER);
	const char* line1 = fTitle.Length() > 0 ? fTitle.String()
		: "Drop a movie here";
	const char* line2 = fDetail.Length() > 0 ? fDetail.String()
		: "or choose Open… from the File menu";
	SetHighColor(200, 200, 204);
	float center = (bounds.left + bounds.right) / 2;
	float middle = (bounds.top + bounds.bottom) / 2;
	DrawString(line1, BPoint(center - font.StringWidth(line1) / 2, middle - 4));
	font.SetSize(12);
	font.SetFace(B_REGULAR_FACE);
	SetFont(&font);
	SetHighColor(130, 130, 136);
	DrawString(line2, BPoint(center - font.StringWidth(line2) / 2, middle + 16));
	SetDrawingMode(B_OP_COPY);
}


void
VideoView::_DrawAudioOnly(BRect updateRect)
{
	BRect bounds = Bounds();
	look::surface(this, updateRect);

	BRect panel = bounds.InsetByCopy(12, 10);
	if (!panel.IsValid() || panel.Height() < 20)
		return;
	look::lcd(this, panel);

	BFont font(be_bold_font);
	font.SetSize(15);
	SetFont(&font);
	SetDrawingMode(B_OP_OVER);
	font_height height;
	font.GetHeight(&height);
	BString title(fTitle);
	font.TruncateString(&title, B_TRUNCATE_END, panel.Width() - 24);
	SetHighColor(look::lcd_text_color());
	float baseline = panel.top + 10 + height.ascent;
	DrawString(title.String(), BPoint(panel.left + 12, baseline));

	BFont small(be_plain_font);
	small.SetSize(12);
	SetFont(&small);
	BString detail(fDetail);
	small.TruncateString(&detail, B_TRUNCATE_END, panel.Width() - 24);
	SetHighColor(look::lcd_dim_text_color());
	DrawString(detail.String(), BPoint(panel.left + 12, baseline + 20));
	SetDrawingMode(B_OP_COPY);
}


void
VideoView::FrameResized(float width, float height)
{
	{
		std::lock_guard<std::mutex> lock(fGeometryLock);
		fVideoRect = _VideoRectFor(Bounds());
	}
	UpdateWindowOrigin();
	fDirty = true;
	Invalidate();
}


void
VideoView::_Post(uint32 what)
{
	BMessage message(what);
	fTarget.SendMessage(&message);
}


void
VideoView::MouseDown(BPoint where)
{
	MakeFocus(true);
	BMessage* current = Window()->CurrentMessage();
	int32 buttons = B_PRIMARY_MOUSE_BUTTON;
	int32 clicks = 1;
	if (current != NULL) {
		current->FindInt32("buttons", &buttons);
		current->FindInt32("clicks", &clicks);
	}
	if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
		BMessage message('ctxm');
		message.AddPoint("where", ConvertToScreen(where));
		fTarget.SendMessage(&message);
		return;
	}
	if (fAudioOnly)
		return;
	BMessage message(kMsgVideoClicked);
	message.AddInt32("clicks", clicks);
	fTarget.SendMessage(&message);
}


void
VideoView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (fFullScreen && (fabsf(where.x - fLastMouse.x) > 2
			|| fabsf(where.y - fLastMouse.y) > 2)) {
		BMessage message(kMsgControlsActivity);
		message.AddPoint("where", where);
		fTarget.SendMessage(&message);
	}
	fLastMouse = where;
}


void
VideoView::KeyDown(const char* bytes, int32 count)
{
	uint32 modifierKeys = modifiers();
	bool shift = (modifierKeys & B_SHIFT_KEY) != 0;
	bool option = (modifierKeys & B_OPTION_KEY) != 0;

	switch (bytes[0]) {
		case B_SPACE:
			_Post(kMsgTogglePlay);
			return;
		case B_LEFT_ARROW:
			if (option)
				_Post(kMsgGoToStart);
			else if (shift) {
				BMessage message(kMsgSeekBy);
				message.AddInt64("delta", -10000000);
				fTarget.SendMessage(&message);
			} else
				_Post(kMsgStepBackward);
			return;
		case B_RIGHT_ARROW:
			if (option)
				_Post(kMsgGoToEnd);
			else if (shift) {
				BMessage message(kMsgSeekBy);
				message.AddInt64("delta", 10000000);
				fTarget.SendMessage(&message);
			} else
				_Post(kMsgStepForward);
			return;
		case B_UP_ARROW:
			_Post(kMsgVolumeUp);
			return;
		case B_DOWN_ARROW:
			_Post(kMsgVolumeDown);
			return;
		case B_HOME:
			_Post(kMsgGoToStart);
			return;
		case B_END:
			_Post(kMsgGoToEnd);
			return;
		case B_PAGE_UP:
		case B_PAGE_DOWN:
		{
			BMessage message(kMsgChapter);
			message.AddInt32("step", bytes[0] == B_PAGE_UP ? -1 : 1);
			fTarget.SendMessage(&message);
			return;
		}
		case B_ESCAPE:
			_Post(kMsgExitFullScreen);
			return;
		case B_ENTER:
			if (option) {
				_Post(kMsgToggleFullScreen);
				return;
			}
			break;
		case 'f':
		case 'F':
			_Post(kMsgToggleFullScreen);
			return;
		case 'j':
		case 'J':
		case 'k':
		case 'K':
		case 'l':
		case 'L':
		{
			BMessage message(kMsgJKL);
			char key = bytes[0] | 0x20;
			message.AddInt32("key", key == 'j' ? -1 : key == 'k' ? 0 : 1);
			fTarget.SendMessage(&message);
			return;
		}
		case 'm':
		case 'M':
			_Post(kMsgToggleMute);
			return;
		case 'c':
		case 'C':
			_Post(kMsgToggleCaptions);
			return;
		case 's':
		case 'S':
			_Post(kMsgCycleSubtitle);
			return;
		case 'a':
		case 'A':
			_Post(kMsgCycleAudio);
			return;
		case ']':
			_Post(kMsgFaster);
			return;
		case '[':
			_Post(kMsgSlower);
			return;
		case '\\':
		{
			BMessage message(kMsgSetRate);
			message.AddDouble("rate", 1.0);
			fTarget.SendMessage(&message);
			return;
		}
		case 'i':
		case 'I':
			_Post(kMsgShowInspector);
			return;
	}
	BView::KeyDown(bytes, count);
}


void
VideoView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_SIMPLE_DATA:
		case B_REFS_RECEIVED:
			// Dropped files: the window decides what they are.
			fTarget.SendMessage(message);
			break;
		case B_MOUSE_WHEEL_CHANGED:
		{
			float delta = 0;
			if (message->FindFloat("be:wheel_delta_y", &delta) == B_OK
				&& delta != 0) {
				_Post(delta < 0 ? kMsgVolumeUp : kMsgVolumeDown);
			}
			break;
		}
		default:
			BView::MessageReceived(message);
	}
}

}	// namespace airtime
