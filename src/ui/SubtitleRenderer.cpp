/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "SubtitleRenderer.h"

#include <algorithm>
#include <math.h>
#include <string.h>

#include <Bitmap.h>
#include <View.h>

#include "FFmpeg.h"


namespace airtime {


SubtitleRenderer::SubtitleRenderer()
	:
	fScale(1.0f),
	fBottomInset(0),
	fKeyInset(0),
	fKeyWidth(0),
	fKeyHeight(0),
	fKeyScale(0),
	fCanvas(NULL),
	fCanvasView(NULL)
{
}


SubtitleRenderer::~SubtitleRenderer()
{
	delete fCanvas;
}


void
SubtitleRenderer::SetScale(float scale)
{
	fScale = scale;
}


bool
SubtitleRenderer::_EnsureCanvas(int width, int height)
{
	if (fCanvas != NULL) {
		BRect bounds = fCanvas->Bounds();
		if (bounds.Width() + 1 >= width && bounds.Height() + 1 >= height)
			return true;
		width = std::max(width, (int)bounds.Width() + 1);
		height = std::max(height, (int)bounds.Height() + 1);
		delete fCanvas;
		fCanvas = NULL;
		fCanvasView = NULL;
	}
	width = (width + 255) & ~255;
	height = (height + 63) & ~63;
	fCanvas = new BBitmap(BRect(0, 0, width - 1, height - 1),
		B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
	if (fCanvas->InitCheck() != B_OK) {
		delete fCanvas;
		fCanvas = NULL;
		return false;
	}
	fCanvasView = new BView(fCanvas->Bounds(), "subtitles", B_FOLLOW_NONE,
		B_WILL_DRAW);
	fCanvas->AddChild(fCanvasView);
	return true;
}


BFont
SubtitleRenderer::_FontFor(const SubtitleRun& run, float size) const
{
	BFont font(be_bold_font);
	uint16 face = B_BOLD_FACE;
	if (run.italic)
		face |= B_ITALIC_FACE;
	if (run.underline)
		face |= B_UNDERSCORE_FACE;
	font.SetFace(face);
	font.SetSize(size);
	font.SetFlags(B_FORCE_ANTIALIASING);
	return font;
}


bool
SubtitleRenderer::_RenderText(const SubtitleEvent& event, int width,
	int height, Overlay& overlay, float fontSize, bool box)
{
	// Break the runs into words, then the words into lines that fit.
	std::vector<const SubtitleRun*> runs;
	std::vector<Line> lines;
	float maxWidth = width * 0.9f;
	bool anyColor = false;

	for (const SubtitleLine& sourceLine : event.lines) {
		Line line;
		line.width = 0;
		for (const SubtitleRun& run : sourceLine.runs) {
			int runIndex = (int)runs.size();
			runs.push_back(&run);
			if (run.hasColor && (run.color.red != 255 || run.color.green != 255
					|| run.color.blue != 255)) {
				anyColor = true;
			}
			BFont font = _FontFor(run, fontSize);
			const char* text = run.text.String();
			int32 length = run.text.Length();
			int32 start = 0;
			while (start < length) {
				int32 end = start;
				while (end < length && text[end] != ' ')
					end++;
				while (end < length && text[end] == ' ')
					end++;
				Token token;
				token.text.SetTo(text + start, end - start);
				token.run = runIndex;
				float tokenWidth = font.StringWidth(token.text.String());
				if (line.width + tokenWidth > maxWidth && !line.tokens.empty()) {
					lines.push_back(line);
					line = Line();
					line.width = 0;
				}
				line.tokens.push_back(token);
				line.width += tokenWidth;
				start = end;
			}
		}
		// Trailing spaces do not count towards centring.
		if (!line.tokens.empty()) {
			Token& last = line.tokens.back();
			int32 trimmed = last.text.Length();
			while (trimmed > 0 && last.text[trimmed - 1] == ' ')
				trimmed--;
			if (trimmed != last.text.Length()) {
				BFont font = _FontFor(*runs[last.run], fontSize);
				line.width -= font.StringWidth(last.text.String())
					- font.StringWidth(last.text.String(), trimmed);
				last.text.Truncate(trimmed);
			}
		}
		lines.push_back(line);
	}
	while (!lines.empty() && lines.back().tokens.empty())
		lines.pop_back();
	if (lines.empty())
		return false;

	BFont plain = _FontFor(SubtitleRun(), fontSize);
	font_height metrics;
	plain.GetHeight(&metrics);
	float lineHeight = ceilf(metrics.ascent + metrics.descent
		+ metrics.leading * 0.3f);
	float outline = box ? 0 : std::max(1.5f, fontSize * 0.085f);
	float pad = box ? ceilf(fontSize * 0.28f) : ceilf(outline + 2);

	float widest = 0;
	for (const Line& line : lines)
		widest = std::max(widest, line.width);
	int blockWidth = (int)ceilf(widest + pad * 2);
	int blockHeight = (int)ceilf(lines.size() * lineHeight + pad * 2);
	if (blockWidth <= 0 || blockHeight <= 0 || !_EnsureCanvas(blockWidth,
			blockHeight)) {
		return false;
	}

	BRect block(0, 0, blockWidth - 1, blockHeight - 1);
	int32 canvasStride = fCanvas->BytesPerRow();
	std::vector<uint8> fill((size_t)blockWidth * blockHeight);
	std::vector<uint8> edge;
	std::vector<uint32> colors;

	auto drawText = [&](bool colored, float dx, float dy) {
		for (size_t i = 0; i < lines.size(); i++) {
			const Line& line = lines[i];
			float x = pad + (blockWidth - pad * 2 - line.width) / 2 + dx;
			float y = pad + i * lineHeight + metrics.ascent + dy;
			for (const Token& token : line.tokens) {
				const SubtitleRun& run = *runs[token.run];
				BFont font = _FontFor(run, fontSize);
				fCanvasView->SetFont(&font);
				if (colored && run.hasColor)
					fCanvasView->SetHighColor(run.color);
				else
					fCanvasView->SetHighColor(255, 255, 255);
				fCanvasView->DrawString(token.text.String(), BPoint(x, y));
				x += font.StringWidth(token.text.String());
			}
		}
	};
	auto readChannel = [&](std::vector<uint8>& target) {
		const uint8* bits = (const uint8*)fCanvas->Bits();
		for (int y = 0; y < blockHeight; y++) {
			const uint8* row = bits + y * canvasStride;
			uint8* out = target.data() + (size_t)y * blockWidth;
			for (int x = 0; x < blockWidth; x++)
				out[x] = row[x * 4 + 1];
		}
	};

	if (!fCanvas->Lock())
		return false;
	fCanvasView->SetDrawingMode(B_OP_OVER);
	fCanvasView->SetLowColor(0, 0, 0);

	fCanvasView->SetHighColor(0, 0, 0);
	fCanvasView->FillRect(block);
	drawText(false, 0, 0);
	fCanvasView->Sync();
	readChannel(fill);

	if (anyColor) {
		fCanvasView->SetHighColor(0, 0, 0);
		fCanvasView->FillRect(block);
		drawText(true, 0, 0);
		fCanvasView->Sync();
		colors.resize((size_t)blockWidth * blockHeight);
		const uint8* bits = (const uint8*)fCanvas->Bits();
		for (int y = 0; y < blockHeight; y++) {
			const uint32* row = (const uint32*)(bits + y * canvasStride);
			memcpy(colors.data() + (size_t)y * blockWidth, row, blockWidth * 4);
		}
	}

	if (!box) {
		edge.resize((size_t)blockWidth * blockHeight);
		fCanvasView->SetHighColor(0, 0, 0);
		fCanvasView->FillRect(block);
		const int steps = 16;
		for (int ring = 0; ring < 2; ring++) {
			float radius = ring == 0 ? outline : outline * 0.5f;
			for (int i = 0; i < steps; i++) {
				float angle = i * 2 * M_PI / steps;
				drawText(false, cosf(angle) * radius, sinf(angle) * radius);
			}
		}
		fCanvasView->Sync();
		readChannel(edge);
	}
	fCanvas->Unlock();

	overlay.width = blockWidth;
	overlay.height = blockHeight;
	overlay.pixels.resize((size_t)blockWidth * blockHeight);
	for (size_t i = 0; i < overlay.pixels.size(); i++) {
		float f = fill[i] / 255.0f;
		float background = box ? 0.78f : edge[i] / 255.0f;
		float alpha = f + background * (1 - f);
		if (alpha <= 0.0f) {
			overlay.pixels[i] = 0;
			continue;
		}
		float red = 255, green = 255, blue = 255;
		if (anyColor && f > 0.02f) {
			uint32 c = colors[i];
			red = std::min(255.0f, ((c >> 16) & 0xff) / f);
			green = std::min(255.0f, ((c >> 8) & 0xff) / f);
			blue = std::min(255.0f, (c & 0xff) / f);
		}
		// The outline (or box) is black: only the text adds colour.
		float scale = f / alpha;
		uint32 a = (uint32)(alpha * 255 + 0.5f);
		uint32 r = (uint32)(red * scale + 0.5f);
		uint32 g = (uint32)(green * scale + 0.5f);
		uint32 b = (uint32)(blue * scale + 0.5f);
		overlay.pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
	}
	return true;
}


void
SubtitleRenderer::_RenderBitmaps(const SubtitleEvent& event, int width,
	int height, std::vector<Overlay>& overlays)
{
	int canvasWidth = event.canvasWidth;
	int canvasHeight = event.canvasHeight;
	if (canvasWidth <= 0 || canvasHeight <= 0) {
		// Without a canvas, the pictures are assumed to fit the video.
		canvasWidth = width;
		canvasHeight = height;
		for (const SubtitleBitmap& bitmap : event.bitmaps) {
			canvasWidth = std::max(canvasWidth, bitmap.x + bitmap.width);
			canvasHeight = std::max(canvasHeight, bitmap.y + bitmap.height);
		}
	}
	double scaleX = (double)width / canvasWidth;
	double scaleY = (double)height / canvasHeight;

	for (const SubtitleBitmap& bitmap : event.bitmaps) {
		Overlay overlay;
		overlay.x = (int)lround(bitmap.x * scaleX);
		overlay.y = (int)lround(bitmap.y * scaleY);
		overlay.width = std::max(1, (int)lround(bitmap.width * scaleX));
		overlay.height = std::max(1, (int)lround(bitmap.height * scaleY));
		overlay.pixels.resize((size_t)overlay.width * overlay.height);

		if (overlay.width == bitmap.width && overlay.height == bitmap.height) {
			overlay.pixels = bitmap.pixels;
		} else {
			SwsContext* context = sws_getContext(bitmap.width, bitmap.height,
				AV_PIX_FMT_BGRA, overlay.width, overlay.height, AV_PIX_FMT_BGRA,
				SWS_BILINEAR, NULL, NULL, NULL);
			if (context == NULL)
				continue;
			const uint8* source[4] = {(const uint8*)bitmap.pixels.data(),
				NULL, NULL, NULL};
			int sourceStride[4] = {bitmap.width * 4, 0, 0, 0};
			uint8* target[4] = {(uint8*)overlay.pixels.data(), NULL, NULL,
				NULL};
			int targetStride[4] = {overlay.width * 4, 0, 0, 0};
			sws_scale(context, source, sourceStride, 0, bitmap.height, target,
				targetStride);
			sws_freeContext(context);
		}
		overlays.push_back(std::move(overlay));
	}
}


const std::vector<Overlay>&
SubtitleRenderer::Render(const std::vector<SubtitleEventPtr>& events,
	int width, int height)
{
	std::vector<uint64> key;
	for (const SubtitleEventPtr& event : events)
		key.push_back(event->id);
	if (key == fKey && width == fKeyWidth && height == fKeyHeight
		&& fScale == fKeyScale && fBottomInset == fKeyInset) {
		return fOverlays;
	}
	fKey = key;
	fKeyInset = fBottomInset;
	fKeyWidth = width;
	fKeyHeight = height;
	fKeyScale = fScale;
	fOverlays.clear();

	float fontSize = std::max(13.0f, height * 0.052f * fScale);
	int marginX = (int)(width * 0.05f);
	int marginY = (int)(height * 0.05f);
	int bottomUsed = std::max(0, fBottomInset - marginY + 8);
	int topUsed = 0;

	// Events further down the list are newer; stack them above the older
	// ones at the bottom, as rolling captions do.
	for (const SubtitleEventPtr& event : events) {
		if (event->IsBitmap()) {
			_RenderBitmaps(*event, width, height, fOverlays);
			continue;
		}
		Overlay overlay;
		if (!_RenderText(*event, width, height, overlay,
				event->caption ? fontSize * 0.9f : fontSize, event->caption)) {
			continue;
		}
		// Captions come placed on a 15 row grid (libavcodec's 288 lines
		// high); keep the ones meant for the top up there, the rest go to
		// the bottom where captions are read.
		int alignment = event->alignment;
		if (event->caption) {
			alignment = event->positionY >= 0 && event->positionY < 120
				? 8 : 2;
		}
		int column = (alignment - 1) % 3;
		int row = (alignment - 1) / 3;
		if (column == 0)
			overlay.x = marginX;
		else if (column == 2)
			overlay.x = width - marginX - overlay.width;
		else
			overlay.x = (width - overlay.width) / 2;
		if (row == 0) {
			overlay.y = height - marginY - bottomUsed - overlay.height;
			bottomUsed += overlay.height + 2;
		} else if (row == 2) {
			overlay.y = marginY + topUsed;
			topUsed += overlay.height + 2;
		} else
			overlay.y = (height - overlay.height) / 2;
		fOverlays.push_back(std::move(overlay));
	}
	return fOverlays;
}


bool
SubtitleRenderer::RenderMessage(const char* text, int width, int height,
	Overlay& overlay)
{
	SubtitleEvent event;
	SubtitleLine line;
	SubtitleRun run;
	run.text = text;
	line.runs.push_back(run);
	event.lines.push_back(line);
	float fontSize = std::max(13.0f, height * 0.038f);
	if (!_RenderText(event, width, height, overlay, fontSize, false))
		return false;
	overlay.x = width - (int)(width * 0.04f) - overlay.width;
	overlay.y = (int)(height * 0.04f);
	return true;
}


bool
SubtitleRenderer::RenderButton(const char* text, bool highlighted, int width,
	int height, float density, int bottomInset, Overlay& overlay)
{
	if (density <= 0)
		density = 1;
	float viewHeight = height / density;
	float fontSize = std::min(32.0f, std::max(12.0f, viewHeight * 0.024f))
		* density;
	BFont font(be_bold_font);
	font.SetSize(fontSize);
	font.SetFlags(B_FORCE_ANTIALIASING);
	font_height metrics;
	font.GetHeight(&metrics);
	float textWidth = font.StringWidth(text);
	float padX = ceilf(fontSize * 1.1f);
	float padY = ceilf(fontSize * 0.6f);
	int blockWidth = (int)ceilf(textWidth + padX * 2);
	int blockHeight = (int)ceilf(metrics.ascent + metrics.descent + padY * 2);
	if (blockWidth > width || blockHeight > height
		|| !_EnsureCanvas(blockWidth, blockHeight)) {
		return false;
	}

	BRect block(0, 0, blockWidth - 1, blockHeight - 1);
	float radius = std::max(3.0f, blockHeight * 0.18f);
	float border = std::max(1.0f, roundf(fontSize * 0.09f));
	size_t count = (size_t)blockWidth * blockHeight;
	std::vector<uint8> shape(count), ring(count), glyphs(count);
	int32 canvasStride = fCanvas->BytesPerRow();
	auto readChannel = [&](std::vector<uint8>& target) {
		const uint8* bits = (const uint8*)fCanvas->Bits();
		for (int y = 0; y < blockHeight; y++) {
			const uint8* row = bits + y * canvasStride;
			uint8* out = target.data() + (size_t)y * blockWidth;
			for (int x = 0; x < blockWidth; x++)
				out[x] = row[x * 4 + 1];
		}
	};

	// Coverage of the shape, its edge and the text, white on black.
	if (!fCanvas->Lock())
		return false;
	fCanvasView->SetDrawingMode(B_OP_OVER);
	fCanvasView->SetLowColor(0, 0, 0);
	fCanvasView->SetHighColor(0, 0, 0);
	fCanvasView->FillRect(block);
	fCanvasView->SetHighColor(255, 255, 255);
	fCanvasView->FillRoundRect(block, radius, radius);
	fCanvasView->Sync();
	readChannel(shape);

	fCanvasView->SetHighColor(0, 0, 0);
	fCanvasView->FillRect(block);
	fCanvasView->SetHighColor(255, 255, 255);
	fCanvasView->SetPenSize(border);
	BRect edge = block.InsetByCopy(border / 2, border / 2);
	fCanvasView->StrokeRoundRect(edge, radius - border / 2,
		radius - border / 2);
	fCanvasView->SetPenSize(1);
	fCanvasView->Sync();
	readChannel(ring);

	fCanvasView->SetHighColor(0, 0, 0);
	fCanvasView->FillRect(block);
	fCanvasView->SetHighColor(255, 255, 255);
	fCanvasView->SetFont(&font);
	fCanvasView->DrawString(text, BPoint(roundf((blockWidth - textWidth) / 2),
		padY + metrics.ascent));
	fCanvasView->Sync();
	readChannel(glyphs);
	fCanvas->Unlock();

	// A dark translucent button with a light edge; under the pointer, a
	// light one with dark text.
	struct Layer { float red, green, blue, alpha; };
	Layer fill = highlighted ? Layer{240, 240, 240, 0.95f}
		: Layer{20, 20, 20, 0.72f};
	Layer line = highlighted ? Layer{255, 255, 255, 1.0f}
		: Layer{235, 235, 235, 0.85f};
	Layer ink = highlighted ? Layer{16, 16, 16, 1.0f}
		: Layer{255, 255, 255, 1.0f};

	overlay.width = blockWidth;
	overlay.height = blockHeight;
	overlay.pixels.resize(count);
	for (size_t i = 0; i < count; i++) {
		float inside = shape[i] / 255.0f;
		float alpha = inside * fill.alpha;
		float red = fill.red, green = fill.green, blue = fill.blue;
		auto over = [&](const Layer& layer, float coverage) {
			float a = std::min(coverage, inside) * layer.alpha;
			if (a <= 0)
				return;
			float out = a + alpha * (1 - a);
			red = (layer.red * a + red * alpha * (1 - a)) / out;
			green = (layer.green * a + green * alpha * (1 - a)) / out;
			blue = (layer.blue * a + blue * alpha * (1 - a)) / out;
			alpha = out;
		};
		over(line, ring[i] / 255.0f);
		over(ink, glyphs[i] / 255.0f);
		uint32 a = (uint32)(alpha * 255 + 0.5f);
		overlay.pixels[i] = a == 0 ? 0 : (a << 24)
			| ((uint32)(red + 0.5f) << 16) | ((uint32)(green + 0.5f) << 8)
			| (uint32)(blue + 0.5f);
	}

	int margin = std::max((int)(8 * density),
		(int)(std::min(width, height) * 0.04f));
	overlay.x = width - margin - blockWidth;
	overlay.y = height - margin - (int)(std::max(0, bottomInset) * density)
		- blockHeight;
	if (overlay.y < 0)
		overlay.y = 0;
	return true;
}


/*static*/ void
SubtitleRenderer::Blend(const Overlay& overlay, uint8* bits, int32 bytesPerRow,
	int width, int height, uint8 opacity)
{
	int startX = std::max(0, overlay.x);
	int startY = std::max(0, overlay.y);
	int endX = std::min(width, overlay.x + overlay.width);
	int endY = std::min(height, overlay.y + overlay.height);
	for (int y = startY; y < endY; y++) {
		const uint32* source = overlay.pixels.data()
			+ (size_t)(y - overlay.y) * overlay.width + (startX - overlay.x);
		uint8* target = bits + y * bytesPerRow + startX * 4;
		for (int x = startX; x < endX; x++, source++, target += 4) {
			uint32 pixel = *source;
			uint32 alpha = (pixel >> 24) * opacity / 255;
			if (alpha == 0)
				continue;
			uint32 blue = pixel & 0xff;
			uint32 green = (pixel >> 8) & 0xff;
			uint32 red = (pixel >> 16) & 0xff;
			if (alpha == 255) {
				target[0] = blue;
				target[1] = green;
				target[2] = red;
				continue;
			}
			uint32 inverse = 255 - alpha;
			target[0] = (blue * alpha + target[0] * inverse + 127) / 255;
			target[1] = (green * alpha + target[1] * inverse + 127) / 255;
			target[2] = (red * alpha + target[2] * inverse + 127) / 255;
		}
	}
}

}	// namespace airtime
