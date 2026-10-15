/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "YuvScaler.h"

#include <algorithm>
#include <math.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif


namespace airtime {


static const int kWeightBits = 14;
static const int kMaxTaps = 32;
	// a triangle as wide as 15 source samples: an 8K film in a small window
static const int kWindow = 32;
	// source samples eight target samples can draw on together


/*!	One row of the vertical pass: out[x] is the weighted sum of the rows at
	x. Sixteen-bit samples are shifted down by kWeightBits, ten-bit ones (in
	the low bits) by 8, which leaves both as ten bits times 64. */
static void
filter_rows(const uint16* const* rows, const uint16* weights, int count,
	int samples, int shift, uint16* out)
{
	int x = 0;
#if defined(__aarch64__)
	const int32x4_t down = vdupq_n_s32(-shift);
	const uint32x4_t rounding = vdupq_n_u32(1u << (shift - 1));
	for (; x + 8 <= samples; x += 8) {
		uint32x4_t low = rounding;
		uint32x4_t high = rounding;
		for (int k = 0; k < count; k++) {
			uint16x8_t value = vld1q_u16(rows[k] + x);
			low = vmlal_n_u16(low, vget_low_u16(value), weights[k]);
			high = vmlal_high_n_u16(high, value, weights[k]);
		}
		vst1q_u16(out + x, vcombine_u16(vmovn_u32(vshlq_u32(low, down)),
			vmovn_u32(vshlq_u32(high, down))));
	}
#endif
	for (; x < samples; x++) {
		uint32 sum = 1u << (shift - 1);
		for (int k = 0; k < count; k++)
			sum += (uint32)weights[k] * rows[k][x];
		out[x] = (uint16)(sum >> shift);
	}
}


/*!	The vertical pass over P010's chroma, Cb and Cr in pairs, which it
	separates. */
static void
filter_chroma_rows(const uint16* const* rows, const uint16* weights,
	int count, int pairs, uint16* cb, uint16* cr)
{
	const uint32 rounding = 1u << (kWeightBits - 1);
	int x = 0;
#if defined(__aarch64__)
	for (; x + 8 <= pairs; x += 8) {
		uint32x4_t sums[4];
		for (uint32x4_t& sum : sums)
			sum = vdupq_n_u32(rounding);
		for (int k = 0; k < count; k++) {
			uint16x8x2_t value = vld2q_u16(rows[k] + 2 * x);
			sums[0] = vmlal_n_u16(sums[0], vget_low_u16(value.val[0]),
				weights[k]);
			sums[1] = vmlal_high_n_u16(sums[1], value.val[0], weights[k]);
			sums[2] = vmlal_n_u16(sums[2], vget_low_u16(value.val[1]),
				weights[k]);
			sums[3] = vmlal_high_n_u16(sums[3], value.val[1], weights[k]);
		}
		vst1q_u16(cb + x, vcombine_u16(vshrn_n_u32(sums[0], kWeightBits),
			vshrn_n_u32(sums[1], kWeightBits)));
		vst1q_u16(cr + x, vcombine_u16(vshrn_n_u32(sums[2], kWeightBits),
			vshrn_n_u32(sums[3], kWeightBits)));
	}
#endif
	for (; x < pairs; x++) {
		uint32 sumCb = rounding;
		uint32 sumCr = rounding;
		for (int k = 0; k < count; k++) {
			sumCb += (uint32)weights[k] * rows[k][2 * x];
			sumCr += (uint32)weights[k] * rows[k][2 * x + 1];
		}
		cb[x] = (uint16)(sumCb >> kWeightBits);
		cr[x] = (uint16)(sumCr >> kWeightBits);
	}
}


/*!	The horizontal pass. `in` has 32 samples to spare at its end. */
static void
filter_columns(const uint16* in, const YuvScaler::Taps& taps, int width,
	uint16* out)
{
	int count = taps.count;
#if defined(__aarch64__)
	if (taps.grouped) {
		// Eight target samples at once, their sources picked out of a
		// window of 32 by table lookups.
		const uint8* indices = taps.groupIndices.data();
		const uint16* weights = taps.groupWeights.data();
		const uint32x4_t rounding = vdupq_n_u32(1u << (kWeightBits - 1));
		for (int x = 0; x < width; x += 8) {
			const uint8* window = (const uint8*)(in + taps.groupFirst[x / 8]);
			uint8x16x4_t table;
			table.val[0] = vld1q_u8(window);
			table.val[1] = vld1q_u8(window + 16);
			table.val[2] = vld1q_u8(window + 32);
			table.val[3] = vld1q_u8(window + 48);
			uint32x4_t low = rounding;
			uint32x4_t high = rounding;
			for (int k = 0; k < count; k++, indices += 16, weights += 8) {
				uint16x8_t value = vreinterpretq_u16_u8(vqtbl4q_u8(table,
					vld1q_u8(indices)));
				uint16x8_t weight = vld1q_u16(weights);
				low = vmlal_u16(low, vget_low_u16(value),
					vget_low_u16(weight));
				high = vmlal_high_u16(high, value, weight);
			}
			uint16x8_t result = vcombine_u16(vshrn_n_u32(low, kWeightBits),
				vshrn_n_u32(high, kWeightBits));
			if (x + 8 <= width)
				vst1q_u16(out + x, result);
			else {
				uint16 last[8];
				vst1q_u16(last, result);
				memcpy(out + x, last, (width - x) * sizeof(uint16));
			}
		}
		return;
	}
#endif
	const uint16* weights = taps.weights.data();
	for (int i = 0; i < width; i++, weights += count) {
		const uint16* source = in + taps.first[i];
		uint32 sum = 1u << (kWeightBits - 1);
		for (int k = 0; k < count; k++)
			sum += (uint32)weights[k] * source[k];
		out[i] = (uint16)(sum >> kWeightBits);
	}
}


bool
YuvScaler::Handles(const AVFrame* frame)
{
	return (frame->format == AV_PIX_FMT_P010LE
			|| frame->format == AV_PIX_FMT_YUV420P10LE)
		&& frame->width > 1 && frame->height > 1;
}


/*!	Target sample i is centred on source position (i + 0.5) * scale + offset
	and takes the source samples within the triangle's radius, the edge
	samples standing in for those beyond the picture. */
bool
YuvScaler::_MakeTaps(Taps& taps, int sourceSize, int targetSize,
	double scale, double offset)
{
	double radius = std::max(1.0, scale);
	int count = std::min((int)ceil(2 * radius) + 1, sourceSize);
	if (count > kMaxTaps)
		return false;
	taps.count = count;
	taps.first.resize(targetSize);
	taps.weights.assign((size_t)targetSize * count, 0);
	std::vector<double> weights(count);
	for (int i = 0; i < targetSize; i++) {
		double center = (i + 0.5) * scale + offset;
		int lowest = (int)floor(center - radius) + 1;
		int first = std::max(0, std::min(lowest, sourceSize - count));
		std::fill(weights.begin(), weights.end(), 0.0);
		double total = 0;
		for (int t = lowest; t < center + radius; t++) {
			double weight = 1 - fabs(t - center) / radius;
			if (weight <= 0)
				continue;
			int index = std::max(0, std::min(t, sourceSize - 1)) - first;
			if (index < 0 || index >= count)
				continue;
			weights[index] += weight;
			total += weight;
		}
		taps.first[i] = first;
		uint16* quantized = &taps.weights[(size_t)i * count];
		if (total <= 0) {
			quantized[std::max(0, std::min((int)lround(center), sourceSize - 1)
				- first)] = 1 << kWeightBits;
			continue;
		}
		int sum = 0;
		int largest = 0;
		for (int k = 0; k < count; k++) {
			quantized[k] = (uint16)lround(weights[k] / total
				* (1 << kWeightBits));
			sum += quantized[k];
			if (quantized[k] > quantized[largest])
				largest = k;
		}
		quantized[largest] += (1 << kWeightBits) - sum;
	}

	int groups = (targetSize + 7) / 8;
	taps.grouped = true;
	taps.groupFirst.resize(groups);
	taps.groupIndices.assign((size_t)groups * count * 16, 0);
	taps.groupWeights.assign((size_t)groups * count * 8, 0);
	for (int group = 0; group < groups; group++) {
		int base = taps.first[group * 8];
		taps.groupFirst[group] = base;
		for (int lane = 0; lane < 8; lane++) {
			int i = group * 8 + lane;
			if (i >= targetSize)
				break;
			int offset = taps.first[i] - base;
			if (offset + count > kWindow) {
				taps.grouped = false;
				break;
			}
			for (int k = 0; k < count; k++) {
				size_t tap = (size_t)group * count + k;
				taps.groupIndices[tap * 16 + 2 * lane] = 2 * (offset + k);
				taps.groupIndices[tap * 16 + 2 * lane + 1] = 2 * (offset + k) + 1;
				taps.groupWeights[tap * 8 + lane]
					= taps.weights[(size_t)i * count + k];
			}
		}
	}
	if (!taps.grouped) {
		taps.groupFirst.clear();
		taps.groupIndices.clear();
		taps.groupWeights.clear();
	}
	return true;
}


bool
YuvScaler::Prepare(const AVFrame* frame, int width, int height)
{
	if (fFormat == frame->format && fSourceWidth == frame->width
		&& fSourceHeight == frame->height && fWidth == width
		&& fHeight == height && fChromaLocation == frame->chroma_location) {
		return true;
	}
	fFormat = -1;

	// Where chroma sits between the luma samples: (MPEG-2's) left of a
	// pair and halfway down, unless the stream says otherwise.
	double across = -0.25;
	double down = -0.5;
	switch (frame->chroma_location) {
		case AVCHROMA_LOC_CENTER:
			across = -0.5;
			break;
		case AVCHROMA_LOC_TOPLEFT:
			down = -0.25;
			break;
		case AVCHROMA_LOC_TOP:
			across = -0.5;
			down = -0.25;
			break;
		case AVCHROMA_LOC_BOTTOMLEFT:
			down = -0.75;
			break;
		case AVCHROMA_LOC_BOTTOM:
			across = -0.5;
			down = -0.75;
			break;
		default:
			break;
	}
	double scaleX = (double)frame->width / width;
	double scaleY = (double)frame->height / height;
	if (!_MakeTaps(fLumaColumns, frame->width, width, scaleX, -0.5)
		|| !_MakeTaps(fLumaRows, frame->height, height, scaleY, -0.5)
		|| !_MakeTaps(fChromaColumns, (frame->width + 1) / 2, width,
			scaleX / 2, across)
		|| !_MakeTaps(fChromaRows, (frame->height + 1) / 2, height,
			scaleY / 2, down)) {
		return false;
	}
	fFormat = frame->format;
	fLumaAsIs = width == frame->width && height == frame->height;
	fSourceWidth = frame->width;
	fSourceHeight = frame->height;
	fWidth = width;
	fHeight = height;
	fChromaLocation = frame->chroma_location;
	return true;
}


void
YuvScaler::ScaleRow(const AVFrame* frame, int y, uint16* luma, uint16* cb,
	uint16* cr, Scratch& scratch) const
{
	bool p010 = fFormat == AV_PIX_FMT_P010LE;
	int shift = p010 ? kWeightBits : kWeightBits - 6;
	const uint16* rows[kMaxTaps];

	if (fLumaAsIs) {
		// A picture as large as the window (a 4K film's half size pictures
		// full screen at 1080p): its luma is what the taps would give.
		const uint16* row = (const uint16*)(frame->data[0]
			+ (size_t)y * frame->linesize[0]);
		if (p010)
			memcpy(luma, row, fWidth * sizeof(uint16));
		else {
			int x = 0;
#if defined(__aarch64__)
			for (; x + 8 <= fWidth; x += 8)
				vst1q_u16(luma + x, vshlq_n_u16(vld1q_u16(row + x), 6));
#endif
			for (; x < fWidth; x++)
				luma[x] = (uint16)(row[x] << 6);
		}
	} else {
		int first = fLumaRows.first[y];
		for (int k = 0; k < fLumaRows.count; k++) {
			rows[k] = (const uint16*)(frame->data[0]
				+ (size_t)(first + k) * frame->linesize[0]);
		}
		scratch.luma.resize(fSourceWidth + kWindow);
		filter_rows(rows, &fLumaRows.weights[(size_t)y * fLumaRows.count],
			fLumaRows.count, fSourceWidth, shift, scratch.luma.data());
		filter_columns(scratch.luma.data(), fLumaColumns, fWidth, luma);
	}

	int first;

	int pairs = (fSourceWidth + 1) / 2;
	scratch.cb.resize(pairs + kWindow);
	scratch.cr.resize(pairs + kWindow);
	first = fChromaRows.first[y];
	const uint16* weights = &fChromaRows.weights[(size_t)y * fChromaRows.count];
	if (p010) {
		for (int k = 0; k < fChromaRows.count; k++) {
			rows[k] = (const uint16*)(frame->data[1]
				+ (size_t)(first + k) * frame->linesize[1]);
		}
		filter_chroma_rows(rows, weights, fChromaRows.count, pairs,
			scratch.cb.data(), scratch.cr.data());
	} else {
		for (int plane = 1; plane <= 2; plane++) {
			for (int k = 0; k < fChromaRows.count; k++) {
				rows[k] = (const uint16*)(frame->data[plane]
					+ (size_t)(first + k) * frame->linesize[plane]);
			}
			filter_rows(rows, weights, fChromaRows.count, pairs, shift,
				plane == 1 ? scratch.cb.data() : scratch.cr.data());
		}
	}
	filter_columns(scratch.cb.data(), fChromaColumns, fWidth, cb);
	filter_columns(scratch.cr.data(), fChromaColumns, fWidth, cr);
}

}	// namespace airtime
