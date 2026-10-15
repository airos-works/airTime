/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_FRAME_RENDERER_H
#define AIRTIME_FRAME_RENDERER_H


#include <memory>
#include <vector>

#include "FFmpeg.h"
#include "SdrScaler.h"
#include "YuvScaler.h"


namespace airtime {

/*!	Converts a decoded picture, in whatever format the decoder produced, to
	B_RGB32 pixels of the size it is shown at, spread over several threads:
	with scalers of its own for the common cases on ARM, in one swscale pass
	otherwise. */
class FrameRenderer {
public:
								FrameRenderer();
								~FrameRenderer();

			bool				Render(const AVFrame* frame, uint8* bits,
									int32 bytesPerRow, int width, int height);

private:
			bool				_Prepare(const AVFrame* frame, int width,
									int height, bool hdr);
			bool				_RenderHDR(const AVFrame* frame, uint8* bits,
									int32 bytesPerRow, int width, int height);
			bool				_RenderSDR(const AVFrame* frame, uint8* bits,
									int32 bytesPerRow, int width, int height);
			void				_PrepareToneMapping(const AVFrame* frame);

			SwsContext*			fContext;
			int					fSourceWidth;
			int					fSourceHeight;
			int					fSourceFormat;
			int					fColorSpace;
			int					fRange;
			int					fWidth;
			int					fHeight;
			bool				fHDR;
			AVFrame*			fSource;
			AVFrame*			fTarget;

			// High dynamic range: PQ or HLG, BT.2020, shown on an ordinary
			// display.
			SdrScaler			fSdrScaler;		// eight-bit 4:2:0
			YuvScaler			fScaler;		// ten-bit 4:2:0
			std::vector<uint16>	fLinear;		// R'G'B' 16 bit, scaled, for
												// the rest
			std::vector<float>	fTransfer;		// 4096 steps of code ->
												// linear, 1 = SDR white
			std::vector<uint8>	fEncode;		// linear -> sRGB, 4096 steps
			int					fTransferKind;
			float				fPeak;			// content peak, 1 = SDR white
			bigtime_t			fTraceScaling;	// AIRTIME_TRACE_HDR: processor time
			bigtime_t			fTraceMapping;
			std::unique_ptr<class WorkerPool> fPool;
};

}	// namespace airtime

#endif	// AIRTIME_FRAME_RENDERER_H
