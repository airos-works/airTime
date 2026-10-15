/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "FrameRenderer.h"

#include <atomic>

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

extern "C" {
#include <libavutil/mastering_display_metadata.h>
}

#include "WorkerPool.h"
#include "YuvScaler.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif


namespace airtime {


static void
no_free(void* opaque, uint8_t* data)
{
}


FrameRenderer::FrameRenderer()
	:
	fContext(NULL),
	fSourceWidth(0),
	fSourceHeight(0),
	fSourceFormat(-1),
	fColorSpace(-1),
	fRange(-1),
	fWidth(0),
	fHeight(0),
	fHDR(false),
	fSource(av_frame_alloc()),
	fTarget(av_frame_alloc()),
	fTransferKind(-1),
	fPeak(0),
	fTraceScaling(0),
	fTraceMapping(0)
{
}


FrameRenderer::~FrameRenderer()
{
	sws_freeContext(fContext);
	av_frame_free(&fSource);
	av_frame_free(&fTarget);
}


static int
sws_colorspace(const AVFrame* frame)
{
	switch (frame->colorspace) {
		case AVCOL_SPC_BT709:
			return SWS_CS_ITU709;
		case AVCOL_SPC_BT470BG:
		case AVCOL_SPC_SMPTE170M:
			return SWS_CS_ITU601;
		case AVCOL_SPC_SMPTE240M:
			return SWS_CS_SMPTE240M;
		case AVCOL_SPC_FCC:
			return SWS_CS_FCC;
		case AVCOL_SPC_BT2020_NCL:
		case AVCOL_SPC_BT2020_CL:
			return SWS_CS_BT2020;
		default:
			// Unlabelled: high definition is 709, the rest 601.
			return frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
	}
}


static bool
is_hdr(const AVFrame* frame)
{
	return frame->color_trc == AVCOL_TRC_SMPTE2084
		|| frame->color_trc == AVCOL_TRC_ARIB_STD_B67;
}


bool
FrameRenderer::_Prepare(const AVFrame* frame, int width, int height, bool hdr)
{
	int colorSpace = sws_colorspace(frame);
	AVPixelFormat format = (AVPixelFormat)frame->format;
	int range = frame->color_range == AVCOL_RANGE_JPEG
		|| format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P
		|| format == AV_PIX_FMT_YUVJ444P ? 1 : 0;

	if (fContext != NULL && fSourceWidth == frame->width
		&& fSourceHeight == frame->height && fSourceFormat == frame->format
		&& fWidth == width && fHeight == height && fColorSpace == colorSpace
		&& fRange == range && fHDR == hdr) {
		return true;
	}
	sws_freeContext(fContext);
	fContext = sws_alloc_context();
	if (fContext == NULL)
		return false;

	// Bicubic when enlarging looks better; area averaging when shrinking a
	// lot keeps fine detail from flickering. On the ARM boards swscale's
	// fast bilinear path is the one with vector code, and the others cost
	// twice as much per picture.
	int flags = SWS_BICUBIC;
	if (width < frame->width / 2)
		flags = SWS_AREA;
	else if (width <= frame->width)
		flags = SWS_BILINEAR;
#if defined(__aarch64__)
	if (width != frame->width || height != frame->height)
		flags = SWS_FAST_BILINEAR;
#endif
	const char* forced = getenv("AIRTIME_SWS_FLAGS");
	if (forced != NULL) {
		if (strcmp(forced, "fast") == 0)
			flags = SWS_FAST_BILINEAR;
		else if (strcmp(forced, "bilinear") == 0)
			flags = SWS_BILINEAR;
		else if (strcmp(forced, "area") == 0)
			flags = SWS_AREA;
		else if (strcmp(forced, "point") == 0)
			flags = SWS_POINT;
		else if (strcmp(forced, "bicubic") == 0)
			flags = SWS_BICUBIC;
	}

	unsigned cpus = std::thread::hardware_concurrency();
	int threads = cpus > 8 ? 8 : cpus > 0 ? (int)cpus : 4;
	if (getenv("AIRTIME_SWS_THREADS") != NULL)
		threads = atoi(getenv("AIRTIME_SWS_THREADS"));
	av_opt_set_int(fContext, "srcw", frame->width, 0);
	av_opt_set_int(fContext, "srch", frame->height, 0);
	av_opt_set_int(fContext, "src_format", frame->format, 0);
	av_opt_set_int(fContext, "dstw", width, 0);
	av_opt_set_int(fContext, "dsth", height, 0);
	// High dynamic range goes through 16 bit R'G'B', still in its own
	// transfer and primaries, to be mapped down afterwards.
	av_opt_set_int(fContext, "dst_format",
		hdr ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_BGRA, 0);
	av_opt_set_int(fContext, "sws_flags", flags, 0);
	av_opt_set_int(fContext, "threads", threads, 0);
	if (sws_init_context(fContext, NULL, NULL) < 0) {
		sws_freeContext(fContext);
		fContext = NULL;
		return false;
	}
	const int* coefficients = sws_getCoefficients(colorSpace);
	sws_setColorspaceDetails(fContext, coefficients, range,
		sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

	fSourceWidth = frame->width;
	fSourceHeight = frame->height;
	fSourceFormat = frame->format;
	fWidth = width;
	fHeight = height;
	fColorSpace = colorSpace;
	fRange = range;
	fHDR = hdr;
	return true;
}


bool
FrameRenderer::Render(const AVFrame* frame, uint8* bits, int32 bytesPerRow,
	int width, int height)
{
	if (frame == NULL || frame->width <= 0 || frame->height <= 0
		|| width <= 0 || height <= 0) {
		return false;
	}

	// Pixels from a hardware decoder at the size they are shown at: copy.
	if (frame->format == AV_PIX_FMT_BGRA && frame->width == width
		&& frame->height == height) {
		for (int y = 0; y < height; y++) {
			memcpy(bits + y * bytesPerRow,
				frame->data[0] + y * frame->linesize[0], width * 4);
		}
		return true;
	}

	if (is_hdr(frame))
		return _RenderHDR(frame, bits, bytesPerRow, width, height);
	if (_RenderSDR(frame, bits, bytesPerRow, width, height))
		return true;

	if (!_Prepare(frame, width, height, false))
		return false;

	// swscale's threads only work through sws_scale_frame(), which wants
	// reference counted frames on both sides.
	av_frame_unref(fTarget);
	fTarget->format = AV_PIX_FMT_BGRA;
	fTarget->width = width;
	fTarget->height = height;
	fTarget->data[0] = bits;
	fTarget->linesize[0] = bytesPerRow;
	fTarget->buf[0] = av_buffer_create(bits, bytesPerRow * height, no_free,
		NULL, 0);
	if (fTarget->buf[0] == NULL)
		return false;

	av_frame_unref(fSource);
	if (av_frame_ref(fSource, frame) < 0) {
		av_frame_unref(fTarget);
		return false;
	}
	int result = sws_scale_frame(fContext, fTarget, fSource);
	av_frame_unref(fSource);
	av_frame_unref(fTarget);
	if (result < 0) {
		// Fall back to the plain, single threaded call.
		const uint8* const* source = frame->data;
		uint8* target[4] = {bits, NULL, NULL, NULL};
		int targetStride[4] = {bytesPerRow, 0, 0, 0};
		return sws_scale(fContext, source, frame->linesize, 0, frame->height,
			target, targetStride) > 0;
	}
	return true;
}

/*!	Eight bit 4:2:0 with the scaler made for it, where that is the quicker
	one: on ARM. AIRTIME_SDR_SWSCALE and AIRTIME_SDR_SCALER choose. */
bool
FrameRenderer::_RenderSDR(const AVFrame* frame, uint8* bits,
	int32 bytesPerRow, int width, int height)
{
	bool wanted = getenv("AIRTIME_SDR_SWSCALE") == NULL;
#if !defined(__aarch64__)
	wanted = wanted && getenv("AIRTIME_SDR_SCALER") != NULL;
#endif
	if (!wanted || !SdrScaler::Handles(frame, width, height)
		|| !fSdrScaler.Prepare(frame, width, height)) {
		return false;
	}

	if (fPool.get() == NULL)
		fPool.reset(new WorkerPool());
	fPool->Run(height, [&](int first, int last) {
		static thread_local SdrScaler::Scratch scratch;
		for (int y = first; y < last; y++) {
			fSdrScaler.RenderRow(frame, y, bits + (size_t)y * bytesPerRow,
				scratch);
		}
	});
	return true;
}


// #pragma mark - high dynamic range


// SMPTE ST 2084 (PQ)
static double
pq_to_nits(double value)
{
	const double m1 = 2610.0 / 16384;
	const double m2 = 2523.0 / 4096 * 128;
	const double c1 = 3424.0 / 4096;
	const double c2 = 2413.0 / 4096 * 32;
	const double c3 = 2392.0 / 4096 * 32;
	double power = pow(value, 1 / m2);
	double numerator = std::max(power - c1, 0.0);
	return 10000 * pow(numerator / (c2 - c3 * power), 1 / m1);
}


// ARIB STD-B67 (HLG): the inverse of the camera's curve, scene light 0-1.
static double
hlg_to_scene(double value)
{
	const double a = 0.17883277;
	const double b = 1 - 4 * a;
	const double c = 0.5 - a * log(4 * a);
	if (value <= 0.5)
		return value * value / 3;
	return (exp((value - c) / a) + b) / 12;
}


static const double kReferenceWhite = 203.0;
	// nits that SDR white stands for (ITU-R BT.2408)


void
FrameRenderer::_PrepareToneMapping(const AVFrame* frame)
{
	// How bright the film gets: the content light level, else the mastering
	// display, else a common 1000 nits.
	double peakNits = 0;
	const AVFrameSideData* light = av_frame_get_side_data(frame,
		AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
	if (light != NULL) {
		const AVContentLightMetadata* metadata
			= (const AVContentLightMetadata*)light->data;
		peakNits = metadata->MaxCLL;
	}
	if (peakNits <= 0) {
		const AVFrameSideData* mastering = av_frame_get_side_data(frame,
			AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
		if (mastering != NULL) {
			const AVMasteringDisplayMetadata* metadata
				= (const AVMasteringDisplayMetadata*)mastering->data;
			if (metadata->has_luminance)
				peakNits = av_q2d(metadata->max_luminance);
		}
	}
	int kind = frame->color_trc;
	if (kind == AVCOL_TRC_ARIB_STD_B67)
		peakNits = 1000;
	if (peakNits <= 0) {
		// Keep what an earlier picture said, if anything did.
		peakNits = fPeak > 0 ? fPeak * kReferenceWhite : 1000;
	}
	peakNits = std::max(peakNits, 400.0);
	float peak = (float)(peakNits / kReferenceWhite);

	if (kind == fTransferKind && fabsf(peak - fPeak) < 0.01f)
		return;
	fTransferKind = kind;
	fPeak = peak;

	fTransfer.resize(4096);
	for (int i = 0; i < 4096; i++) {
		double code = i / 4095.0;
		double light;
		if (kind == AVCOL_TRC_SMPTE2084)
			light = pq_to_nits(code) / kReferenceWhite;
		else
			light = hlg_to_scene(code);
		fTransfer[i] = (float)light;
	}

	if (fEncode.empty()) {
		fEncode.resize(4096);
		for (int i = 0; i < 4096; i++) {
			double linear = i / 4095.0;
			double encoded = linear <= 0.0031308 ? linear * 12.92
				: 1.055 * pow(linear, 1 / 2.4) - 0.055;
			fEncode[i] = (uint8)std::min(255.0, encoded * 255 + 0.5);
		}
	}
}


namespace {

/*!	Linear light in BT.2020 to sRGB pixels: the gamut changed to BT.709 and
	the highlights above a knee compressed so that the film's peak lands on
	white. */
struct ToneMap {
	const float*	transfer;	// R'G'B' in 4096 steps -> linear, 1 = SDR white
	const uint8*	encode;		// linear in 4096 steps -> sRGB
	bool			hlg;
	float			knee;
	float			inverseExcessPeakSquared;

	inline void Map(float r, float g, float b, uint8* out) const
	{
		if (hlg) {
			// The display's system gamma (1.2 at 1000 nits).
			float luminance = 0.2627f * r + 0.6780f * g + 0.0593f * b;
			float gain = 1000.0f / 203.0f
				* powf(std::max(luminance, 1e-6f), 0.2f);
			r *= gain;
			g *= gain;
			b *= gain;
		}
		float r709 = 1.6605f * r - 0.5876f * g - 0.0728f * b;
		float g709 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
		float b709 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
		r709 = std::max(r709, 0.0f);
		g709 = std::max(g709, 0.0f);
		b709 = std::max(b709, 0.0f);

		// On the largest channel, so that hues hold. Linear up to the knee;
		// above it the excess is compressed (Reinhard, extended, on the
		// excess alone, which keeps the slope continuous at the knee).
		float maximum = std::max(r709, std::max(g709, b709));
		if (maximum > knee) {
			float excess = (maximum - knee) / (1 - knee);
			float mapped = knee + (1 - knee) * excess
				* (1 + excess * inverseExcessPeakSquared) / (1 + excess);
			float scale = mapped / maximum;
			r709 *= scale;
			g709 *= scale;
			b709 *= scale;
		}
		out[2] = encode[std::min(4095, (int)(r709 * 4095 + 0.5f))];
		out[1] = encode[std::min(4095, (int)(g709 * 4095 + 0.5f))];
		out[0] = encode[std::min(4095, (int)(b709 * 4095 + 0.5f))];
		out[3] = 255;
	}
};


inline int
code_index(float value)
{
	return value <= 0 ? 0 : value >= 1 ? 4095 : (int)(value * 4095 + 0.5f);
}


/*!	Y'CbCr, as the scaler gives it (ten-bit codes times 64), to R'G'B'. */
struct ColorMatrix {
	float			lumaOffset;
	float			lumaScale;
	float			chromaScale;
	float			redCr;
	float			greenCb;
	float			greenCr;
	float			blueCb;

	void Set(const AVFrame* frame)
	{
		float kr = 0.2627f;
		float kb = 0.0593f;
		switch (frame->colorspace) {
			case AVCOL_SPC_BT709:
				kr = 0.2126f;
				kb = 0.0722f;
				break;
			case AVCOL_SPC_BT470BG:
			case AVCOL_SPC_SMPTE170M:
				kr = 0.299f;
				kb = 0.114f;
				break;
			default:
				break;
		}
		float kg = 1 - kr - kb;
		bool full = frame->color_range == AVCOL_RANGE_JPEG;
		lumaOffset = full ? 0 : 64 * 64;
		lumaScale = 1.0f / ((full ? 1023 : 876) * 64);
		chromaScale = 1.0f / ((full ? 1023 : 896) * 64);
		redCr = 2 * (1 - kr);
		greenCb = -2 * kb * (1 - kb) / kg;
		greenCr = -2 * kr * (1 - kr) / kg;
		blueCb = 2 * (1 - kb);
	}
};


/*!	A row of scaled Y'CbCr to pixels. */
void
map_row(const ToneMap& map, const ColorMatrix& matrix, const uint16* luma,
	const uint16* cb, const uint16* cr, int width, uint8* out)
{
	const float* transfer = map.transfer;
	int x = 0;
#if defined(__aarch64__)
	// In four passes over the row: the arithmetic in vectors, four pixels
	// at a time, and the table lookups each on its own. Done together, four
	// pixels at a time, the lookups chained loads lane by lane into vectors
	// and the pixels were put together lane by lane, and a loop body as
	// long as the Cortex-A76's reorder window could not overlap one group's
	// chains with the next: 38-39 cycles a pixel there, 27-28 in passes,
	// the pixels the same. (HLG's gain is left to the plain code.)
	if (!map.hlg && width >= 4) {
		int vectorWidth = width & ~3;
		static thread_local std::vector<uint16> sIndices;
		static thread_local std::vector<float> sLight;
		sIndices.resize((size_t)vectorWidth * 3);
		sLight.resize((size_t)vectorWidth * 3);
		uint16* redIndex = sIndices.data();
		uint16* greenIndex = redIndex + vectorWidth;
		uint16* blueIndex = greenIndex + vectorWidth;
		float* redLight = sLight.data();
		float* greenLight = redLight + vectorWidth;
		float* blueLight = greenLight + vectorWidth;

		const float32x4_t zero = vdupq_n_f32(0);
		const float32x4_t one = vdupq_n_f32(1);
		const float32x4_t steps = vdupq_n_f32(4095);
		const float32x4_t lumaOffset = vdupq_n_f32(matrix.lumaOffset);
		const float32x4_t chromaOffset = vdupq_n_f32(32768);
		const float lumaScale = matrix.lumaScale;
		const float chromaScale = matrix.chromaScale;
		const float redCr = matrix.redCr;
		const float greenCb = matrix.greenCb;
		const float greenCr = matrix.greenCr;
		const float blueCb = matrix.blueCb;

		// Y'CbCr to the R'G'B' codes' steps in the transfer table
		for (int i = 0; i < vectorWidth; i += 4) {
			float32x4_t l = vmulq_n_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(
				vld1_u16(luma + i))), lumaOffset), lumaScale);
			float32x4_t u = vmulq_n_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(
				vld1_u16(cb + i))), chromaOffset), chromaScale);
			float32x4_t v = vmulq_n_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(
				vld1_u16(cr + i))), chromaOffset), chromaScale);
			float32x4_t red = vfmaq_n_f32(l, v, redCr);
			float32x4_t green = vfmaq_n_f32(vfmaq_n_f32(l, u, greenCb), v,
				greenCr);
			float32x4_t blue = vfmaq_n_f32(l, u, blueCb);
			vst1_u16(redIndex + i, vmovn_u32(vcvtnq_u32_f32(vmulq_f32(
				vminq_f32(vmaxq_f32(red, zero), one), steps))));
			vst1_u16(greenIndex + i, vmovn_u32(vcvtnq_u32_f32(vmulq_f32(
				vminq_f32(vmaxq_f32(green, zero), one), steps))));
			vst1_u16(blueIndex + i, vmovn_u32(vcvtnq_u32_f32(vmulq_f32(
				vminq_f32(vmaxq_f32(blue, zero), one), steps))));
		}

		// to light
		for (int i = 0; i < vectorWidth; i++) {
			redLight[i] = transfer[redIndex[i]];
			greenLight[i] = transfer[greenIndex[i]];
			blueLight[i] = transfer[blueIndex[i]];
		}

		// BT.2020 to BT.709, the tone curve, and the steps of the sRGB
		// table
		const float32x4_t knee = vdupq_n_f32(map.knee);
		const float overKnee = 1 / (1 - map.knee);
		const float aboveKnee = 1 - map.knee;
		const float inverseExcessPeakSquared = map.inverseExcessPeakSquared;
		for (int i = 0; i < vectorWidth; i += 4) {
			float32x4_t r = vld1q_f32(redLight + i);
			float32x4_t g = vld1q_f32(greenLight + i);
			float32x4_t b = vld1q_f32(blueLight + i);
			float32x4_t r709 = vmaxq_f32(vfmaq_n_f32(vfmaq_n_f32(
				vmulq_n_f32(r, 1.6605f), g, -0.5876f), b, -0.0728f), zero);
			float32x4_t g709 = vmaxq_f32(vfmaq_n_f32(vfmaq_n_f32(
				vmulq_n_f32(r, -0.1246f), g, 1.1329f), b, -0.0083f), zero);
			float32x4_t b709 = vmaxq_f32(vfmaq_n_f32(vfmaq_n_f32(
				vmulq_n_f32(r, -0.0182f), g, -0.1006f), b, 1.1187f), zero);

			// As in ToneMap::Map(), with its two divisions made one.
			float32x4_t maximum = vmaxq_f32(r709, vmaxq_f32(g709, b709));
			float32x4_t excess = vmulq_n_f32(vsubq_f32(maximum, knee),
				overKnee);
			float32x4_t onePlus = vaddq_f32(one, excess);
			float32x4_t mapped = vfmaq_f32(vmulq_f32(knee, onePlus),
				vmulq_n_f32(excess, aboveKnee),
				vfmaq_n_f32(one, excess, inverseExcessPeakSquared));
			float32x4_t scale = vbslq_f32(vcgtq_f32(maximum, knee),
				vdivq_f32(mapped, vmulq_f32(maximum, onePlus)), one);

			vst1_u16(redIndex + i, vmovn_u32(vcvtnq_u32_f32(vminq_f32(
				vmulq_f32(vmulq_f32(r709, scale), steps), steps))));
			vst1_u16(greenIndex + i, vmovn_u32(vcvtnq_u32_f32(vminq_f32(
				vmulq_f32(vmulq_f32(g709, scale), steps), steps))));
			vst1_u16(blueIndex + i, vmovn_u32(vcvtnq_u32_f32(vminq_f32(
				vmulq_f32(vmulq_f32(b709, scale), steps), steps))));
		}

		// to pixels
		const uint8* encode = map.encode;
		for (int i = 0; i < vectorWidth; i++, out += 4) {
			uint32 pixel = 0xff000000u | (uint32)encode[redIndex[i]] << 16
				| (uint32)encode[greenIndex[i]] << 8
				| encode[blueIndex[i]];
			memcpy(out, &pixel, sizeof(pixel));
		}
		x = vectorWidth;
	}
#endif
	for (; x < width; x++, out += 4) {
		float l = (luma[x] - matrix.lumaOffset) * matrix.lumaScale;
		float u = (cb[x] - 32768.0f) * matrix.chromaScale;
		float v = (cr[x] - 32768.0f) * matrix.chromaScale;
		map.Map(transfer[code_index(l + matrix.redCr * v)],
			transfer[code_index(l + matrix.greenCb * u + matrix.greenCr * v)],
			transfer[code_index(l + matrix.blueCb * u)], out);
	}
}


/*!	The scaler's vector code is NEON: on ARM it is several times quicker than
	swscale's 16 bit R'G'B' and the tone mapping after it, while on the X399
	swscale's own SIMD (and its plain conversion when the picture is not
	scaled) beats the scaler's plain code. AIRTIME_HDR_SWSCALE and
	AIRTIME_HDR_SCALER choose. */
bool
scaler_path_wanted()
{
	if (getenv("AIRTIME_HDR_SWSCALE") != NULL)
		return false;
#if defined(__aarch64__)
	return true;
#else
	return getenv("AIRTIME_HDR_SCALER") != NULL;
#endif
}


struct ScaledRows {
	YuvScaler::Scratch	scratch;
	std::vector<uint16>	luma;
	std::vector<uint16>	cb;
	std::vector<uint16>	cr;
};

}	// namespace


bool
FrameRenderer::_RenderHDR(const AVFrame* frame, uint8* bits,
	int32 bytesPerRow, int width, int height)
{
	_PrepareToneMapping(frame);
	if (fPool.get() == NULL)
		fPool.reset(new WorkerPool());

	ToneMap map;
	map.transfer = fTransfer.data();
	map.encode = fEncode.data();
	map.hlg = fTransferKind == AVCOL_TRC_ARIB_STD_B67;
	map.knee = 0.6f;
	float excessPeak = std::max((fPeak - map.knee) / (1 - map.knee), 1.01f);
	map.inverseExcessPeakSquared = 1.0f / (excessPeak * excessPeak);

	bigtime_t traceStart = system_time();
	bigtime_t traceScaled = traceStart;
	if (scaler_path_wanted() && YuvScaler::Handles(frame)
		&& fScaler.Prepare(frame, width, height)) {
		// Ten-bit 4:2:0, as the hardware decoders and libavcodec give HDR
		// films: scaled in Y'CbCr, then made into pixels a row at a time,
		// all in one pass over the target.
		ColorMatrix matrix;
		matrix.Set(frame);
		static bool sTraceRows = getenv("AIRTIME_TRACE_HDR") != NULL;
		std::atomic<bigtime_t> scaling(0), mapping(0);
		fPool->Run(height, [&](int first, int last) {
			static thread_local ScaledRows rows;
			rows.luma.resize(width);
			rows.cb.resize(width);
			rows.cr.resize(width);
			bigtime_t scaled = 0, mapped = 0;
			for (int y = first; y < last; y++) {
				bigtime_t start = sTraceRows ? system_time() : 0;
				fScaler.ScaleRow(frame, y, rows.luma.data(), rows.cb.data(),
					rows.cr.data(), rows.scratch);
				bigtime_t middle = sTraceRows ? system_time() : 0;
				map_row(map, matrix, rows.luma.data(), rows.cb.data(),
					rows.cr.data(), width, bits + y * bytesPerRow);
				if (sTraceRows) {
					scaled += middle - start;
					mapped += system_time() - middle;
				}
			}
			scaling += scaled;
			mapping += mapped;
		});
		fTraceScaling += scaling;
		fTraceMapping += mapping;
	} else {
		// Anything else goes through 16 bit R'G'B' from swscale, still in
		// its own transfer and primaries, to be mapped down afterwards.
		if (!_Prepare(frame, width, height, true))
			return false;
		fLinear.resize((size_t)width * height * 3);
		uint8* target[4] = {(uint8*)fLinear.data(), NULL, NULL, NULL};
		int targetStride[4] = {width * 6, 0, 0, 0};

		av_frame_unref(fTarget);
		fTarget->format = AV_PIX_FMT_RGB48LE;
		fTarget->width = width;
		fTarget->height = height;
		fTarget->data[0] = target[0];
		fTarget->linesize[0] = targetStride[0];
		fTarget->buf[0] = av_buffer_create(target[0], width * 6 * height,
			no_free, NULL, 0);
		av_frame_unref(fSource);
		int result = -1;
		if (fTarget->buf[0] != NULL && av_frame_ref(fSource, frame) >= 0)
			result = sws_scale_frame(fContext, fTarget, fSource);
		av_frame_unref(fSource);
		av_frame_unref(fTarget);
		if (result < 0 && sws_scale(fContext, frame->data, frame->linesize, 0,
				frame->height, target, targetStride) <= 0) {
			return false;
		}
		traceScaled = system_time();

		const uint16* linear = fLinear.data();
		fPool->Run(height, [&](int first, int last) {
			for (int y = first; y < last; y++) {
				const uint16* in = linear + (size_t)y * width * 3;
				uint8* out = bits + y * bytesPerRow;
				for (int x = 0; x < width; x++, in += 3, out += 4) {
					map.Map(map.transfer[in[0] >> 4], map.transfer[in[1] >> 4],
						map.transfer[in[2] >> 4], out);
				}
			}
		});
	}

	static bool sTrace = getenv("AIRTIME_TRACE_HDR") != NULL;
	if (sTrace) {
		static bigtime_t sScale, sMap;
		static int sCount;
		sScale += traceScaled - traceStart;
		sMap += system_time() - traceScaled;
		if (++sCount == 48) {
			fprintf(stderr, "airTime: HDR %s %dx%d -> %dx%d: swscale %.1f ms,"
				" tone mapping %.1f ms (processor time: scaling %.1f ms,"
				" making pixels %.1f ms)\n", av_get_pix_fmt_name(
					(AVPixelFormat)frame->format), frame->width, frame->height,
				width, height, sScale / 48000.0, sMap / 48000.0,
				fTraceScaling / 48000.0, fTraceMapping / 48000.0);
			sScale = sMap = 0;
			fTraceScaling = fTraceMapping = 0;
			sCount = 0;
		}
	}
	return true;
}

}	// namespace airtime
