/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "SkipSegments.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <regex>


namespace airtime {


// #pragma mark - which episode


static std::string
lower(std::string text)
{
	for (char& c : text)
		c = tolower((unsigned char)c);
	return text;
}


static std::string
trim(const std::string& text, const char* junk = " \t-_.")
{
	size_t start = text.find_first_not_of(junk);
	if (start == std::string::npos)
		return "";
	size_t end = text.find_last_not_of(junk);
	return text.substr(start, end - start + 1);
}


static std::string
base_name(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}


static std::string
parent_path(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	if (slash == std::string::npos || slash == 0)
		return "";
	return path.substr(0, slash);
}


static std::string
strip_extension(const std::string& name)
{
	size_t dot = name.find_last_of('.');
	if (dot == std::string::npos || dot == 0 || name.size() - dot > 5)
		return name;
	return name.substr(0, dot);
}


/*!	"Show.Name.(2008)" or "[Group] Show_Name 2008 -" to "Show Name", with
	the year apart. */
static std::string
clean_show_name(std::string name, int* year)
{
	// A release group in brackets at the start.
	while (!name.empty() && (name[0] == '[' || name[0] == '{')) {
		size_t close = name.find(name[0] == '[' ? ']' : '}');
		if (close == std::string::npos)
			break;
		name.erase(0, close + 1);
	}
	for (char& c : name) {
		if (c == '.' || c == '_')
			c = ' ';
	}
	name = trim(name, " \t-");

	static const std::regex sBracketYear(
		"^(.*\\S)\\s*[\\(\\[]((?:19|20)\\d\\d)[\\)\\]]$");
	static const std::regex sYear("^(.*\\S)\\s+((?:19|20)\\d\\d)$");
	std::smatch match;
	if (std::regex_match(name, match, sBracketYear)
		|| std::regex_match(name, match, sYear)) {
		*year = atoi(match[2].str().c_str());
		name = trim(match[1].str(), " \t-");
	}

	std::string collapsed;
	for (char c : name) {
		if (c == ' ' && (collapsed.empty() || collapsed.back() == ' '))
			continue;
		collapsed += c;
	}
	return trim(collapsed, " ");
}


/*!	"Season 2", "Season.02", "S02" or "Specials": the season a folder holds,
	or -1. */
static int
season_folder(const std::string& name)
{
	static const std::regex sSeason("^\\s*(?:season|series|staffel|saison)"
		"[\\s._-]*(\\d{1,4})\\s*$", std::regex::icase);
	static const std::regex sShort("^\\s*s(\\d{1,4})\\s*$", std::regex::icase);
	std::smatch match;
	if (std::regex_match(name, match, sSeason)
		|| std::regex_match(name, match, sShort)) {
		return atoi(match[1].str().c_str());
	}
	if (lower(trim(name, " ")) == "specials")
		return 0;
	return -1;
}


static int
tag_number(const TagMap& tags, const char* key)
{
	TagMap::const_iterator found = tags.find(key);
	if (found == tags.end())
		return -1;
	const char* text = found->second.c_str();
	while (*text == ' ')
		text++;
	if (!isdigit((unsigned char)*text))
		return -1;
	// "2/10" (track style) is episode 2.
	return atoi(text);
}


EpisodeInfo
identify_episode(const std::string& path, const TagMap& tags)
{
	EpisodeInfo info;

	std::string name = strip_extension(base_name(path));
	std::string folder = parent_path(path);
	std::string folderName = base_name(folder);
	int folderSeason = season_folder(folderName);

	// The series' folder: the one above the season's, if there is one.
	std::string seriesFolder = folderName;
	if (folderSeason >= 0)
		seriesFolder = base_name(parent_path(folder));

	std::string prefix;
	bool found = false;
	std::smatch match;

	static const std::regex sSxxExx(
		"(?:^|[^a-z0-9])s(\\d{1,4})[\\s._-]*e(\\d{1,4})(?![0-9])",
		std::regex::icase);
	static const std::regex sNxNN(
		"(?:^|[^a-z0-9])(\\d{1,2})x(\\d{2,3})(?![0-9])", std::regex::icase);
	static const std::regex sWords(
		"(?:^|[^a-z0-9])season[\\s._-]*(\\d{1,4})[\\s._,-]*episode[\\s._-]*"
		"(\\d{1,4})(?![0-9])", std::regex::icase);
	if (std::regex_search(name, match, sSxxExx)
		|| std::regex_search(name, match, sNxNN)
		|| std::regex_search(name, match, sWords)) {
		info.season = atoi(match[1].str().c_str());
		info.episode = atoi(match[2].str().c_str());
		prefix = match.prefix().str();
		found = true;
	} else if (folderSeason >= 0) {
		// "Show/Season 1/E02 - Title", "Episode 2", or "02 - Title".
		static const std::regex sEpisode(
			"(?:^|[^a-z0-9])(?:e|ep|episode)[\\s._-]*(\\d{1,4})(?![0-9])",
			std::regex::icase);
		static const std::regex sLeading("^\\s*(\\d{1,3})(?:[\\s._-]|$)");
		if (std::regex_search(name, match, sEpisode)
			|| std::regex_search(name, match, sLeading)) {
			info.season = folderSeason;
			info.episode = atoi(match[1].str().c_str());
			prefix = match.prefix().str();
			found = true;
		}
	}

	if (found) {
		info.show = clean_show_name(prefix, &info.year);
		if (info.show.empty())
			info.show = clean_show_name(seriesFolder, &info.year);
	}

	// iTunes style tags (MP4) win over the name.
	TagMap::const_iterator show = tags.find("show");
	int tagSeason = tag_number(tags, "season_number");
	int tagEpisode = tag_number(tags, "episode_sort");
	if (show != tags.end() && trim(show->second, " ").length() > 0
		&& tagSeason >= 0 && tagEpisode >= 0) {
		int year = 0;
		info.show = clean_show_name(show->second, &year);
		if (year != 0)
			info.year = year;
		info.season = tagSeason;
		info.episode = tagEpisode;
	}

	if (!info.IsEpisode()) {
		EpisodeInfo none;
		return none;
	}
	return info;
}


// #pragma mark - JSON


const JsonValue&
JsonValue::operator[](const char* key) const
{
	static const JsonValue sNull;
	if (type != kObject)
		return sNull;
	for (const auto& member : members) {
		if (member.first == key)
			return member.second;
	}
	return sNull;
}


namespace {

class JsonParser {
public:
	JsonParser(const std::string& text)
		:
		fText(text),
		fPosition(0),
		fDepth(0)
	{
	}

	bool Parse(JsonValue& value)
	{
		if (!_Value(value))
			return false;
		_Space();
		return fPosition == fText.size();
	}

private:
	void _Space()
	{
		while (fPosition < fText.size() && isspace((unsigned char)fText[fPosition]))
			fPosition++;
	}

	bool _Literal(const char* word)
	{
		size_t length = strlen(word);
		if (fText.compare(fPosition, length, word) != 0)
			return false;
		fPosition += length;
		return true;
	}

	static void _AppendUtf8(std::string& out, uint32_t code)
	{
		if (code < 0x80)
			out += (char)code;
		else if (code < 0x800) {
			out += (char)(0xc0 | (code >> 6));
			out += (char)(0x80 | (code & 0x3f));
		} else if (code < 0x10000) {
			out += (char)(0xe0 | (code >> 12));
			out += (char)(0x80 | ((code >> 6) & 0x3f));
			out += (char)(0x80 | (code & 0x3f));
		} else {
			out += (char)(0xf0 | (code >> 18));
			out += (char)(0x80 | ((code >> 12) & 0x3f));
			out += (char)(0x80 | ((code >> 6) & 0x3f));
			out += (char)(0x80 | (code & 0x3f));
		}
	}

	bool _Hex4(uint32_t& code)
	{
		if (fPosition + 4 > fText.size())
			return false;
		code = 0;
		for (int i = 0; i < 4; i++) {
			char c = fText[fPosition++];
			code <<= 4;
			if (c >= '0' && c <= '9')
				code |= c - '0';
			else if (c >= 'a' && c <= 'f')
				code |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				code |= c - 'A' + 10;
			else
				return false;
		}
		return true;
	}

	bool _String(std::string& out)
	{
		// At the opening quote.
		fPosition++;
		while (fPosition < fText.size()) {
			char c = fText[fPosition++];
			if (c == '"')
				return true;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (fPosition >= fText.size())
				return false;
			c = fText[fPosition++];
			switch (c) {
				case '"': case '\\': case '/': out += c; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u':
				{
					uint32_t code;
					if (!_Hex4(code))
						return false;
					if (code >= 0xd800 && code < 0xdc00
						&& _Literal("\\u")) {
						uint32_t low;
						if (!_Hex4(low))
							return false;
						if (low >= 0xdc00 && low < 0xe000)
							code = 0x10000 + ((code - 0xd800) << 10)
								+ (low - 0xdc00);
					}
					_AppendUtf8(out, code);
					break;
				}
				default:
					return false;
			}
		}
		return false;
	}

	bool _Value(JsonValue& value)
	{
		_Space();
		if (fPosition >= fText.size() || fDepth > 64)
			return false;
		char c = fText[fPosition];
		if (c == '{') {
			value.type = JsonValue::kObject;
			fPosition++;
			fDepth++;
			_Space();
			if (fPosition < fText.size() && fText[fPosition] == '}') {
				fPosition++;
				fDepth--;
				return true;
			}
			while (true) {
				_Space();
				if (fPosition >= fText.size() || fText[fPosition] != '"')
					return false;
				std::string key;
				if (!_String(key))
					return false;
				_Space();
				if (fPosition >= fText.size() || fText[fPosition] != ':')
					return false;
				fPosition++;
				value.members.emplace_back(key, JsonValue());
				if (!_Value(value.members.back().second))
					return false;
				_Space();
				if (fPosition >= fText.size())
					return false;
				if (fText[fPosition] == ',') {
					fPosition++;
					continue;
				}
				if (fText[fPosition] != '}')
					return false;
				fPosition++;
				fDepth--;
				return true;
			}
		}
		if (c == '[') {
			value.type = JsonValue::kArray;
			fPosition++;
			fDepth++;
			_Space();
			if (fPosition < fText.size() && fText[fPosition] == ']') {
				fPosition++;
				fDepth--;
				return true;
			}
			while (true) {
				value.items.emplace_back();
				if (!_Value(value.items.back()))
					return false;
				_Space();
				if (fPosition >= fText.size())
					return false;
				if (fText[fPosition] == ',') {
					fPosition++;
					continue;
				}
				if (fText[fPosition] != ']')
					return false;
				fPosition++;
				fDepth--;
				return true;
			}
		}
		if (c == '"') {
			value.type = JsonValue::kString;
			return _String(value.string);
		}
		if (_Literal("true")) {
			value.type = JsonValue::kBool;
			value.boolean = true;
			return true;
		}
		if (_Literal("false")) {
			value.type = JsonValue::kBool;
			return true;
		}
		if (_Literal("null")) {
			value.type = JsonValue::kNull;
			return true;
		}
		if (c == '-' || isdigit((unsigned char)c)) {
			const char* start = fText.c_str() + fPosition;
			char* end;
			value.number = strtod(start, &end);
			if (end == start)
				return false;
			value.type = JsonValue::kNumber;
			fPosition += end - start;
			return true;
		}
		return false;
	}

	const std::string&	fText;
	size_t				fPosition;
	int					fDepth;
};

}	// namespace


bool
parse_json(const std::string& text, JsonValue& value)
{
	value = JsonValue();
	JsonParser parser(text);
	return parser.Parse(value);
}


// #pragma mark - the web services


bool
parse_tvmaze_search(const std::string& json, EpisodeInfo& info)
{
	JsonValue root;
	if (!parse_json(json, root) || root.type != JsonValue::kArray)
		return false;

	const JsonValue* chosen = NULL;
	for (const JsonValue& result : root.items) {
		const JsonValue& show = result["show"];
		if (show.type != JsonValue::kObject)
			continue;
		if (chosen == NULL)
			chosen = &show;
		if (info.year == 0)
			break;
		const JsonValue& premiered = show["premiered"];
		if (premiered.type == JsonValue::kString
			&& atoi(premiered.string.c_str()) == info.year) {
			chosen = &show;
			break;
		}
	}
	if (chosen == NULL)
		return false;

	const JsonValue& externals = (*chosen)["externals"];
	const JsonValue& imdb = externals["imdb"];
	const JsonValue& tvdb = externals["thetvdb"];
	if (imdb.type == JsonValue::kString
		&& imdb.string.compare(0, 2, "tt") == 0) {
		info.imdbId = imdb.string;
	}
	if (tvdb.type == JsonValue::kNumber && tvdb.number > 0)
		info.tvdbId = (int)tvdb.number;
	return !info.imdbId.empty() || info.tvdbId > 0;
}


static bool
number_field(const JsonValue& object, const char* key, int64_t* value)
{
	const JsonValue& field = object[key];
	if (field.type != JsonValue::kNumber)
		return false;
	*value = (int64_t)field.number;
	return true;
}


/*!	One segment in milliseconds to microseconds. Without a start an intro
	begins with the film; without an end, credits run to its end. */
static bool
make_segment(const JsonValue& object, bool credits, SkipSegment& segment,
	int* votes)
{
	if (object.type != JsonValue::kObject)
		return false;
	int64_t start = 0;
	int64_t end = -1;
	bool hasStart = number_field(object, "start_ms", &start);
	bool hasEnd = number_field(object, "end_ms", &end);
	if (credits ? !hasStart : !hasEnd)
		return false;
	if (!hasStart)
		start = 0;
	if (start < 0 || (hasEnd && end <= start))
		return false;
	segment.start = start * 1000;
	segment.end = hasEnd ? end * 1000 : -1;

	int64_t count = 1;
	number_field(object, "submission_count", &count);
	*votes = std::max(*votes, (int)count);
	return true;
}


SkipSegments
parse_introdb(const std::string& json)
{
	SkipSegments segments;
	JsonValue root;
	if (!parse_json(json, root) || root.type != JsonValue::kObject)
		return segments;

	SkipSegment segment;
	if (make_segment(root["intro"], false, segment, &segments.introVotes))
		segments.intros.push_back(segment);
	if (make_segment(root["outro"], true, segment, &segments.creditVotes))
		segments.credits.push_back(segment);
	return segments;
}


SkipSegments
parse_theintrodb(const std::string& json)
{
	SkipSegments segments;
	JsonValue root;
	if (!parse_json(json, root) || root.type != JsonValue::kObject)
		return segments;

	SkipSegment segment;
	for (const JsonValue& item : root["intro"].items) {
		if (make_segment(item, false, segment, &segments.introVotes))
			segments.intros.push_back(segment);
	}
	for (const JsonValue& item : root["credits"].items) {
		if (make_segment(item, true, segment, &segments.creditVotes))
			segments.credits.push_back(segment);
	}
	return segments;
}


SkipSegments
merge_skip_segments(const SkipSegments& theIntroDB, const SkipSegments& introDB)
{
	SkipSegments merged;
	bool introFromFirst = !theIntroDB.intros.empty()
		&& (introDB.intros.empty()
			|| theIntroDB.introVotes >= introDB.introVotes);
	const SkipSegments& intro = introFromFirst ? theIntroDB : introDB;
	merged.intros = intro.intros;
	merged.introVotes = intro.introVotes;

	bool creditsFromFirst = !theIntroDB.credits.empty()
		&& (introDB.credits.empty()
			|| theIntroDB.creditVotes >= introDB.creditVotes);
	const SkipSegments& credits = creditsFromFirst ? theIntroDB : introDB;
	merged.credits = credits.credits;
	merged.creditVotes = credits.creditVotes;
	return merged;
}


std::string
url_encode(const std::string& text)
{
	static const char* kHex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : text) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else {
			out += '%';
			out += kHex[c >> 4];
			out += kHex[c & 15];
		}
	}
	return out;
}


// #pragma mark - while playing


static const int64_t kHideBeforeEnd = 1000000;


SkipKind
active_skip(const SkipSegments& segments, int64_t position, int64_t duration,
	int64_t* target)
{
	for (const SkipSegment& segment : segments.intros) {
		int64_t end = segment.end;
		if (duration > 0) {
			// Timed for a longer release than this one.
			if (segment.start >= duration)
				continue;
			end = std::min(end, duration);
		}
		if (position >= segment.start && position < end - kHideBeforeEnd) {
			*target = end;
			return kSkipIntro;
		}
	}
	for (const SkipSegment& segment : segments.credits) {
		if (duration <= 0 || segment.start >= duration)
			continue;
		int64_t end = segment.end < 0 ? duration
			: std::min(segment.end, duration);
		if (position >= segment.start && position < end - kHideBeforeEnd) {
			*target = end;
			return kSkipCredits;
		}
	}
	return kSkipNone;
}

}	// namespace airtime
