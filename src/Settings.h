/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_SETTINGS_H
#define AIRTIME_SETTINGS_H


#include <Rect.h>
#include <String.h>


namespace airtime {

struct Settings {
	float		volume;
	bool		muted;
	float		subtitleScale;
	bool		loop;
	bool		hardwareDecoding;
	// --no-hardware: off for this run only, not saved.
	bool		noHardwareThisRun;
	bool		snapToAspect;
	// Draw straight into the frame buffer at the screen's density where
	// the app_server allows it (B_DIRECT_DEVICE_PIXELS).
	bool		devicePixels;
	bool		autoPlay;
	// Look up when TV episodes' intros and credits are, on the web.
	bool		skipLookup;
	int32		timeDisplay;
	BString		audioLanguage;		// "" = the file's default
	BString		subtitleLanguage;	// "" = off unless forced
	BRect		inspectorFrame;

				Settings();
	void		Load();
	void		Save() const;
};

Settings& settings();

}	// namespace airtime

#endif	// AIRTIME_SETTINGS_H
