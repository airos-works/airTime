/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "SkipLookup.h"

#include <stdio.h>

#include <map>
#include <mutex>
#include <new>

#include <DataIO.h>
#include <OS.h>
#include <Url.h>

#include <HttpRequest.h>
#include <HttpResult.h>
#include <UrlProtocolRoster.h>
#include <UrlRequest.h>

#include "AppInfo.h"
#include "Messages.h"


using BPrivate::Network::BHttpRequest;
using BPrivate::Network::BHttpResult;
using BPrivate::Network::BUrlProtocolRoster;
using BPrivate::Network::BUrlRequest;


namespace airtime {

static const bigtime_t kRequestTimeout = 10000000;


/*!	A GET, its body into body. False when it could not be made or the server
	did not answer 200. Certificates are checked (the default context turns
	down one that does not verify). */
static bool
http_get(const std::string& url, std::string& body, int32* statusCode = NULL)
{
	if (statusCode != NULL)
		*statusCode = 0;
	BUrl address(url.c_str(), false);
	if (!address.IsValid())
		return false;

	BMallocIO output;
	BUrlRequest* request = BUrlProtocolRoster::MakeRequest(address, &output);
	BHttpRequest* http = dynamic_cast<BHttpRequest*>(request);
	if (http == NULL) {
		delete request;
		return false;
	}
	BString agent;
	agent.SetToFormat("airTime/%s (air/OS)", kAppVersion);
	http->SetUserAgent(agent);
	http->SetFollowLocation(true);
	http->SetTimeout(kRequestTimeout);

	thread_id thread = request->Run();
	if (thread < 0) {
		delete request;
		return false;
	}
	bigtime_t until = system_time() + kRequestTimeout;
	bool stopped = false;
	while (wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 250000, NULL)
			== B_TIMED_OUT) {
		if (!stopped && system_time() > until) {
			request->Stop();
			stopped = true;
		}
	}

	int32 status = 0;
	const BHttpResult* result
		= dynamic_cast<const BHttpResult*>(&request->Result());
	if (result != NULL)
		status = result->StatusCode();
	bool ok = request->Status() == B_OK && status == 200;
	delete request;
	if (statusCode != NULL)
		*statusCode = status;
	if (!ok)
		return false;
	body.assign((const char*)output.Buffer(), output.BufferLength());
	return true;
}


// The series ids found by name, for the next episode of the same series.
static std::mutex sSeriesLock;
static std::map<std::string, EpisodeInfo> sSeries;


static bool
find_series(EpisodeInfo& episode)
{
	std::string key = episode.show + "|" + std::to_string(episode.year);
	{
		std::lock_guard<std::mutex> lock(sSeriesLock);
		auto found = sSeries.find(key);
		if (found != sSeries.end()) {
			episode.imdbId = found->second.imdbId;
			episode.tvdbId = found->second.tvdbId;
			return true;
		}
	}

	std::string body;
	if (!http_get("https://api.tvmaze.com/search/shows?q="
			+ url_encode(episode.show), body)
		|| !parse_tvmaze_search(body, episode)) {
		return false;
	}
	std::lock_guard<std::mutex> lock(sSeriesLock);
	sSeries[key] = episode;
	return true;
}


static BString
describe(const SkipSegment& segment)
{
	BString text;
	text.SetToFormat("%" B_PRId64 ".%01" B_PRId64 "–", segment.start / 1000000,
		segment.start / 100000 % 10);
	if (segment.end < 0)
		text << "end";
	else {
		text << BString().SetToFormat("%" B_PRId64 ".%01" B_PRId64 " s",
			segment.end / 1000000, segment.end / 100000 % 10);
	}
	return text;
}


struct LookupJob {
	EpisodeInfo		episode;
	bigtime_t		duration;
	BMessenger		target;
	int32			generation;
};


static status_t
lookup_thread(void* data)
{
	LookupJob* job = (LookupJob*)data;
	EpisodeInfo& episode = job->episode;

	if (!find_series(episode)) {
		fprintf(stderr, "airTime: \"%s\" not found on TVmaze; no intro or "
			"credits times\n", episode.show.c_str());
		delete job;
		return B_OK;
	}

	SkipSegments theIntroDB;
	std::string body;
	std::string query = "season=" + std::to_string(episode.season)
		+ "&episode=" + std::to_string(episode.episode);
	if (episode.tvdbId > 0 || !episode.imdbId.empty()) {
		std::string url = "https://api.theintrodb.org/v4/media?";
		if (episode.tvdbId > 0)
			url += "tvdb_id=" + std::to_string(episode.tvdbId);
		else
			url += "imdb_id=" + url_encode(episode.imdbId);
		url += "&" + query + "&details=true";
		if (job->duration > 0)
			url += "&duration_ms=" + std::to_string(job->duration / 1000);
		if (http_get(url, body))
			theIntroDB = parse_theintrodb(body);
	}

	// IntroDB numbers from 1: no specials.
	SkipSegments introDB;
	if (!episode.imdbId.empty() && episode.season >= 1 && episode.episode >= 1) {
		std::string url = "https://api.introdb.app/segments?imdb_id="
			+ url_encode(episode.imdbId) + "&" + query;
		body.clear();
		if (http_get(url, body))
			introDB = parse_introdb(body);
	}

	SkipSegments segments = merge_skip_segments(theIntroDB, introDB);
	BString found;
	for (const SkipSegment& segment : segments.intros)
		found << " intro " << describe(segment);
	for (const SkipSegment& segment : segments.credits)
		found << " credits " << describe(segment);
	fprintf(stderr, "airTime: %s S%02dE%02d (%s, TVDB %d):%s\n",
		episode.show.c_str(), episode.season, episode.episode,
		episode.imdbId.empty() ? "no IMDb id" : episode.imdbId.c_str(),
		episode.tvdbId, found.Length() > 0 ? found.String()
			: " no intro or credits times");

	if (!segments.Empty()) {
		BMessage message(kMsgSkipSegmentsFound);
		message.AddInt32("generation", job->generation);
		add_skip_segments(message, segments);
		job->target.SendMessage(&message);
	}
	delete job;
	return B_OK;
}


void
start_skip_lookup(const EpisodeInfo& episode, bigtime_t duration,
	const BMessenger& target, int32 generation)
{
	LookupJob* job = new(std::nothrow) LookupJob;
	if (job == NULL)
		return;
	job->episode = episode;
	job->duration = duration;
	job->target = target;
	job->generation = generation;
	thread_id thread = spawn_thread(lookup_thread, "intro lookup",
		B_LOW_PRIORITY, job);
	if (thread < 0 || resume_thread(thread) != B_OK) {
		delete job;
		return;
	}
}


static void
add_list(BMessage& message, const char* name,
	const std::vector<SkipSegment>& list)
{
	for (const SkipSegment& segment : list) {
		message.AddInt64(name, segment.start);
		message.AddInt64(name, segment.end);
	}
}


static void
find_list(const BMessage& message, const char* name,
	std::vector<SkipSegment>& list)
{
	int64 start, end;
	for (int32 i = 0; message.FindInt64(name, i, &start) == B_OK
			&& message.FindInt64(name, i + 1, &end) == B_OK; i += 2) {
		SkipSegment segment;
		segment.start = start;
		segment.end = end;
		list.push_back(segment);
	}
}


void
add_skip_segments(BMessage& message, const SkipSegments& segments)
{
	add_list(message, "intro", segments.intros);
	add_list(message, "credits", segments.credits);
}


SkipSegments
find_skip_segments(const BMessage& message)
{
	SkipSegments segments;
	find_list(message, "intro", segments.intros);
	find_list(message, "credits", segments.credits);
	return segments;
}

}	// namespace airtime
