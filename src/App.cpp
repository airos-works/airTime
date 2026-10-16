/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "App.h"

#include "MediaKeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <AboutWindow.h>
#include <Alert.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Mime.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>

#include "AppInfo.h"
#include "FFmpeg.h"
#include "Messages.h"
#include "PlayerWindow.h"
#include "Screens.h"
#include "Settings.h"
#include "Tracks.h"
#include "VideoDecoder.h"


namespace airtime {


// The types airTime offers to open, and makes itself the player for.
static const char* const kMediaTypes[] = {
	"video/mp4", "video/quicktime", "video/x-matroska", "video/webm",
	"video/x-msvideo", "video/avi", "video/mpeg", "video/mp2t", "video/MP2T",
	"video/x-m4v", "video/3gpp", "video/3gpp2", "video/x-flv", "video/ogg",
	"video/x-ms-wmv", "video/x-ms-asf", "video/dv", "video/x-mng",
	"video/vnd.avi", "video/h264", "video/h265", "video/AV1",
	"audio/mpeg", "audio/mp4", "audio/x-m4a", "audio/aac", "audio/x-aac",
	"audio/flac", "audio/x-flac", "audio/ogg", "audio/x-vorbis+ogg",
	"audio/opus", "audio/x-wav", "audio/wav", "audio/x-aiff", "audio/aiff",
	"audio/x-matroska", "audio/webm", "audio/x-ms-wma", "audio/ac3",
	"audio/x-ape", "audio/x-wavpack", "audio/x-mpegurl",
	NULL
};


App::App()
	:
	BApplication(kAppSignature),
	fOpenPanel(NULL),
	fWindowCount(0),
	fFullScreen(false),
	fStartAt(-1),
	fPlay(true)
{
	settings().Load();
	fPlay = settings().autoPlay;
	av_log_set_level(getenv("AIRTIME_TRACE") != NULL
		? AV_LOG_INFO : AV_LOG_ERROR);
}


App::~App()
{
	delete fOpenPanel;
}


BRect
App::_NextFrame()
{
	// On the monitor the user is looking at (where the pointer is), a little
	// further down and right for every window.
	BRect monitor = monitor_frame_at_pointer();
	BRect frame(0, 0, 535, 400);
	float offset = 24.0f * (fWindowCount % 8);
	frame.OffsetTo(monitor.left + 80 + offset, monitor.top + 60 + offset);
	fWindowCount++;
	return frame;
}


PlayerWindow*
App::_NewWindow()
{
	PlayerWindow* window = new PlayerWindow(_NextFrame());
	window->Show();
	return window;
}


PlayerWindow*
App::_EmptyWindow()
{
	for (int32 i = 0; BWindow* window = WindowAt(i); i++) {
		PlayerWindow* player = dynamic_cast<PlayerWindow*>(window);
		if (player == NULL)
			continue;
		if (player->Lock()) {
			bool empty = player->IsEmpty();
			player->Unlock();
			if (empty)
				return player;
		}
	}
	return NULL;
}


void
App::_Open(const entry_ref& ref, PlayerWindow* window)
{
	BString name(ref.name);
	name.ToLower();
	if (name.EndsWith(".srt") || name.EndsWith(".ass") || name.EndsWith(".ssa")
		|| name.EndsWith(".vtt") || name.EndsWith(".idx")) {
		// Subtitles opened on their own go to the frontmost film.
		for (int32 i = 0; BWindow* candidate = WindowAt(i); i++) {
			PlayerWindow* player = dynamic_cast<PlayerWindow*>(candidate);
			if (player != NULL) {
				BMessage message(B_REFS_RECEIVED);
				message.AddRef("refs", &ref);
				player->PostMessage(&message);
				return;
			}
		}
		return;
	}

	if (window == NULL)
		window = _EmptyWindow();
	if (window == NULL)
		window = _NewWindow();
	if (!window->Lock())
		return;
	if (fStartAt >= 0) {
		window->SeekOnOpen(fStartAt);
		fStartAt = -1;
	}
	BString error;
	status_t status = window->OpenFile(ref, fPlay, &error);
	if (status == B_OK && fFullScreen)
		window->SetFullScreen(true);
	window->Unlock();
	if (status != B_OK) {
		BString text;
		text.SetToFormat("“%s” cannot be played.\n\n%s", ref.name,
			error.String());
		BAlert* alert = new BAlert("airTime", text.String(), "OK", NULL, NULL,
			B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->Go(NULL);
	}
}


void
App::ArgvReceived(int32 argc, char** argv)
{
	for (int32 i = 1; i < argc; i++) {
		const char* argument = argv[i];
		if (strcmp(argument, "--fullscreen") == 0
			|| strcmp(argument, "-f") == 0) {
			fFullScreen = true;
			continue;
		}
		if (strcmp(argument, "--paused") == 0) {
			fPlay = false;
			continue;
		}
		if (strcmp(argument, "--no-hardware") == 0) {
			settings().noHardwareThisRun = true;
			continue;
		}
		if (strcmp(argument, "--start") == 0 && i + 1 < argc) {
			bigtime_t time;
			if (parse_time(argv[++i], &time))
				fStartAt = time;
			continue;
		}
		if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
			printf("usage: airTime [--fullscreen] [--paused] [--no-hardware] "
				"[--start time] [file...]\n");
			continue;
		}
		BEntry entry(argument, true);
		entry_ref ref;
		if (entry.GetRef(&ref) == B_OK && entry.Exists())
			_Open(ref, NULL);
		else
			fprintf(stderr, "airTime: %s: no such file\n", argument);
	}
}


void
App::RefsReceived(BMessage* message)
{
	entry_ref ref;
	BMessenger target;
	PlayerWindow* window = NULL;
	if (message->FindMessenger("window", &target) == B_OK) {
		BLooper* looper = NULL;
		target.Target(&looper);
		window = dynamic_cast<PlayerWindow*>(looper);
	}
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		// The first film may go into the window that asked for it, if it is
		// empty; every other one gets a window of its own.
		PlayerWindow* into = NULL;
		if (i == 0 && window != NULL && window->Lock()) {
			if (window->IsEmpty())
				into = window;
			window->Unlock();
		}
		_Open(ref, into);
	}
}


void
App::ReadyToRun()
{
	if (CountWindows() == 0)
		_NewWindow();
}


void
App::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgOpenFile:
		{
			if (fOpenPanel == NULL) {
				fOpenPanel = new BFilePanel(B_OPEN_PANEL, NULL, NULL,
					B_FILE_NODE, true);
				fOpenPanel->Window()->SetTitle("airTime: Open");
			}
			BMessenger window;
			BMessage* panelMessage = new BMessage(B_REFS_RECEIVED);
			if (message->FindMessenger("window", &window) == B_OK)
				panelMessage->AddMessenger("window", window);
			fOpenPanel->SetMessage(panelMessage);
			delete panelMessage;
			fOpenPanel->Show();
			break;
		}
		case kMsgMakeDefault:
		{
			int32 changed = RegisterAsDefaultPlayer(true);
			BString text;
			text.SetToFormat("airTime now opens films and music from Tracker "
				"(%" B_PRId32 " file types).", changed);
			BAlert* alert = new BAlert("airTime", text.String(), "OK");
			alert->Go(NULL);
			break;
		}
		case kMsgMediaKey:
			_HandleMediaKey(message);
			break;
		default:
			BApplication::MessageReceived(message);
	}
}


/*!	A keyboard's media key from the system's mediakey command (see
	MediaKeys.h): the query asks whether a window plays; the key goes to the
	playing window, else the active one, else the first with a film. Next and
	previous move between chapters.
*/
void
App::_HandleMediaKey(BMessage* message)
{
	int32 key = message->GetInt32("key", -1);
	bool query = message->GetBool("query", false);

	PlayerWindow* chosen = NULL;
	bool chosenPlaying = false;
	bool chosenActive = false;
	for (int32 i = 0; BWindow* window = WindowAt(i); i++) {
		PlayerWindow* player = dynamic_cast<PlayerWindow*>(window);
		if (player == NULL || !player->Lock())
			continue;

		bool canHandle = !player->IsEmpty();
		if (key == B_MEDIA_KEY_NEXT || key == B_MEDIA_KEY_PREVIOUS)
			canHandle = canHandle && player->HasChapters();
		bool playing = player->IsPlaying();
		bool active = player->IsActive();
		player->Unlock();

		if (!canHandle)
			continue;
		if (chosen == NULL || (playing && !chosenPlaying)
			|| (playing == chosenPlaying && active && !chosenActive)) {
			chosen = player;
			chosenPlaying = playing;
			chosenActive = active;
		}
	}

	if (query) {
		reply_media_key_query(message, chosen != NULL, chosenPlaying);
		return;
	}

	bool handled = chosen != NULL;
	if (handled) {
		switch (key) {
			case B_MEDIA_KEY_PLAY_PAUSE:
				chosen->PostMessage(kMsgTogglePlay);
				break;
			case B_MEDIA_KEY_PLAY:
				chosen->PostMessage(kMsgPlay);
				break;
			case B_MEDIA_KEY_PAUSE:
			case B_MEDIA_KEY_STOP:
				chosen->PostMessage(kMsgPause);
				break;
			case B_MEDIA_KEY_NEXT:
			case B_MEDIA_KEY_PREVIOUS:
			{
				BMessage chapter(kMsgChapter);
				chapter.AddInt32("step", key == B_MEDIA_KEY_NEXT ? 1 : -1);
				chosen->PostMessage(&chapter);
				break;
			}
			default:
				handled = false;
		}
	}
	reply_media_key_handled(message, handled);
}


void
App::AboutRequested()
{
	BAboutWindow* about = new BAboutWindow("airTime", kAppSignature);
	about->AddDescription("The movie player of air/OS: films, music, "
		"subtitles and closed captions, with the decoding done by the "
		"graphics hardware where it can be.");
	const char* authors[] = {"air/OS contributors", NULL};
	about->AddAuthors(authors);
	about->AddCopyright(2026, "air/OS contributors");
	BString decoders("Hardware video decoders: ");
	decoders << hardware_decoder_summary();
	BString libraries;
	libraries.SetToFormat("FFmpeg: libavformat %s, libavcodec %s",
		AV_STRINGIFY(LIBAVFORMAT_VERSION), AV_STRINGIFY(LIBAVCODEC_VERSION));
	about->AddText(decoders.String());
	about->AddText(libraries.String());
	about->Show();
}


bool
App::QuitRequested()
{
	settings().Save();
	return BApplication::QuitRequested();
}


/*static*/ int32
App::RegisterAsDefaultPlayer(bool force)
{
	int32 changed = 0;
	for (int32 i = 0; kMediaTypes[i] != NULL; i++) {
		BMimeType type(kMediaTypes[i]);
		if (type.InitCheck() != B_OK)
			continue;
		if (!type.IsInstalled())
			type.Install();
		char preferred[B_MIME_TYPE_LENGTH];
		bool hasPreferred = type.GetPreferredApp(preferred) == B_OK;
		// Without `force`, only take over from Haiku's MediaPlayer.
		if (!force && hasPreferred
			&& strcasecmp(preferred, "application/x-vnd.Haiku-MediaPlayer") != 0
			&& strcasecmp(preferred, kAppSignature) != 0) {
			continue;
		}
		if (hasPreferred && strcasecmp(preferred, kAppSignature) == 0)
			continue;
		if (type.SetPreferredApp(kAppSignature) == B_OK)
			changed++;
	}
	return changed;
}

}	// namespace airtime
