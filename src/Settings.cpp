/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Settings.h"

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>


namespace airtime {


static Settings sSettings;


Settings&
settings()
{
	return sSettings;
}


Settings::Settings()
	:
	volume(0.8f),
	muted(false),
	subtitleScale(1.0f),
	loop(false),
	hardwareDecoding(true),
	noHardwareThisRun(false),
	snapToAspect(true),
	devicePixels(true),
	autoPlay(true),
	skipLookup(true),
	timeDisplay(0),
	inspectorFrame(0, 0, -1, -1)
{
}


static BPath
settings_path()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path, true) != B_OK)
		return BPath();
	path.Append("airTime");
	create_directory(path.Path(), 0755);
	path.Append("settings");
	return path;
}


void
Settings::Load()
{
	BPath path = settings_path();
	BFile file(path.Path(), B_READ_ONLY);
	BMessage message;
	if (file.InitCheck() != B_OK || message.Unflatten(&file) != B_OK)
		return;
	message.FindFloat("volume", &volume);
	message.FindBool("muted", &muted);
	message.FindFloat("subtitle scale", &subtitleScale);
	message.FindBool("loop", &loop);
	message.FindBool("hardware decoding", &hardwareDecoding);
	message.FindBool("snap to aspect", &snapToAspect);
	message.FindBool("device pixels", &devicePixels);
	message.FindBool("auto play", &autoPlay);
	// Find*() zeroes what it does not find: new settings need a default.
	skipLookup = message.GetBool("skip lookup", true);
	message.FindInt32("time display", &timeDisplay);
	message.FindString("audio language", &audioLanguage);
	message.FindString("subtitle language", &subtitleLanguage);
	message.FindRect("inspector frame", &inspectorFrame);
	if (volume < 0 || volume > 1)
		volume = 0.8f;
	if (subtitleScale < 0.5f || subtitleScale > 2.5f)
		subtitleScale = 1.0f;
}


void
Settings::Save() const
{
	BMessage message('airT');
	message.AddFloat("volume", volume);
	message.AddBool("muted", muted);
	message.AddFloat("subtitle scale", subtitleScale);
	message.AddBool("loop", loop);
	message.AddBool("hardware decoding", hardwareDecoding);
	message.AddBool("snap to aspect", snapToAspect);
	message.AddBool("device pixels", devicePixels);
	message.AddBool("auto play", autoPlay);
	message.AddBool("skip lookup", skipLookup);
	message.AddInt32("time display", timeDisplay);
	message.AddString("audio language", audioLanguage);
	message.AddString("subtitle language", subtitleLanguage);
	message.AddRect("inspector frame", inspectorFrame);

	BPath path = settings_path();
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		message.Flatten(&file);
}

}	// namespace airtime
