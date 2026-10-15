/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_YUV_SCALER_H
#define AIRTIME_YUV_SCALER_H


#include <vector>

#include "FFmpeg.h"


namespace airtime {

/*!	Scales ten-bit 4:2:0 pictures (P010 from the hardware decoders,
	yuv420p10 from libavcodec) to the size they are shown at, a target row
	at a time, and gives each row's luma, Cb and Cr at full target width as
	sixteen-bit values (a ten-bit code times 64). What becomes of them -
	tone mapping, for the films that come this way - is the caller's.

	The filter is a triangle as wide as the scale factor: bilinear when
	enlarging, an average over the area each target pixel covers when
	shrinking. Chroma is placed where the stream says it was sampled.
	Rows are independent, so several threads can make them at once, each
	with its own Scratch.
*/
class YuvScaler {
public:
	struct Scratch {
		std::vector<uint16>	luma;
		std::vector<uint16>	cb;
		std::vector<uint16>	cr;
	};

	// How one dimension is scaled.
	struct Taps {
		int					count = 0;
		std::vector<int32>	first;		// per target sample
		std::vector<uint16>	weights;	// count per target sample,
										// summing to 1 << 14
		// The same for eight target samples at once, when their sources
		// fit in 32 samples: where each lane finds its sample, as byte
		// indices into those 32, and its weight, for every tap.
		bool				grouped = false;
		std::vector<int32>	groupFirst;
		std::vector<uint8>	groupIndices;	// 16 per group and tap
		std::vector<uint16>	groupWeights;	// 8 per group and tap
	};

	static	bool				Handles(const AVFrame* frame);

			// Builds the filters when the picture or target size changed.
			bool				Prepare(const AVFrame* frame, int width,
									int height);
			void				ScaleRow(const AVFrame* frame, int y,
									uint16* luma, uint16* cb, uint16* cr,
									Scratch& scratch) const;

private:
	static	bool				_MakeTaps(Taps& taps, int sourceSize,
									int targetSize, double scale,
									double offset);

			int					fFormat = -1;
			int					fSourceWidth = 0;
			int					fSourceHeight = 0;
			int					fWidth = 0;
			int					fHeight = 0;
			int					fChromaLocation = -1;
			bool				fLumaAsIs = false;	// the window as large
			Taps				fLumaColumns;
			Taps				fLumaRows;
			Taps				fChromaColumns;
			Taps				fChromaRows;
};

}	// namespace airtime

#endif	// AIRTIME_YUV_SCALER_H
