/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "VideoDecoder.h"

#include <algorithm>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <MediaFormats.h>
#include <Path.h>
#include <Screen.h>
#include <image.h>

#include <DecoderPlugin.h>

#include "Bitstream.h"
#include "Tracks.h"


namespace airtime {

namespace {

/*!	What one add-on can decode. The Media Kit's own lookup gives a format to
	the first add-on that claims it and never falls back if that add-on then
	refuses, so the limits of each one are written down here and airTime
	only offers it streams it is known to handle.
*/
struct CodecLimits {
	AVCodecID		codec;
	int				maxWidth;
	int				maxHeight;
	int				maxBitDepth;
};

struct HardwareDecoderKind {
	const char*		leafName;
	const char*		displayName;
	CodecLimits		codecs[4];
	int				maxReferenceFrames;	// H.264 only; 0 = no limit
	// The NVDEC add-on writes Cb Y0 Cr Y1 where Haiku's B_YCbCr422 means
	// Y0 Cb Y1 Cr.
	bool			chromaFirst;
	// What NewDecoder() is asked for: sunxi_cedar's decoder 0 goes on in
	// software when the engine refuses a stream (for the Media Kit's
	// users), its decoder 1 refuses, and airTime falls back itself.
	uint			decoderIndex;
	// Gives pictures half as wide and high (P010 and I420) when asked
	// for exactly that, in the pass that copies them anyway.
	bool			halfSize;
};

const HardwareDecoderKind kKinds[] = {
	// Polaris UVD in the air/OS amdgpu driver. The add-on registers no
	// Media Kit formats, so airTime owns selection and software fallback.
	{"amduvd", "AMD UVD", {{AV_CODEC_ID_H264, 4096, 4096, 8},
		{AV_CODEC_ID_NONE, 0, 0, 0}}, 16, false, 0},
	// The same addon exposes Main/Main 10 at index 1, retaining P010
	// precision for ten-bit scaling and tone mapping in the player.
	{"amduvd", "AMD UVD", {{AV_CODEC_ID_HEVC, 4096, 4096, 10},
		{AV_CODEC_ID_NONE, 0, 0, 0}}, 0, false, 1},
	// The NVDEC add-on of this fork: eight bit 4:2:0 progressive H.264, and
	// HEVC Main and Main 10 (which the add-on does not offer the Media Kit:
	// it is only asked here, where a refusal falls back to libavcodec).
	// Before it called itself "nvdec h264 2", its H.264 picture store was one
	// short with 16 reference frames and only the first pictures came out,
	// so with an older one those streams go to libavcodec.
	{"nvdec", "NVDEC", {{AV_CODEC_ID_H264, 4096, 4096, 8},
		{AV_CODEC_ID_HEVC, 8192, 8192, 10}, {AV_CODEC_ID_NONE, 0, 0, 0}},
		15, true, 0},
	// Rockchip MPP on the RK3588: RKVDEC for H.264 and HEVC (Main and
	// Main 10), VPU981 for AV1 (Main, eight and ten bits), 4:2:0. Ten-bit
	// pictures come over as P010; an add-on too old to give them fails the
	// stream, which then goes to libavcodec.
	{"00_rockchip_mpp", "RK3588 VPU", {{AV_CODEC_ID_H264, 8192, 4320, 8},
		{AV_CODEC_ID_HEVC, 8192, 4320, 10}, {AV_CODEC_ID_AV1, 8192, 4320, 10},
		{AV_CODEC_ID_NONE, 0, 0, 0}}, 0, false, 0},
	// The Raspberry Pi's VideoCore firmware (the rpi_mmal add-on of the
	// air/OS tree): H.264 up to 1080p, eight bit, as NV12.
	{"rpi_mmal", "Raspberry Pi VideoCore", {{AV_CODEC_ID_H264, 1920, 1088, 8},
		{AV_CODEC_ID_NONE, 0, 0, 0}}, 0, false, 0},
	// The HEVC decoder block of the Raspberry Pi 4's SoC (the rpi_hevc
	// add-on of the air/OS tree): Main and Main 10, as I420 and P010.
	{"rpi_hevc", "Raspberry Pi HEVC decoder",
		{{AV_CODEC_ID_HEVC, 4096, 4096, 10}, {AV_CODEC_ID_NONE, 0, 0, 0}}, 0,
		false, 0},
	// The Cedar video engine of the Allwinner A733 (the 00_sunxi_cedar add-on
	// of the air/OS tree, Radxa Cubie A7S): progressive 4:2:0 H.264 (eight
	// bit), and HEVC Main and Main 10, up to 4096 wide; I420 and NV12, and
	// P010 for ten bits.
	// A 4K film on a 1080p screen comes over at half size: the 25 MB of a
	// 4K P010 picture took 13-17 ms to make and 31-35 ms to scale and tone
	// map into a 1471x827 window, too much at 24 a second.
	{"00_sunxi_cedar", "Allwinner Cedar", {{AV_CODEC_ID_H264, 4096, 2304, 8},
		{AV_CODEC_ID_HEVC, 4096, 2304, 10}, {AV_CODEC_ID_NONE, 0, 0, 0}}, 0,
		false, 1, true},
	// The Qualcomm SC8280XP's Iris video codec (the 00_qcom_video add-on of the
	// air/OS tree, Radxa Dragon Q8B): progressive eight bit 4:2:0 H.264 and
	// HEVC Main up to 4096x2304, as I420 and NV12; decoder 1 refuses what
	// it does not decode rather than going on in software.
	{"00_qcom_video", "Qualcomm Iris", {{AV_CODEC_ID_H264, 4096, 2304, 8},
		{AV_CODEC_ID_HEVC, 4096, 2304, 8}, {AV_CODEC_ID_NONE, 0, 0, 0}}, 0,
		false, 1, true},
};

// Not among Haiku's colour spaces: 4:2:0 as a plane of luma then one of Cb
// and Cr in pairs, both bytes_per_row apart, in sixteen bits (the NVDEC and
// RK3588 add-ons) or eight (the RK3588 one, which then needs only copy).
const color_space kColorSpaceP010 = (color_space)0x50303130;	// 'P010'
const color_space kColorSpaceNV12 = (color_space)0x4e563132;	// 'NV12'
// Three planes, luma then Cb then Cr, the chroma rows half of bytes_per_row
// apart (the Raspberry Pi add-on): what libavcodec gives too, and what the
// scaler is quickest with.
const color_space kColorSpaceI420 = (color_space)0x49343230;	// 'I420'


struct LoadedAddOn {
	const HardwareDecoderKind*	kind;
	BString						path;
	image_id					image;
	DecoderPlugin*				plugin;
	int							maxReferenceFrames;
};


/*!	The NVDEC add-on says by its name whether it holds 16 reference frames. */
int
max_reference_frames(const HardwareDecoderKind& kind, DecoderPlugin* plugin)
{
	if (kind.maxReferenceFrames == 0 || strcmp(kind.leafName, "nvdec") != 0)
		return kind.maxReferenceFrames;
	Decoder* decoder = plugin->NewDecoder(0);
	if (decoder == NULL)
		return kind.maxReferenceFrames;
	media_codec_info info;
	memset(&info, 0, sizeof(info));
	decoder->GetCodecInfo(&info);
	delete decoder;
	return strcmp(info.short_name, "nvdec h264") == 0
		? kind.maxReferenceFrames : 0;
}


std::mutex sAddOnLock;
bool sAddOnsScanned = false;
std::vector<LoadedAddOn> sAddOns;


void
scan_add_ons()
{
	// Called with sAddOnLock held.
	if (sAddOnsScanned)
		return;
	sAddOnsScanned = true;

	if (getenv("AIRTIME_NO_HARDWARE") != NULL)
		return;

	char** paths = NULL;
	size_t count = 0;
	if (find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY, "media/plugins", &paths,
			&count) != B_OK) {
		return;
	}

	for (const HardwareDecoderKind& kind : kKinds) {
		for (size_t i = 0; i < count; i++) {
			BPath path(paths[i], kind.leafName);
			BEntry entry(path.Path());
			if (!entry.Exists())
				continue;

			image_id image = load_add_on(path.Path());
			if (image < 0) {
				fprintf(stderr, "airTime: could not load %s: %s\n",
					path.Path(), strerror(image));
				continue;
			}
			MediaPlugin* (*instantiate)() = NULL;
			if (get_image_symbol(image, "instantiate_plugin",
					B_SYMBOL_TYPE_TEXT, (void**)&instantiate) != B_OK
				|| instantiate == NULL) {
				unload_add_on(image);
				continue;
			}
			MediaPlugin* plugin = instantiate();
			DecoderPlugin* decoderPlugin
				= dynamic_cast<DecoderPlugin*>(plugin);
			if (decoderPlugin == NULL) {
				delete plugin;
				unload_add_on(image);
				continue;
			}
			sAddOns.push_back(LoadedAddOn{&kind, path.Path(), image,
				decoderPlugin, max_reference_frames(kind, decoderPlugin)});
			// The first directory wins, as in the Media Kit.
			break;
		}
	}
	free(paths);
}


const CodecLimits*
limits_for(const HardwareDecoderKind& kind, AVCodecID codec)
{
	for (const CodecLimits& candidate : kind.codecs) {
		if (candidate.codec == AV_CODEC_ID_NONE)
			break;
		if (candidate.codec == codec)
			return &candidate;
	}
	return NULL;
}


bool
kind_handles_codec(const HardwareDecoderKind& kind, AVCodecID codec)
{
	return limits_for(kind, codec) != NULL;
}


bool
is_ten_bit(const AVCodecParameters* parameters)
{
	return parameters->format == AV_PIX_FMT_YUV420P10LE
		|| (parameters->codec_id == AV_CODEC_ID_HEVC
			&& parameters->profile == FF_PROFILE_HEVC_MAIN_10);
}


/*!	Whether a picture half as wide and high as the stream's shows all the
	screen can: the screens together are no larger, in their own pixels (a
	desktop drawn at a higher density has more of them than its frame
	says). AIRTIME_HW_HALF=0 never asks for half pictures, =1 always. */
static bool
half_size_is_enough(const AVCodecParameters* parameters)
{
	if (const char* value = getenv("AIRTIME_HW_HALF"))
		return strcmp(value, "0") != 0;
	BScreen screen(B_MAIN_SCREEN_ID);
	if (!screen.IsValid())
		return false;
	BRect frame = screen.Frame();
	int64 width = frame.IntegerWidth() + 1;
	int64 height = frame.IntegerHeight() + 1;
	display_mode mode;
	if (screen.GetMode(&mode) == B_OK) {
		width = std::max(width, (int64)mode.virtual_width);
		height = std::max(height, (int64)mode.virtual_height);
	}
	return width * 2 <= parameters->width && height * 2 <= parameters->height;
}


/*!	The stream properties every add-on here needs: 4:2:0 of a depth the
	engine decodes, no fields, no larger than the engine. */
bool
stream_is_eligible(const LoadedAddOn& addOn, AVStream* stream,
	BString* reason)
{
	const HardwareDecoderKind& kind = *addOn.kind;
	AVCodecParameters* parameters = stream->codecpar;
	const CodecLimits* limits = limits_for(kind, parameters->codec_id);
	if (limits == NULL) {
		reason->SetToFormat("%s does not decode %s", kind.displayName,
			avcodec_get_name(parameters->codec_id));
		return false;
	}
	AVPixelFormat format = (AVPixelFormat)parameters->format;
	bool tenBit = format == AV_PIX_FMT_YUV420P10LE;
	if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P
		&& format != AV_PIX_FMT_NONE && !(tenBit && limits->maxBitDepth >= 10)) {
		reason->SetToFormat("%s decodes %d bit 4:2:0 %s only, this is %s",
			kind.displayName, limits->maxBitDepth,
			avcodec_get_name(parameters->codec_id), av_get_pix_fmt_name(format));
		return false;
	}
	if (parameters->field_order != AV_FIELD_UNKNOWN
		&& parameters->field_order != AV_FIELD_PROGRESSIVE) {
		reason->SetToFormat("%s does not decode interlaced pictures",
			kind.displayName);
		return false;
	}
	if (parameters->width <= 0 || parameters->height <= 0
		|| parameters->width > limits->maxWidth
		|| parameters->height > limits->maxHeight) {
		reason->SetToFormat("%dx%d is outside what %s decodes",
			parameters->width, parameters->height, kind.displayName);
		return false;
	}

	if (parameters->codec_id == AV_CODEC_ID_H264) {
		H264SequenceInfo info;
		int lengthSize = nal_length_size(false, parameters->extradata,
			parameters->extradata_size);
		if (find_h264_sps(parameters->extradata, parameters->extradata_size,
				lengthSize, &info)) {
			if (strcmp(kind.leafName, "amduvd") == 0
				&& info.profile != 66 && info.profile != 77 && info.profile != 100) {
				reason->SetToFormat("%s decodes H.264 Baseline, Main and High only",
					kind.displayName);
				return false;
			}
			if (info.chromaFormat != 1 || info.bitDepthLuma != 8
				|| info.bitDepthChroma != 8) {
				reason->SetToFormat("%s decodes 8 bit 4:2:0 H.264 only",
					kind.displayName);
				return false;
			}
			if (!info.frameMbsOnly) {
				reason->SetToFormat("the stream may contain fields, which %s "
					"does not decode", kind.displayName);
				return false;
			}
			int maxReferences = addOn.maxReferenceFrames;
			const char* override = getenv("AIRTIME_NVDEC_MAX_REFERENCES");
			if (override != NULL && strcmp(kind.leafName, "nvdec") == 0)
				maxReferences = atoi(override);
			if (maxReferences > 0
				&& info.maxReferenceFrames > maxReferences) {
				reason->SetToFormat("%d reference frames, %s holds %d",
					info.maxReferenceFrames, kind.displayName, maxReferences);
				return false;
			}
		}
		// Without parameter sets in the extra data (a transport stream),
		// there is nothing to check yet; the watchdog catches a refusal.
	}
	if (parameters->codec_id == AV_CODEC_ID_HEVC
		&& parameters->profile != FF_PROFILE_UNKNOWN
		&& parameters->profile != FF_PROFILE_HEVC_MAIN
		&& !(parameters->profile == FF_PROFILE_HEVC_MAIN_10
			&& limits->maxBitDepth >= 10)) {
		reason->SetToFormat(limits->maxBitDepth >= 10
			? "%s decodes HEVC Main and Main 10 only"
			: "%s decodes HEVC Main (8 bit) only", kind.displayName);
		return false;
	}
	return true;
}


class MediaKitVideoDecoder;


/*!	Libmedia's Decoder owns its chunk provider and deletes it with itself, so
	the add-on gets this go-between rather than the decoder object. */
class ChunkForwarder : public BPrivate::media::ChunkProvider {
public:
	ChunkForwarder(MediaKitVideoDecoder* decoder)
		:
		fDecoder(decoder)
	{
	}

	status_t GetNextChunk(const void** chunk, size_t* size,
		media_header* header);

private:
	MediaKitVideoDecoder*	fDecoder;
};


class MediaKitVideoDecoder : public VideoDecoder {
public:
	MediaKitVideoDecoder(const LoadedAddOn& addOn, AVStream* stream,
		PacketQueue* queue, bigtime_t startTime)
		:
		VideoDecoder(stream, queue, startTime),
		fAddOn(addOn),
		fDecoder(NULL),
		fChunk(av_packet_alloc()),
		fPending(av_packet_alloc()),
		fHasPending(false),
		fPendingSerial(-1),
		fFlushPending(false),
		fEndOfStream(false),
		fDrained(false),
		fCanceled(false),
		fFailed(false),
		fWidth(0),
		fHeight(0),
		fRowBytes(0),
		fFrameDuration(_FrameDuration()),
		fBufferPool(NULL),
		fBufferSize(0)
	{
	}

	~MediaKitVideoDecoder()
	{
		delete fDecoder;
		av_buffer_pool_uninit(&fBufferPool);
		av_packet_free(&fChunk);
		av_packet_free(&fPending);
	}

	status_t Init(BString* reason)
	{
		AVCodecParameters* parameters = fStream->codecpar;

		media_format_description description;
		description.family = B_MISC_FORMAT_FAMILY;
		description.u.misc.file_format = 'ffmp';
		description.u.misc.codec = parameters->codec_id;

		media_format format;
		BMediaFormats formats;
		status_t status = formats.InitCheck();
		if (status == B_OK)
			status = formats.GetFormatFor(description, &format);
		if (status != B_OK) {
			// Nobody registered it yet (normally the ffmpeg add-on does).
			format.Clear();
			format.type = B_MEDIA_ENCODED_VIDEO;
			status = formats.MakeFormatFor(&description, 1, &format);
		}
		if (status != B_OK) {
			reason->SetToFormat("the Media Kit has no format for %s: %s",
				avcodec_get_name(parameters->codec_id), strerror(status));
			return status;
		}

		format.type = B_MEDIA_ENCODED_VIDEO;
		format.user_data_type = B_CODEC_TYPE_INFO;
		*(uint32*)format.user_data = parameters->codec_tag;
		format.user_data[4] = 0;
		format.require_flags = 0;
		format.deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;

		media_raw_video_format& output = format.u.encoded_video.output;
		AVRational rate = av_guess_frame_rate(NULL, fStream, NULL);
		output.field_rate = rate.num > 0 && rate.den > 0
			? (float)av_q2d(rate) : 0.0f;
		output.interlace = 1;
		output.first_active = 0;
		output.last_active = parameters->height - 1;
		output.orientation = B_VIDEO_TOP_LEFT_RIGHT;
		AVRational aspect = parameters->sample_aspect_ratio;
		if (aspect.num <= 0 || aspect.den <= 0)
			aspect = AVRational{1, 1};
		AVRational display;
		av_reduce(&display.num, &display.den,
			(int64)parameters->width * aspect.num,
			(int64)parameters->height * aspect.den, 65535);
		output.pixel_width_aspect = display.num;
		output.pixel_height_aspect = display.den;
		output.display.format = B_YCbCr420;
		output.display.line_width = parameters->width;
		output.display.line_count = parameters->height;
		output.display.bytes_per_row = 0;
		output.display.pixel_offset = 0;
		output.display.line_offset = 0;
		output.display.flags = 0;

		fDecoder = fAddOn.plugin->NewDecoder(fAddOn.kind->decoderIndex);
		if (fDecoder == NULL) {
			reason->SetTo("the add-on made no decoder");
			return B_NO_MEMORY;
		}
		fDecoder->SetChunkProvider(new ChunkForwarder(this));

		media_format encoded = format;
		status = fDecoder->Setup(&encoded, parameters->extradata,
			parameters->extradata_size);
		if (status != B_OK) {
			reason->SetToFormat("%s refused the stream: %s",
				fAddOn.kind->displayName, strerror(status));
			return status;
		}
		fEncodedFormat = encoded;
		fWidth = parameters->width;
		fHeight = parameters->height;
		return B_OK;
	}

	BString Name() const
	{
		BString name;
		name.SetToFormat("%s (%s)", fAddOn.kind->displayName,
			fCodecName.Length() > 0 ? fCodecName.String() : "hardware");
		return name;
	}

	bool IsHardware() const
	{
		return true;
	}

	status_t Decode(VideoFramePtr& output)
	{
		for (;;) {
			if (fCanceled)
				return B_CANCELED;
			if (fFailed)
				return B_ERROR;
			if (fFlushPending) {
				fDecoder->SeekedTo(0, 0);
				fFlushPending = false;
				fEndOfStream = false;
				fDrained = false;
				fSerial = fPendingSerial;
				fPacketsWithoutFrame = 0;
				// A seek between two Decode() calls: let the caller see the
				// new serial before decoding carries on.
				return B_INTERRUPTED;
			}
			// Past the end of the stream (scanning gives the decoder a key
			// frame and then the end) an add-on need not ask for another
			// chunk, which is where a seek is noticed otherwise; the RK3588
			// one does not, and decoding stopped for good.
			if (fEndOfStream && fQueue->HasNewerSerial(fSerial)) {
				fFlushPending = true;
				fPendingSerial = fQueue->Serial();
				continue;
			}
			if (fDrained && !fQueue->HasNewerSerial(fSerial))
				return B_LAST_BUFFER_ERROR;

			if (fRowBytes == 0) {
				status_t status = _NegotiateOutput();
				if (fFlushPending || fCanceled)
					continue;
				if (status != B_OK)
					return status;
			}

			VideoFramePtr frame = std::make_shared<VideoFrame>();
			AVFrame* picture = frame->frame;
			picture->format = fPixelFormat;
			picture->width = fWidth;
			picture->height = fHeight;
			// Rows exactly as wide as the add-on writes them, and for P010
			// and NV12 the chroma plane straight after the luma one,
			// unpadded.
			bool semiPlanar = fPixelFormat == AV_PIX_FMT_P010LE
				|| fPixelFormat == AV_PIX_FMT_NV12;
			bool planar = fPixelFormat == AV_PIX_FMT_YUV420P;
			size_t lumaSize = fRowBytes * fHeight;
			size_t chromaRows = (fHeight + 1) / 2;
			size_t size = semiPlanar || planar
				? lumaSize + fRowBytes * chromaRows : lumaSize;
			if (fBufferPool == NULL || fBufferSize != size) {
				av_buffer_pool_uninit(&fBufferPool);
				fBufferPool = av_buffer_pool_init(size + 64, av_buffer_alloc);
				fBufferSize = size;
			}
			picture->buf[0] = fBufferPool != NULL
				? av_buffer_pool_get(fBufferPool) : NULL;
			if (picture->buf[0] == NULL)
				return B_NO_MEMORY;
			picture->data[0] = picture->buf[0]->data;
			picture->linesize[0] = (int)fRowBytes;
			if (semiPlanar) {
				picture->data[1] = picture->data[0] + lumaSize;
				picture->linesize[1] = (int)fRowBytes;
			} else if (planar) {
				picture->data[1] = picture->data[0] + lumaSize;
				picture->linesize[1] = (int)(fRowBytes / 2);
				picture->data[2] = picture->data[1]
					+ (fRowBytes / 2) * chromaRows;
				picture->linesize[2] = (int)(fRowBytes / 2);
			}
			if (picture->linesize[0] != (int)fRowBytes) {
				fprintf(stderr, "airTime: unexpected row length %d != %zu\n",
					picture->linesize[0], fRowBytes);
				return B_ERROR;
			}

			media_header header;
			memset(&header, 0, sizeof(header));
			media_decode_info info;
			bigtime_t skip = fSkipBefore;
			// The NVDEC add-on leaves pictures before an accurate seek's
			// target in the card's arrangement when told so.
			info.time_to_decode = skip > 0 ? -(skip + fStartTime) : 0;
			int64 count = 0;
			status_t status = fDecoder->Decode(picture->data[0], &count,
				&header, &info);
			if (fFlushPending || fCanceled || fFailed)
				continue;
			if (status != B_OK) {
				if (fEndOfStream) {
					// Everything the decoder held has come out.
					fDrained = true;
					return B_LAST_BUFFER_ERROR;
				}
				return status;
			}
			if (count < 1)
				continue;

			fPacketsWithoutFrame = 0;
			frame->pts = _MediaTime(header.start_time);
			frame->duration = fFrameDuration;
			frame->serial = fSerial;
			frame->hardware = true;
			frame->frame->sample_aspect_ratio
				= fStream->codecpar->sample_aspect_ratio;
			frame->frame->colorspace = fStream->codecpar->color_space;
			frame->frame->color_range = fStream->codecpar->color_range;
			frame->frame->color_trc = fStream->codecpar->color_trc;
			frame->frame->color_primaries
				= fStream->codecpar->color_primaries;
			frame->frame->chroma_location
				= fStream->codecpar->chroma_location;
			_AddLightLevels(frame->frame);
			output = frame;
			return B_OK;
		}
	}

	// For the ChunkForwarder
	status_t GetNextChunk(const void** chunk, size_t* size,
		media_header* header)
	{
		if (fFlushPending)
			return B_INTERRUPTED;
		if (fFailed || fCanceled)
			return B_ERROR;
		// An add-on that takes packet after packet without giving a picture
		// back has refused the stream without saying so (the NVDEC add-on
		// skips pictures it cannot decode); give up on it.
		if (fPacketsWithoutFrame > 150) {
			fFailed = true;
			return B_ERROR;
		}

		if (fEndOfStream) {
			if (fQueue->HasNewerSerial(fSerial)) {
				fFlushPending = true;
				fPendingSerial = fQueue->Serial();
				return B_INTERRUPTED;
			}
			return B_LAST_BUFFER_ERROR;
		}

		for (;;) {
			av_packet_unref(fChunk);
			int serial;
			if (fHasPending) {
				av_packet_move_ref(fChunk, fPending);
				serial = fPendingSerial;
				fHasPending = false;
			} else {
				int got = fQueue->Get(fChunk, &serial, 100000);
				if (got < 0) {
					fCanceled = true;
					return B_CANCELED;
				}
				if (got == 0) {
					// Flushed with nothing behind it yet: unwind so the
					// decoder is reset before the next packet.
					if (fQueue->HasNewerSerial(fSerial) && fSerial >= 0) {
						fFlushPending = true;
						fPendingSerial = fQueue->Serial();
						return B_INTERRUPTED;
					}
					continue;
				}
			}

			if (fSerial < 0)
				fSerial = serial;
			if (serial != fSerial) {
				av_packet_move_ref(fPending, fChunk);
				fHasPending = true;
				fPendingSerial = serial;
				fFlushPending = true;
				return B_INTERRUPTED;
			}

			if (fChunk->data == NULL) {
				fEndOfStream = true;
				return B_LAST_BUFFER_ERROR;
			}
			if (fKeyframesOnly && (fChunk->flags & AV_PKT_FLAG_KEY) == 0)
				continue;

			int64 timestamp = fChunk->pts != AV_NOPTS_VALUE
				? fChunk->pts : fChunk->dts;
			bigtime_t time = to_micros(timestamp, fStream->time_base);
			// Pictures before the time a seek goes to need not come out
			// (the Raspberry Pi's firmware decoder gives none for them).
			bigtime_t skip = fSkipBefore;
			if (skip <= 0 || time == kNoTime || time >= skip + fStartTime)
				fPacketsWithoutFrame++;
			memset(header, 0, sizeof(*header));
			header->type = B_MEDIA_ENCODED_VIDEO;
			header->start_time = time == kNoTime ? 0 : time;
			header->size_used = fChunk->size;
			header->file_pos = fChunk->pos;
			header->orig_size = fChunk->size;
			header->u.encoded_video.field_flags
				= (fChunk->flags & AV_PKT_FLAG_KEY) != 0
					? B_MEDIA_KEY_FRAME : 0;
			*chunk = fChunk->data;
			*size = fChunk->size;
			return B_OK;
		}
	}

private:
	status_t _NegotiateOutput()
	{
		// Luma and chroma are cheaper for an add-on to hand over than
		// pixels, and the conversion to pixels happens anyway while scaling
		// to the window; ask for them first, as the decoder made them if it
		// can (NV12), else packed.
		// Ten-bit pictures keep their depth, for the HDR tone mapping.
		media_format format;
		status_t status = B_ERROR;
		static const color_space kSpaces[] = {kColorSpaceP010,
			kColorSpaceI420, kColorSpaceNV12, B_YCbCr422, B_RGB32};
		const char* forced = getenv("AIRTIME_HW_RGB");
		bool tenBit = is_ten_bit(fStream->codecpar);
		bool half = fAddOn.kind->halfSize
			&& half_size_is_enough(fStream->codecpar);
		for (color_space space : kSpaces) {
			if ((space == B_YCbCr422 || space == kColorSpaceNV12
					|| space == kColorSpaceI420)
				&& forced != NULL) {
				continue;
			}
			if (space == kColorSpaceP010 && (!tenBit || forced != NULL))
				continue;
			format.Clear();
			format.type = B_MEDIA_RAW_VIDEO;
			format.u.raw_video = media_raw_video_format::wildcard;
			format.u.raw_video.display.format = space;
			if (half && (space == kColorSpaceP010
					|| space == kColorSpaceI420)) {
				format.u.raw_video.display.line_width
					= fStream->codecpar->width / 2;
				format.u.raw_video.display.line_count
					= fStream->codecpar->height / 2;
			}
			status = fDecoder->NegotiateOutputFormat(&format);
			if (status == B_OK && format.type == B_MEDIA_RAW_VIDEO
				&& format.u.raw_video.display.format == space) {
				break;
			}
			if (fFlushPending || fCanceled)
				return B_INTERRUPTED;
		}
		if (status != B_OK)
			return status;
		if (format.type != B_MEDIA_RAW_VIDEO)
			return B_MEDIA_BAD_FORMAT;

		const media_video_display_info& display = format.u.raw_video.display;
		switch (display.format) {
			case B_RGB32:
			case B_RGBA32:
				fPixelFormat = AV_PIX_FMT_BGRA;
				break;
			case B_YCbCr422:
				fPixelFormat = fAddOn.kind->chromaFirst
					? AV_PIX_FMT_UYVY422 : AV_PIX_FMT_YUYV422;
				break;
			default:
				if (display.format == kColorSpaceP010) {
					fPixelFormat = AV_PIX_FMT_P010LE;
					break;
				}
				if (display.format == kColorSpaceNV12) {
					fPixelFormat = AV_PIX_FMT_NV12;
					break;
				}
				if (display.format == kColorSpaceI420) {
					fPixelFormat = AV_PIX_FMT_YUV420P;
					break;
				}
				fprintf(stderr, "airTime: %s offers colour space %#x\n",
					fAddOn.kind->displayName, (unsigned)display.format);
				return B_MEDIA_BAD_FORMAT;
		}
		if (display.line_width > 0)
			fWidth = display.line_width;
		if (display.line_count > 0)
			fHeight = display.line_count;
		fRowBytes = display.bytes_per_row > 0 ? display.bytes_per_row
			: (size_t)fWidth * (fPixelFormat == AV_PIX_FMT_BGRA ? 4
				: fPixelFormat == AV_PIX_FMT_NV12
					|| fPixelFormat == AV_PIX_FMT_YUV420P ? 1 : 2);

		media_codec_info codecInfo;
		memset(&codecInfo, 0, sizeof(codecInfo));
		fDecoder->GetCodecInfo(&codecInfo);
		fCodecName = codecInfo.short_name;
		return B_OK;
	}

	/*!	The film's light levels, which the tone mapping reads from each
		frame and libavcodec would have attached. */
	void _AddLightLevels(AVFrame* frame)
	{
		const AVCodecParameters* parameters = fStream->codecpar;
		const AVPacketSideData* light = av_packet_side_data_get(
			parameters->coded_side_data, parameters->nb_coded_side_data,
			AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
		if (light != NULL && light->size >= sizeof(AVContentLightMetadata)) {
			AVFrameSideData* data = av_frame_new_side_data(frame,
				AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, sizeof(AVContentLightMetadata));
			if (data != NULL)
				memcpy(data->data, light->data, sizeof(AVContentLightMetadata));
		}
		const AVPacketSideData* mastering = av_packet_side_data_get(
			parameters->coded_side_data, parameters->nb_coded_side_data,
			AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
		if (mastering != NULL
			&& mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
			AVFrameSideData* data = av_frame_new_side_data(frame,
				AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
				sizeof(AVMasteringDisplayMetadata));
			if (data != NULL) {
				memcpy(data->data, mastering->data,
					sizeof(AVMasteringDisplayMetadata));
			}
		}
	}

	const LoadedAddOn&	fAddOn;
	Decoder*			fDecoder;
	media_format		fEncodedFormat;
	AVPacket*			fChunk;
	AVPacket*			fPending;
	bool				fHasPending;
	int					fPendingSerial;
	bool				fFlushPending;
	bool				fEndOfStream;
	bool				fDrained;
	bool				fCanceled;
	bool				fFailed;
	int					fWidth;
	int					fHeight;
	size_t				fRowBytes;
	AVPixelFormat		fPixelFormat;
	bigtime_t			fFrameDuration;
	BString				fCodecName;
	// Frames are big (a 4K ten-bit one is 24 MB) and new memory is slow to
	// fill the first time, so their buffers are used again.
	AVBufferPool*		fBufferPool;
	size_t				fBufferSize;
};

status_t
ChunkForwarder::GetNextChunk(const void** chunk, size_t* size,
	media_header* header)
{
	return fDecoder->GetNextChunk(chunk, size, header);
}

}	// namespace


VideoDecoder*
create_hardware_decoder(AVStream* stream, PacketQueue* queue,
	bigtime_t startTime, BString* reason)
{
	std::lock_guard<std::mutex> lock(sAddOnLock);
	scan_add_ons();

	*reason = "no hardware video decoder is installed";
	for (const LoadedAddOn& addOn : sAddOns) {
		if (!kind_handles_codec(*addOn.kind, stream->codecpar->codec_id))
			continue;
		if (!stream_is_eligible(addOn, stream, reason))
			continue;
		MediaKitVideoDecoder* decoder = new(std::nothrow) MediaKitVideoDecoder(
			addOn, stream, queue, startTime);
		if (decoder == NULL)
			return NULL;
		if (decoder->Init(reason) != B_OK) {
			delete decoder;
			continue;
		}
		return decoder;
	}
	if (reason->Length() == 0 || *reason == "no hardware video decoder is installed") {
		bool any = false;
		for (const LoadedAddOn& addOn : sAddOns) {
			if (!kind_handles_codec(*addOn.kind, stream->codecpar->codec_id))
				continue;
			any = true;
		}
		if (!sAddOns.empty() && !any) {
			reason->SetToFormat("no hardware decoder for %s",
				avcodec_get_name(stream->codecpar->codec_id));
		}
	}
	return NULL;
}


BString
hardware_decoder_summary()
{
	std::lock_guard<std::mutex> lock(sAddOnLock);
	scan_add_ons();

	BString summary;
	for (const LoadedAddOn& addOn : sAddOns) {
		if (summary.Length() > 0)
			summary << ", ";
		summary << addOn.kind->displayName << " (";
		bool first = true;
		for (const CodecLimits& limits : addOn.kind->codecs) {
			if (limits.codec == AV_CODEC_ID_NONE)
				break;
			if (!first)
				summary << ", ";
			summary << codec_display_name(limits.codec);
			if (limits.maxBitDepth > 8)
				summary << " up to " << limits.maxBitDepth << " bit";
			first = false;
		}
		summary << ")";
	}
	if (summary.Length() == 0)
		summary = "none";
	return summary;
}

}	// namespace airtime
