/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_SKIP_SEGMENTS_H
#define AIRTIME_SKIP_SEGMENTS_H


#include <stdint.h>

#include <map>
#include <string>
#include <vector>


namespace airtime {

/*!	Which episode of which series a file is, as far as its name, its folders
	and its tags tell. */
struct EpisodeInfo {
	std::string			show;		// series name, cleaned up for a search
	int					year = 0;	// first aired, when the name carries it
	int					season = -1;
	int					episode = -1;
	// Filled in by the series search (TVmaze).
	std::string			imdbId;		// the series' IMDb id, "tt…"
	int					tvdbId = 0;	// the series' TheTVDB id

	bool				IsEpisode() const
							{ return !show.empty() && season >= 0
								&& episode >= 0; }
};

// Tag names lower case, as FFmpeg reports the container's tags.
typedef std::map<std::string, std::string> TagMap;

/*!	A film is taken for a television episode when its tags say so (the
	iTunes "show", "season_number" and "episode_sort") or its file name does:
	"Show.Name.S01E02.mkv", "Show Name - 1x02 - Title.mp4", or
	"Show Name/Season 1/02 - Title.mkv". Returns an info that is not
	IsEpisode() for anything else. */
EpisodeInfo identify_episode(const std::string& path, const TagMap& tags);


/*!	A stretch of the film that can be skipped, in microseconds. An end below
	zero means the end of the file. */
struct SkipSegment {
	int64_t				start = 0;
	int64_t				end = -1;
};

struct SkipSegments {
	std::vector<SkipSegment> intros;
	std::vector<SkipSegment> credits;
	// How many submissions agree on them, to choose between the databases.
	int					introVotes = 0;
	int					creditVotes = 0;

	bool				Empty() const
							{ return intros.empty() && credits.empty(); }
};

/*!	A parsed JSON value: just what the web services' answers need. */
struct JsonValue {
	enum Type { kNull, kBool, kNumber, kString, kArray, kObject };
	Type				type = kNull;
	bool				boolean = false;
	double				number = 0;
	std::string			string;
	std::vector<JsonValue> items;
	std::vector<std::pair<std::string, JsonValue> > members;

	// A member of an object; a null value when there is none.
	const JsonValue&	operator[](const char* key) const;
	bool				IsNull() const { return type == kNull; }
};

bool parse_json(const std::string& text, JsonValue& value);

// TVmaze's /search/shows answer: the series' ids into info, choosing the
// result whose premiere matches info.year when there is one.
bool parse_tvmaze_search(const std::string& json, EpisodeInfo& info);
// IntroDB's GET /segments.
SkipSegments parse_introdb(const std::string& json);
// TheIntroDB's GET /v4/media.
SkipSegments parse_theintrodb(const std::string& json);
// Per kind, the database with more submissions behind it; TheIntroDB, which
// picks the release by its length, when they are even.
SkipSegments merge_skip_segments(const SkipSegments& theIntroDB,
	const SkipSegments& introDB);

// Percent-encodes a query parameter's value.
std::string url_encode(const std::string& text);


enum SkipKind {
	kSkipNone = 0,
	kSkipIntro,
	kSkipCredits
};

/*!	What can be skipped at a position, and the time to go to. Nothing in the
	last second of a segment, nor for a segment the film is too short for. */
SkipKind active_skip(const SkipSegments& segments, int64_t position,
	int64_t duration, int64_t* target);

}	// namespace airtime

#endif	// AIRTIME_SKIP_SEGMENTS_H
