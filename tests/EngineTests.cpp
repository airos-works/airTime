/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Tests of the engine parts that do not need Haiku. Built on Linux against
 * the host's FFmpeg with stand-ins for a few Haiku headers (tests/host):
 *
 *	make check-host
 *
 * Subtitle file tests use files from AIRTIME_TEST_MEDIA if it is set.
 */


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include "Bitstream.h"
#include "Languages.h"
#include "SkipSegments.h"
#include "Subtitles.h"
#include "Tracks.h"
#include "YuvScaler.h"


using namespace airtime;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(condition) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
				#condition); \
		} \
	} while (0)

#define CHECK_EQUAL_STRING(actual, expected) \
	do { \
		sChecks++; \
		std::string a = (actual), e = (expected); \
		if (a != e) { \
			sFailures++; \
			fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, \
				__LINE__, e.c_str(), a.c_str()); \
		} \
	} while (0)


static void
test_time()
{
	CHECK_EQUAL_STRING(format_time(0).String(), "0:00");
	CHECK_EQUAL_STRING(format_time(83000000).String(), "1:23");
	CHECK_EQUAL_STRING(format_time(3723000000LL).String(), "1:02:03");
	CHECK_EQUAL_STRING(format_time(3723456789LL, true).String(), "1:02:03.45");
	CHECK_EQUAL_STRING(format_time(5000000, false, true).String(), "0:00:05");
	CHECK_EQUAL_STRING(format_time(-65000000).String(), "-1:05");

	bigtime_t time = 0;
	CHECK(parse_time("1:02:03", &time) && time == 3723000000LL);
	CHECK(parse_time("62:03.5", &time) && time == 3723500000LL);
	CHECK(parse_time("90", &time) && time == 90000000);
	CHECK(parse_time(" 1h2m3s ", &time) && time == 3723000000LL);
	CHECK(parse_time("2m", &time) && time == 120000000);
	CHECK(parse_time("0:00:10.25", &time) && time == 10250000);
	CHECK(!parse_time("", &time));
	CHECK(!parse_time("abc", &time));
	CHECK(!parse_time("1:75", &time));
	CHECK(!parse_time("1:2:3:4", &time));
	CHECK(!parse_time("-5", &time));
}


static void
test_languages()
{
	CHECK_EQUAL_STRING(language_name("eng").String(), "English");
	CHECK_EQUAL_STRING(language_name("fre").String(), "French");
	CHECK_EQUAL_STRING(language_name("fra").String(), "French");
	CHECK_EQUAL_STRING(language_name("de").String(), "German");
	CHECK_EQUAL_STRING(language_name("pt-BR").String(), "Portuguese");
	CHECK_EQUAL_STRING(language_name("und").String(), "");
	CHECK_EQUAL_STRING(language_name("xyz").String(), "XYZ");
	CHECK(same_language("fre", "fr"));
	CHECK(same_language("ger", "deu"));
	CHECK(!same_language("eng", "fre"));
	CHECK(!same_language("", "eng"));
	CHECK_EQUAL_STRING(canonical_language("chi").String(), "zh");

	TrackInfo audio;
	audio.kind = TRACK_AUDIO;
	audio.language = "fre";
	audio.title = "Version française";
	audio.codec = "Dolby Digital";
	audio.channels = 6;
	CHECK_EQUAL_STRING(audio.Label(2).String(),
		"French — Version française (Dolby Digital 5.1)");
	TrackInfo subtitle;
	subtitle.kind = TRACK_SUBTITLE;
	subtitle.language = "eng";
	subtitle.codec = "SubRip";
	subtitle.isForced = true;
	CHECK_EQUAL_STRING(subtitle.Label(1).String(),
		"English (SubRip) [Forced]");
	TrackInfo untitled;
	untitled.kind = TRACK_AUDIO;
	CHECK_EQUAL_STRING(untitled.Label(3).String(), "Track 3");
}


static void
test_ass()
{
	SubtitleEvent event;
	parse_ass_dialogue("0,0,Default,,0,0,0,,Hello {\\i1}world{\\i0}\\Nsecond",
		event);
	CHECK(event.lines.size() == 2);
	CHECK(event.alignment == 2);
	if (event.lines.size() == 2) {
		CHECK(event.lines[0].runs.size() == 2);
		CHECK_EQUAL_STRING(event.lines[0].PlainText().String(), "Hello world");
		if (event.lines[0].runs.size() == 2) {
			CHECK(!event.lines[0].runs[0].italic);
			CHECK(event.lines[0].runs[1].italic);
		}
		CHECK_EQUAL_STRING(event.lines[1].PlainText().String(), "second");
	}

	SubtitleEvent top;
	parse_ass_dialogue("1,0,Default,,0,0,0,,{\\an8}{\\c&H00FFFF&}Top", top);
	CHECK(top.alignment == 8);
	CHECK(top.lines.size() == 1);
	if (!top.lines.empty() && !top.lines[0].runs.empty()) {
		const SubtitleRun& run = top.lines[0].runs[0];
		CHECK(run.hasColor);
		CHECK(run.color.red == 255 && run.color.green == 255
			&& run.color.blue == 0);
	}

	SubtitleEvent legacy;
	parse_ass_dialogue("0,0,Default,,0,0,0,,{\\a6}Old top", legacy);
	CHECK(legacy.alignment == 8);

	SubtitleEvent drawing;
	parse_ass_dialogue(
		"0,0,Default,,0,0,0,,{\\p1}m 0 0 l 100 0 100 100{\\p0}Text", drawing);
	CHECK_EQUAL_STRING(subtitle_plain_text(drawing).String(), "Text");

	SubtitleEvent bold;
	parse_ass_dialogue("0,0,Default,,0,0,0,,{\\b1}Bold{\\b0} plain\\hspace",
		bold);
	CHECK(bold.lines.size() == 1);
	if (bold.lines.size() == 1) {
		CHECK(bold.lines[0].runs[0].bold);
		CHECK_EQUAL_STRING(bold.lines[0].PlainText().String(),
			"Bold plain space");
	}

	SubtitleEvent whole;
	parse_ass_dialogue(
		"Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Full line", whole);
	CHECK_EQUAL_STRING(subtitle_plain_text(whole).String(), "Full line");
}


static SubtitleEventPtr
make_event(bigtime_t start, bigtime_t end, const char* text)
{
	auto event = std::make_shared<SubtitleEvent>();
	event->start = start;
	event->end = end;
	if (text != NULL) {
		SubtitleLine line;
		SubtitleRun run;
		run.text = text;
		line.runs.push_back(run);
		event->lines.push_back(line);
	}
	return event;
}


static void
test_track()
{
	SubtitleTrack track;
	track.SetMaxOpenDuration(5000000);
	track.Add(make_event(1000000, 3000000, "one"));
	track.Add(make_event(1000000, 3000000, "one"));	// a duplicate
	track.Add(make_event(2000000, 4000000, "two"));
	track.Add(make_event(10000000, kNoTime, "open"));
	track.Add(make_event(12000000, kNoTime, NULL));	// clears the screen
	track.Add(make_event(20000000, kNoTime, "last"));
	CHECK(track.Count() == 5);

	CHECK(track.Active(500000).empty());
	CHECK(track.Active(1500000).size() == 1);
	std::vector<SubtitleEventPtr> both = track.Active(2500000);
	CHECK(both.size() == 2);
	if (both.size() == 2) {
		CHECK_EQUAL_STRING(subtitle_plain_text(*both[0]).String(), "one");
		CHECK_EQUAL_STRING(subtitle_plain_text(*both[1]).String(), "two");
	}
	CHECK(track.Active(11000000).size() == 1);
	CHECK(track.Active(13000000).empty());
	CHECK(track.Active(24000000).size() == 1);
	CHECK(track.Active(26000000).empty());	// open events time out

	track.Prune(9000000);
	CHECK(track.Count() == 3);
}


static void
test_files(const char* media)
{
	if (media == NULL) {
		printf("  (no AIRTIME_TEST_MEDIA: subtitle files skipped)\n");
		return;
	}
	std::string base(media);

	SubtitleTrack english;
	BString codec, language, reason;
	CHECK(load_subtitle_file((base + "/en.srt").c_str(), &english, &codec,
		&language, &reason) == B_OK);
	CHECK_EQUAL_STRING(codec.String(), "SubRip");
	CHECK(english.Count() == 22);
	std::vector<SubtitleEventPtr> at10 = english.Active(10500000);
	CHECK(at10.size() == 1);
	if (at10.size() == 1) {
		CHECK_EQUAL_STRING(subtitle_plain_text(*at10[0]).String(),
			"English line 3 at 10 s\nsecond row");
	}
	std::vector<SubtitleEventPtr> italic = english.Active(6500000);
	CHECK(italic.size() == 1 && !italic[0]->lines.empty()
		&& italic[0]->lines[0].runs[0].italic);

	SubtitleTrack german;
	CHECK(load_subtitle_file((base + "/sidecar.de.srt").c_str(), &german,
		&codec, &language, &reason) == B_OK);
	CHECK_EQUAL_STRING(language.String(), "de");
	std::vector<SubtitleEventPtr> line = german.Active(1500000);
	CHECK(line.size() == 1);
	if (line.size() == 1) {
		CHECK_EQUAL_STRING(subtitle_plain_text(*line[0]).String(),
			"Deutsche Zeile 1 – Größe übergroß");
	}

	SubtitleTrack french;
	CHECK(load_subtitle_file((base + "/fr.ass").c_str(), &french, &codec,
		&language, &reason) == B_OK);
	std::vector<SubtitleEventPtr> top = french.Active(9000000);
	CHECK(top.size() == 1 && top[0]->alignment == 8);

	SubtitleTrack missing;
	CHECK(load_subtitle_file((base + "/does-not-exist.srt").c_str(), &missing,
		&codec, &language, &reason) != B_OK);
}


// #pragma mark - captions


static uint8
odd_parity(uint8 value)
{
	value &= 0x7f;
	int bits = 0;
	for (int i = 0; i < 7; i++)
		bits += (value >> i) & 1;
	return bits % 2 == 0 ? value | 0x80 : value;
}


// CEA-608 byte pairs for a pop-on caption on row 15.
static std::vector<std::pair<uint8, uint8> >
pop_on_caption(const char* text)
{
	std::vector<std::pair<uint8, uint8> > pairs;
	auto control = [&](uint8 a, uint8 b) {
		pairs.push_back(std::make_pair(odd_parity(a), odd_parity(b)));
		pairs.push_back(std::make_pair(odd_parity(a), odd_parity(b)));
	};
	control(0x14, 0x20);	// resume caption loading
	control(0x14, 0x2e);	// erase non-displayed memory
	control(0x14, 0x70);	// row 15, column 0
	size_t length = strlen(text);
	for (size_t i = 0; i < length; i += 2) {
		uint8 second = i + 1 < length ? text[i + 1] : 0;
		pairs.push_back(std::make_pair(odd_parity(text[i]),
			second != 0 ? odd_parity(second) : 0x80));
	}
	control(0x14, 0x2f);	// end of caption: show it
	return pairs;
}


static std::vector<uint8>
sei_nal(const std::vector<uint8>& triplets)
{
	std::vector<uint8> payload = {0xb5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03};
	payload.push_back(0x40 | (uint8)(triplets.size() / 3));
	payload.push_back(0xff);
	payload.insert(payload.end(), triplets.begin(), triplets.end());
	payload.push_back(0xff);

	std::vector<uint8> rbsp = {0x04, (uint8)payload.size()};
	rbsp.insert(rbsp.end(), payload.begin(), payload.end());
	rbsp.push_back(0x80);

	// Emulation prevention.
	std::vector<uint8> nal = {0x06};
	int zeros = 0;
	for (uint8 byte : rbsp) {
		if (zeros >= 2 && byte <= 3) {
			nal.push_back(0x03);
			zeros = 0;
		}
		nal.push_back(byte);
		zeros = byte == 0 ? zeros + 1 : 0;
	}
	return nal;
}


static void
test_captions()
{
	std::vector<uint8> triplets = {0xfc, 0x94, 0x20, 0xfc, 0x00, 0x00};
	std::vector<uint8> nal = sei_nal(triplets);

	// Length prefixed, as in MP4 and Matroska.
	std::vector<uint8> packet = {0, 0, 0, (uint8)nal.size()};
	packet.insert(packet.end(), nal.begin(), nal.end());
	std::vector<uint8> slice = {0x65, 0x88, 0x80, 0x00};
	packet.insert(packet.end(), {0, 0, 0, (uint8)slice.size()});
	packet.insert(packet.end(), slice.begin(), slice.end());

	std::vector<uint8> found;
	CHECK(extract_a53_captions(CAPTIONS_H264, packet.data(), packet.size(), 4,
		found));
	CHECK(found == triplets);

	// Annex B.
	std::vector<uint8> annexB = {0, 0, 0, 1};
	annexB.insert(annexB.end(), nal.begin(), nal.end());
	annexB.insert(annexB.end(), {0, 0, 1});
	annexB.insert(annexB.end(), slice.begin(), slice.end());
	found.clear();
	CHECK(extract_a53_captions(CAPTIONS_H264, annexB.data(), annexB.size(), 0,
		found));
	CHECK(found == triplets);

	// No captions in a plain slice.
	found.clear();
	CHECK(!extract_a53_captions(CAPTIONS_H264, slice.data(), slice.size(), 0,
		found));

	// The 608 decoder turns a pop-on caption into an event.
	SubtitleTrack track;
	CaptionDecoder decoder(&track);
	CHECK(decoder.Init() == B_OK);
	std::vector<std::pair<uint8, uint8> > pairs = pop_on_caption("HELLO CC");
	bigtime_t pts = 0;
	for (auto& pair : pairs) {
		uint8 data[3] = {0xfc, pair.first, pair.second};
		decoder.Add(pts, data, 3);
		pts += 33367;
	}
	for (int i = 0; i < 60; i++) {
		uint8 padding[3] = {0xfc, 0x80, 0x80};
		decoder.Add(pts, padding, 3);
		pts += 33367;
	}
	decoder.EndOfStream();
	std::vector<SubtitleEventPtr> shown = track.Active(pts - 1000000);
	CHECK(!shown.empty());
	if (!shown.empty()) {
		CHECK(shown[0]->caption);
		CHECK(shown[0]->positionY > 200);
		CHECK_EQUAL_STRING(subtitle_plain_text(*shown[0]).String(), "HELLO CC");
	}

	// Erasing the screen ends the caption.
	uint8 erase[2] = {odd_parity(0x14), odd_parity(0x2c)};
	for (int i = 0; i < 2; i++) {
		uint8 data[3] = {0xfc, erase[0], erase[1]};
		decoder.Add(pts, data, 3);
		pts += 33367;
	}
	for (int i = 0; i < 20; i++) {
		uint8 padding[3] = {0xfc, 0x80, 0x80};
		decoder.Add(pts, padding, 3);
		pts += 33367;
	}
	decoder.EndOfStream();
	CHECK(track.Active(pts - 100000).empty());
}


static void
test_sps()
{
	// x264, 1920x1080 (1088 coded), High profile, 4 reference frames.
	const uint8 sps[] = {0x67, 0x64, 0x00, 0x28, 0xac, 0xd9, 0x40, 0x78, 0x02,
		0x27, 0xe5, 0x84, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03,
		0x00, 0xf0, 0x3c, 0x60, 0xc6, 0x58};
	H264SequenceInfo info;
	CHECK(parse_h264_sps(sps, sizeof(sps), &info));
	CHECK(info.profile == 100);
	CHECK(info.chromaFormat == 1);
	CHECK(info.bitDepthLuma == 8);
	CHECK(info.frameMbsOnly);
	CHECK(info.width == 1920);
	CHECK(info.height == 1088);
	CHECK(info.maxReferenceFrames == 4);

	std::vector<uint8> avcC = {1, 0x64, 0x00, 0x28, 0xff, 0xe1, 0,
		(uint8)sizeof(sps)};
	avcC.insert(avcC.end(), sps, sps + sizeof(sps));
	avcC.insert(avcC.end(), {1, 0, 4, 0x68, 0xeb, 0xe3, 0xcb});
	CHECK(nal_length_size(false, avcC.data(), avcC.size()) == 4);
	H264SequenceInfo fromRecord;
	CHECK(find_h264_sps(avcC.data(), avcC.size(), 0, &fromRecord));
	CHECK(fromRecord.width == 1920);
}


/*!	The ten-bit scaler against swscale's bilinear filter on a smooth picture
	(they differ only in rounding and where chroma is sampled), P010 and
	yuv420p10 giving the same, and flat pictures staying flat. */
static void
test_scaler()
{
	const int width = 640;
	const int height = 360;
	AVFrame* p010 = av_frame_alloc();
	AVFrame* planar = av_frame_alloc();
	p010->format = AV_PIX_FMT_P010LE;
	planar->format = AV_PIX_FMT_YUV420P10LE;
	for (AVFrame* frame : {p010, planar}) {
		frame->width = width;
		frame->height = height;
		CHECK(av_frame_get_buffer(frame, 0) == 0);
	}
	auto lumaAt = [](int x, int y) {
		return (int)(512 + 300 * sin(x / 23.0) * cos(y / 17.0));
	};
	auto chromaAt = [](int x, int y, int plane) {
		return plane == 0 ? (int)(512 + 200 * sin(x / 40.0))
			: (int)(512 + 200 * cos(y / 30.0));
	};
	for (int y = 0; y < height; y++) {
		uint16* row = (uint16*)(p010->data[0] + y * p010->linesize[0]);
		uint16* planarRow = (uint16*)(planar->data[0]
			+ y * planar->linesize[0]);
		for (int x = 0; x < width; x++) {
			row[x] = lumaAt(x, y) << 6;
			planarRow[x] = lumaAt(x, y);
		}
	}
	for (int y = 0; y < height / 2; y++) {
		uint16* pairs = (uint16*)(p010->data[1] + y * p010->linesize[1]);
		uint16* cb = (uint16*)(planar->data[1] + y * planar->linesize[1]);
		uint16* cr = (uint16*)(planar->data[2] + y * planar->linesize[2]);
		for (int x = 0; x < width / 2; x++) {
			pairs[2 * x] = chromaAt(x, y, 0) << 6;
			pairs[2 * x + 1] = chromaAt(x, y, 1) << 6;
			cb[x] = chromaAt(x, y, 0);
			cr[x] = chromaAt(x, y, 1);
		}
	}

	const int targetWidth = 293;
	const int targetHeight = 165;
	SwsContext* sws = sws_getContext(width, height, AV_PIX_FMT_P010LE,
		targetWidth, targetHeight, AV_PIX_FMT_YUV444P16LE, SWS_BILINEAR,
		NULL, NULL, NULL);
	CHECK(sws != NULL);
	std::vector<uint16> reference[3];
	uint8* planes[4];
	int strides[4] = {targetWidth * 2, targetWidth * 2, targetWidth * 2, 0};
	for (int i = 0; i < 3; i++) {
		reference[i].resize(targetWidth * targetHeight);
		planes[i] = (uint8*)reference[i].data();
	}
	planes[3] = NULL;
	sws_scale(sws, p010->data, p010->linesize, 0, height, planes, strides);
	sws_freeContext(sws);

	YuvScaler scaler;
	YuvScaler planarScaler;
	YuvScaler::Scratch scratch;
	CHECK(YuvScaler::Handles(p010) && YuvScaler::Handles(planar));
	CHECK(scaler.Prepare(p010, targetWidth, targetHeight));
	CHECK(planarScaler.Prepare(planar, targetWidth, targetHeight));
	std::vector<uint16> row[3], planarRow[3];
	for (int i = 0; i < 3; i++) {
		row[i].resize(targetWidth);
		planarRow[i].resize(targetWidth);
	}
	int worst[3] = {};
	bool same = true;
	for (int y = 0; y < targetHeight; y++) {
		scaler.ScaleRow(p010, y, row[0].data(), row[1].data(), row[2].data(),
			scratch);
		planarScaler.ScaleRow(planar, y, planarRow[0].data(),
			planarRow[1].data(), planarRow[2].data(), scratch);
		for (int i = 0; i < 3; i++) {
			// Away from the edges, which swscale treats differently.
			for (int x = 2; x < targetWidth - 2 && y > 1
					&& y < targetHeight - 2; x++) {
				int difference = abs(row[i][x] - reference[i][y * targetWidth
					+ x]) >> 6;
				worst[i] = std::max(worst[i], difference);
			}
			for (int x = 0; x < targetWidth; x++)
				same = same && abs(row[i][x] - planarRow[i][x]) <= 1;
		}
	}
	printf("scaler: largest difference from swscale %d, %d, %d of 1023\n",
		worst[0], worst[1], worst[2]);
	CHECK(worst[0] <= 12 && worst[1] <= 12 && worst[2] <= 12);
	CHECK(same);

	// Flat stays flat, enlarged and shrunk, edges included.
	for (int y = 0; y < height; y++) {
		uint16* luma = (uint16*)(p010->data[0] + y * p010->linesize[0]);
		for (int x = 0; x < width; x++)
			luma[x] = 700 << 6;
	}
	for (int size : {97, 640, 1500}) {
		CHECK(scaler.Prepare(p010, size, size * 9 / 16));
		std::vector<uint16> luma(size), cb(size), cr(size);
		bool flat = true;
		for (int y = 0; y < size * 9 / 16; y++) {
			scaler.ScaleRow(p010, y, luma.data(), cb.data(), cr.data(),
				scratch);
			for (int x = 0; x < size; x++)
				flat = flat && luma[x] == 700 << 6;
		}
		CHECK(flat);
	}
	av_frame_free(&p010);
	av_frame_free(&planar);
}


static void
test_skip_segments()
{
	TagMap none;
	EpisodeInfo info = identify_episode(
		"/Videos/TV/Breaking.Bad.S01E02.720p.BluRay.x264.mkv", none);
	CHECK(info.IsEpisode());
	CHECK_EQUAL_STRING(info.show, "Breaking Bad");
	CHECK(info.season == 1 && info.episode == 2);

	info = identify_episode("/m/The Office (US) - 2x05 - Halloween.mp4", none);
	CHECK_EQUAL_STRING(info.show, "The Office (US)");
	CHECK(info.season == 2 && info.episode == 5);

	info = identify_episode("/m/Doctor Who (2005)/Season 01/"
		"Doctor Who (2005) - S01E03 - The Unquiet Dead.mkv", none);
	CHECK_EQUAL_STRING(info.show, "Doctor Who");
	CHECK(info.year == 2005);
	CHECK(info.season == 1 && info.episode == 3);

	info = identify_episode("/m/Severance/Season 2/04 - Woe's Hollow.mkv",
		none);
	CHECK_EQUAL_STRING(info.show, "Severance");
	CHECK(info.season == 2 && info.episode == 4);

	info = identify_episode("/m/Severance/S02E07.mkv", none);
	CHECK_EQUAL_STRING(info.show, "Severance");
	CHECK(info.season == 2 && info.episode == 7);

	info = identify_episode("/m/Show/Specials/Episode 3.mkv", none);
	CHECK_EQUAL_STRING(info.show, "Show");
	CHECK(info.season == 0 && info.episode == 3);

	info = identify_episode("/m/[Group] Show_Name_s03e10_[1080p].mkv", none);
	CHECK_EQUAL_STRING(info.show, "Show Name");
	CHECK(info.season == 3 && info.episode == 10);

	// Not episodes: films, a resolution, a bare number.
	CHECK(!identify_episode("/m/Blade Runner (1982) 1920x1080.mkv", none)
		.IsEpisode());
	CHECK(!identify_episode("/m/Movies/Heat.1995.mkv", none).IsEpisode());
	CHECK(!identify_episode("/m/Music/02 - Song.mp3", none).IsEpisode());

	TagMap tags;
	tags["show"] = "Slow Horses";
	tags["season_number"] = "4";
	tags["episode_sort"] = "6";
	info = identify_episode("/m/download.m4v", tags);
	CHECK_EQUAL_STRING(info.show, "Slow Horses");
	CHECK(info.season == 4 && info.episode == 6);

	JsonValue json;
	CHECK(parse_json("{\"a\": [1, -2.5e1, true, null, \"x\\u00e9\\n\"],"
		" \"b\": {}}", json));
	CHECK(json["a"].items.size() == 5);
	CHECK(json["a"].items[1].number == -25);
	CHECK_EQUAL_STRING(json["a"].items[4].string, "x\xc3\xa9\n");
	CHECK(json["missing"].IsNull());
	CHECK(!parse_json("{\"a\": }", json));
	CHECK(!parse_json("[1, 2", json));

	EpisodeInfo search;
	search.show = "The Office";
	search.year = 2005;
	CHECK(parse_tvmaze_search("[{\"score\":0.9,\"show\":{\"name\":\"The Office\","
		"\"premiered\":\"2001-07-09\",\"externals\":{\"thetvdb\":78107,"
		"\"imdb\":\"tt0290978\"}}},{\"score\":0.8,\"show\":{\"name\":"
		"\"The Office\",\"premiered\":\"2005-03-24\",\"externals\":"
		"{\"tvrage\":6061,\"thetvdb\":73244,\"imdb\":\"tt0386676\"}}}]",
		search));
	CHECK_EQUAL_STRING(search.imdbId, "tt0386676");
	CHECK(search.tvdbId == 73244);
	CHECK(!parse_tvmaze_search("[]", search));

	SkipSegments introDB = parse_introdb("{\"imdb_id\":\"tt0903747\","
		"\"intro\":{\"start_sec\":314.5,\"end_sec\":331,\"start_ms\":314500,"
		"\"end_ms\":331000,\"confidence\":1,\"submission_count\":5},"
		"\"recap\":null,\"outro\":{\"start_ms\":2843000,\"end_ms\":2901000,"
		"\"submission_count\":1},\"post_credits\":null}");
	CHECK(introDB.intros.size() == 1 && introDB.introVotes == 5);
	CHECK(introDB.intros[0].start == 314500000
		&& introDB.intros[0].end == 331000000);
	CHECK(introDB.credits.size() == 1 && introDB.credits[0].end == 2901000000);

	SkipSegments theIntroDB = parse_theintrodb("{\"tvdb_id\":81189,"
		"\"type\":\"tv\",\"season\":1,\"episode\":2,\"intro\":[{\"start_ms\":"
		"null,\"end_ms\":23000,\"submission_count\":2}],\"credits\":"
		"[{\"start_ms\":2847619,\"end_ms\":null,\"submission_count\":3}]}");
	CHECK(theIntroDB.intros.size() == 1 && theIntroDB.intros[0].start == 0
		&& theIntroDB.intros[0].end == 23000000);
	CHECK(theIntroDB.credits.size() == 1
		&& theIntroDB.credits[0].start == 2847619000
		&& theIntroDB.credits[0].end == -1);
	CHECK(parse_theintrodb("{\"error\":\"no submissions\"}").Empty());
	CHECK(parse_introdb("not json").Empty());

	// The intro from IntroDB (5 votes against 2), the credits from
	// TheIntroDB (3 against 1).
	SkipSegments merged = merge_skip_segments(theIntroDB, introDB);
	CHECK(merged.intros.size() == 1 && merged.intros[0].start == 314500000);
	CHECK(merged.credits.size() == 1 && merged.credits[0].end == -1);
	merged = merge_skip_segments(SkipSegments(), introDB);
	CHECK(merged.credits.size() == 1 && merged.credits[0].start == 2843000000);

	int64_t duration = 2950000000;
	int64_t target = 0;
	merged = merge_skip_segments(theIntroDB, introDB);
	CHECK(active_skip(merged, 0, duration, &target) == kSkipNone);
	CHECK(active_skip(merged, 314500000, duration, &target) == kSkipIntro);
	CHECK(target == 331000000);
	CHECK(active_skip(merged, 330500000, duration, &target) == kSkipNone);
	CHECK(active_skip(merged, 2900000000, duration, &target) == kSkipCredits);
	CHECK(target == duration);
	// Credits timed for a release longer than this file.
	CHECK(active_skip(merged, 2900000000, 2800000000, &target) == kSkipNone);
	CHECK(active_skip(SkipSegments(), 1000, duration, &target) == kSkipNone);

	CHECK_EQUAL_STRING(url_encode("Grey's Anatomy & Co"),
		"Grey%27s%20Anatomy%20%26%20Co");
}


int
main()
{
	printf("airTime engine tests\n");
	test_time();
	test_skip_segments();
	test_languages();
	test_ass();
	test_track();
	test_files(getenv("AIRTIME_TEST_MEDIA"));
	test_captions();
	test_sps();
	test_scaler();
	printf("%d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
