/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_SKIP_LOOKUP_H
#define AIRTIME_SKIP_LOOKUP_H


#include <Message.h>
#include <Messenger.h>

#include "SkipSegments.h"


namespace airtime {

/*!	Asks the web for when an episode's intro and credits are: TVmaze for the
	series' ids, then IntroDB (introdb.app) and TheIntroDB (theintrodb.org)
	for the times. Runs on a thread of its own; what it finds arrives at the
	target as kMsgSkipSegmentsFound with the generation it was started with,
	and nothing arrives when nothing is found. */
void start_skip_lookup(const EpisodeInfo& episode, bigtime_t duration,
	const BMessenger& target, int32 generation);

// The segments in and out of a message, as pairs of int64 times.
void add_skip_segments(BMessage& message, const SkipSegments& segments);
SkipSegments find_skip_segments(const BMessage& message);

}	// namespace airtime

#endif	// AIRTIME_SKIP_LOOKUP_H
