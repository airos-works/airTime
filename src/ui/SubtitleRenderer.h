/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_SUBTITLE_RENDERER_H
#define AIRTIME_SUBTITLE_RENDERER_H


#include <vector>

#include <Font.h>

#include "Subtitles.h"


class BBitmap;
class BView;


namespace airtime {

/*!	A picture with straight alpha, B_RGBA32, placed on the video. */
struct Overlay {
	int					x;
	int					y;
	int					width;
	int					height;
	std::vector<uint32>	pixels;
};


/*!	Turns subtitle events into overlays for a picture of a given size: text
	in white with a dark outline (captions on a black box, as televisions
	show them), picture subtitles scaled from their canvas. Text is drawn
	off screen by the app_server and its alpha recovered from the coverage,
	since the server does not write alpha into off screen bitmaps. */
class SubtitleRenderer {
public:
								SubtitleRenderer();
								~SubtitleRenderer();

			void				SetScale(float scale);
			float				Scale() const { return fScale; }
			// Rows at the bottom of the picture that something else covers
			// (the full screen controller); bottom subtitles go above them.
			void				SetBottomInset(int rows) { fBottomInset = rows; }

			// Returns overlays for the events; the result is reused while
			// the events and the size stay the same.
			const std::vector<Overlay>& Render(
									const std::vector<SubtitleEventPtr>& events,
									int width, int height);

			// A short message in a corner ("Subtitles: English").
			bool				RenderMessage(const char* text, int width,
									int height, Overlay& overlay);
			// A button in the bottom right corner ("Skip Intro"), above
			// bottomInset rows. density: picture pixels to a view's, so that
			// the button keeps its size on screen whether the picture is made
			// at the screen's density or not; the inset is in view rows.
			bool				RenderButton(const char* text,
									bool highlighted, int width, int height,
									float density, int bottomInset,
									Overlay& overlay);

	static	void				Blend(const Overlay& overlay, uint8* bits,
									int32 bytesPerRow, int width, int height,
									uint8 opacity = 255);

private:
			struct Token {
				BString			text;
				int				run;
			};
			struct Line {
				std::vector<Token> tokens;
				float			width;
			};

			bool				_RenderText(const SubtitleEvent& event,
									int width, int height, Overlay& overlay,
									float fontSize, bool box);
			void				_RenderBitmaps(const SubtitleEvent& event,
									int width, int height,
									std::vector<Overlay>& overlays);
			BFont				_FontFor(const SubtitleRun& run,
									float size) const;
			bool				_EnsureCanvas(int width, int height);

			float				fScale;
			int					fBottomInset;
			int					fKeyInset;
			std::vector<uint64>	fKey;
			int					fKeyWidth;
			int					fKeyHeight;
			float				fKeyScale;
			std::vector<Overlay> fOverlays;

			BBitmap*			fCanvas;
			BView*				fCanvasView;
};

}	// namespace airtime

#endif	// AIRTIME_SUBTITLE_RENDERER_H
