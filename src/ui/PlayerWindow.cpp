/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "PlayerWindow.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <FilePanel.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <PropertyInfo.h>
#include <Roster.h>
#include <Screen.h>
#include <WindowScreen.h>

#include "AppInfo.h"
#include "ControlBar.h"
#include "DirectRescue.h"
#include "GoToTimeWindow.h"
#include "InspectorWindow.h"
#include "Languages.h"
#include "Messages.h"
#include "Look.h"
#include "Screens.h"
#include "Settings.h"
#include "VideoView.h"


// Stay connected to the frame buffer on a desktop drawn at a higher density,
// placed in its pixels (air/OS's app_server; older ones drop the flag).
#ifdef B_DIRECT_DEVICE_PIXELS
static const uint32 kDirectDevicePixels = B_DIRECT_DEVICE_PIXELS;
#else
static const uint32 kDirectDevicePixels = 0x00000400;
#endif


// The flag, unless View ▸ Draw at Screen Density or AIRTIME_NO_DEVICE_PIXELS
// says otherwise.
static uint32
device_pixels_flag()
{
	return airtime::settings().devicePixels
		&& getenv("AIRTIME_NO_DEVICE_PIXELS") == NULL ? kDirectDevicePixels : 0;
}


namespace airtime {

static const float kBorder = 8;
static const float kAudioInfoHeight = 70;
static const float kEmptyVideoWidth = 520;
static const float kEmptyVideoHeight = 292;
static const bigtime_t kPulseInterval = 100000;
static const bigtime_t kScreenSaverInterval = 20000000;
static const bigtime_t kHudTimeout = 2800000;

static const double kSpeeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0};
static const int kSpeedCount = sizeof(kSpeeds) / sizeof(kSpeeds[0]);
static const double kMaxScanRate = 64.0;


/*!	The window's grey surface, with a groove around the picture. */
class FrameView : public BView {
public:
	FrameView(BRect frame)
		:
		BView(frame, "frame", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS
			| B_FULL_UPDATE_ON_RESIZE),
		fGroove(0, 0, -1, -1)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void SetGroove(BRect groove)
	{
		fGroove = groove;
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		look::surface(this, updateRect);
		if (fGroove.IsValid())
			look::inset_frame(this, fGroove);
	}

private:
	BRect		fGroove;
};


static property_info sProperties[] = {
	{"Position", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Playback position in microseconds; set it "
		"with a number of microseconds or a time such as \"1:10:00\".", 0,
		{B_INT64_TYPE}},
	{"Duration", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"Length of the movie in microseconds.", 0, {B_INT64_TYPE}},
	{"Playing", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Whether the movie plays.", 0,
		{B_BOOL_TYPE}},
	{"Rate", {B_GET_PROPERTY, B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"Playback speed; negative or above 3 scans.", 0, {B_DOUBLE_TYPE}},
	{"Volume", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Volume from 0 to 1.", 0, {B_FLOAT_TYPE}},
	{"Muted", {B_GET_PROPERTY, B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"Whether the sound is muted.", 0, {B_BOOL_TYPE}},
	{"AudioTrack", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Index of the audio track playing.", 0,
		{B_INT32_TYPE}},
	{"Subtitle", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Index of the subtitle track shown, -1 none.",
		0, {B_INT32_TYPE}},
	{"Captions", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Whether closed captions are shown.", 0,
		{B_BOOL_TYPE}},
	{"FullScreen", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Whether the window fills the screen.", 0,
		{B_BOOL_TYPE}},
	{"File", {B_GET_PROPERTY, B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"The movie's path; setting it opens another.", 0, {B_STRING_TYPE}},
	{"Decoder", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"The video decoder in use.", 0, {B_STRING_TYPE}},
	{"Stats", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"Playback statistics.", 0, {B_STRING_TYPE}},
	{"Tracks", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"The audio and subtitle tracks.", 0, {B_STRING_TYPE}},
	{"SubtitleText", {B_GET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"The subtitles shown now.", 0, {B_STRING_TYPE}},
	{"Size", {B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"Resize to a scale of the movie; 0 fits the screen.", 0,
		{B_FLOAT_TYPE}},
	{"SubtitleFile", {B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"Adds a subtitle file.", 0, {B_STRING_TYPE}},
	{"DevicePixels", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Whether to draw into the frame buffer at the "
		"screen's density where the app_server allows it.", 0, {B_BOOL_TYPE}},
	{"HardwareDecoding", {B_GET_PROPERTY, B_SET_PROPERTY, 0},
		{B_DIRECT_SPECIFIER, 0}, "Whether hardware decoders may be used.", 0,
		{B_BOOL_TYPE}},
	{"Loop", {B_GET_PROPERTY, B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"Whether the movie starts over at its end.", 0, {B_BOOL_TYPE}},
	{"Frame", {B_GET_PROPERTY, B_SET_PROPERTY, 0}, {B_DIRECT_SPECIFIER, 0},
		"The window's frame on the screen.", 0, {B_RECT_TYPE}},
	{0}
};


PlayerWindow::PlayerWindow(BRect frame)
	:
	BDirectWindow(frame, "airTime", B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
		B_ASYNCHRONOUS_CONTROLS | device_pixels_flag()),
	fPlayer(NULL),
	fHasFile(false),
	fSeekOnOpen(-1),
	fPulseRunner(NULL),
	fPulseCount(0),
	fFullScreen(false),
	fHudShown(false),
	fLastActivity(0),
	fScreenSaverKicked(0),
	fCreated(system_time()),
	fDaemonKilled(false),
	fAlwaysOnTop(false),
	fIgnoreResize(0),
	fResizePending(false),
	fResizedAt(0),
	fScanHeld(false),
	fScanDirection(0),
	fScanPressedAt(0),
	fScanWasPlaying(false),
	fScanWasScanning(false),
	fScanStartRate(1.0),
	fScrubbing(false),
	fScrubWasPlaying(false),
	fSubtitlePanel(NULL),
	fWasPlayingBeforeEnd(false)
{
	fPlayer = new Player(BMessenger(this));
	fPlayer->SetHardwareDecoding(settings().hardwareDecoding
		&& !settings().noHardwareThisRun);
	fPlayer->SetVolume(settings().volume);
	fPlayer->SetMuted(settings().muted);
	fPlayer->SetLooping(settings().loop);

	_BuildMenus();

	BRect bounds = Bounds();
	bounds.top = _MenuHeight();
	fFrame = new FrameView(bounds);
	AddChild(fFrame);

	BRect frameBounds = fFrame->Bounds();
	float controlsHeight = ControlBar::PreferredHeight(false);
	fVideo = new VideoView(BRect(kBorder, kBorder,
		frameBounds.right - kBorder, frameBounds.bottom - controlsHeight),
		BMessenger(this));
	fFrame->AddChild(fVideo);
	fControls = new ControlBar(BRect(0, frameBounds.bottom - controlsHeight
		+ 1, frameBounds.right, frameBounds.bottom), false, BMessenger(this));
	fFrame->AddChild(fControls);

	fHud = new ControlBar(BRect(0, 0, 640, ControlBar::PreferredHeight(true)),
		true, BMessenger(this));
	fHud->Hide();
	fVideo->AddChild(fHud);

	fVideo->SetPlayer(fPlayer);
	fVideo->SetSubtitleScale(settings().subtitleScale);
	fControls->SetVolume(settings().volume, settings().muted);
	fHud->SetVolume(settings().volume, settings().muted);
	fControls->SetTimeDisplay(settings().timeDisplay);
	fHud->SetTimeDisplay(settings().timeDisplay);
	fPlayer->SetVideoSink(fVideo);

	_Layout();
	_ApplySizeLimits();

	BMessage pulse(kMsgPulse);
	fPulseRunner = new BMessageRunner(BMessenger(this), &pulse,
		kPulseInterval);
}


PlayerWindow::~PlayerWindow()
{
	delete fPulseRunner;
	// Stop every thread before the views they draw into go away.
	fPlayer->SetVideoSink(NULL);
	fPlayer->Close();
	delete fPlayer;
	// No more DirectConnected() calls into a window half taken down.
	Hide();
	Sync();
	// If the app_server gave up on this window's frame buffer access, the
	// stop never came, and BDirectWindow's destructor would wait for it.
	release_dead_direct_connection(this);
	delete fSubtitlePanel;
	if (fInspector.IsValid())
		fInspector.SendMessage(B_QUIT_REQUESTED);
}


// #pragma mark - opening


status_t
PlayerWindow::OpenFile(const entry_ref& ref, bool play, BString* error)
{
	BPath path(&ref);
	if (path.InitCheck() != B_OK) {
		*error = "The file could not be found.";
		return B_ENTRY_NOT_FOUND;
	}

	if (fFullScreen)
		_SetFullScreen(false);
	fPlayer->SetVideoSink(NULL);
	fPlayer->Close();
	fVideo->SetPlayer(NULL);

	fPlayer->SetPreferredLanguages(settings().audioLanguage.String(),
		settings().subtitleLanguage.String());
	// The view first, so that the first picture has somewhere to go.
	fVideo->SetPlayer(fPlayer);
	fPlayer->SetVideoSink(fVideo);
	status_t status = fPlayer->Open(path.Path(), error);
	fHasFile = status == B_OK;
	if (status != B_OK) {
		SetTitle("airTime");
		fVideo->SetInfo("", "");
		fControls->SetMedia(false, false, false);
		_Layout();
		return status;
	}
	fRef = ref;
	be_roster->AddToRecentDocuments(&ref, kAppSignature);

	SetTitle(ref.name);
	// Again, now that the player knows the picture's shape.
	fVideo->SetPlayer(fPlayer);

	bool hasVideo = fPlayer->HasVideo();
	fVideo->SetAudioOnly(!hasVideo);
	BString title = fPlayer->Metadata("title");
	if (title.Length() == 0)
		title = ref.name;
	BString detail = fPlayer->Metadata("artist");
	BString album = fPlayer->Metadata("album");
	if (album.Length() > 0) {
		if (detail.Length() > 0)
			detail << " — ";
		detail << album;
	}
	fVideo->SetInfo(title.String(), detail.String());

	fControls->SetMedia(true, fPlayer->HasMovingVideo(), fPlayer->HasAudio());
	fHud->SetMedia(true, true, fPlayer->HasAudio());
	std::vector<bigtime_t> chapters;
	for (const Chapter& chapter : fPlayer->Chapters())
		chapters.push_back(chapter.start);
	fControls->SetChapters(chapters);
	fHud->SetChapters(chapters);

	// Subtitle files next to the movie with the same name.
	std::vector<BString> sidecars = Player::FindSidecarSubtitles(path.Path());
	int selectedBefore = fPlayer->CurrentSubtitleTrack();
	for (const BString& file : sidecars) {
		BString reason;
		if (fPlayer->AddSubtitleFile(file.String(), &reason) != B_OK)
			fprintf(stderr, "airTime: %s: %s\n", file.String(), reason.String());
	}
	// Found files are offered, not forced on: keep the earlier choice unless
	// the subtitle language preference picks one of them.
	if (!sidecars.empty()) {
		int chosen = selectedBefore;
		BString preferred = settings().subtitleLanguage;
		if (chosen < 0 && preferred.Length() > 0 && preferred != "off") {
			std::vector<TrackInfo> tracks = fPlayer->SubtitleTracks();
			for (size_t i = 0; i < tracks.size(); i++) {
				if (tracks[i].external >= 0
					&& same_language(tracks[i].language.String(),
						preferred.String())) {
					chosen = (int)i;
					break;
				}
			}
		}
		fPlayer->SelectSubtitleTrack(chosen);
	}

	_Layout();
	if (!fFullScreen)
		_ResizeToVideo(1.0f);
	_ApplySizeLimits();

	if (fSeekOnOpen > 0) {
		fPlayer->Seek(fSeekOnOpen, true);
		fSeekOnOpen = -1;
	}
	if (play)
		fPlayer->Play();
	_UpdateControls();
	return B_OK;
}


// #pragma mark - menus


static BMenuItem*
item(const char* label, uint32 what, char shortcut = 0, uint32 modifiers = 0)
{
	return new BMenuItem(label, new BMessage(what), shortcut, modifiers);
}


void
PlayerWindow::_BuildMenus()
{
	fMenuBar = new BMenuBar(BRect(0, 0, Bounds().Width(), 20), "menu bar");

	fFileMenu = new BMenu("File");
	fFileMenu->AddItem(item("Open…", kMsgOpenFile, 'O'));
	fRecentMenu = new BMenu("Open Recent");
	fFileMenu->AddItem(fRecentMenu);
	fFileMenu->AddSeparatorItem();
	fFileMenu->AddItem(item("Add Subtitle File…", kMsgAddSubtitleFile, 'O',
		B_SHIFT_KEY));
	fFileMenu->AddItem(item("Show in Tracker", kMsgShowInTracker));
	fFileMenu->AddItem(item("Movie Inspector", kMsgShowInspector, 'I'));
	fFileMenu->AddSeparatorItem();
	fFileMenu->AddItem(item("Make airTime the Default Player",
		kMsgMakeDefault));
	fFileMenu->AddSeparatorItem();
	fFileMenu->AddItem(item("Close", B_QUIT_REQUESTED, 'W'));
	BMenuItem* quit = item("Quit", B_QUIT_REQUESTED, 'Q');
	quit->SetTarget(be_app);
	fFileMenu->AddItem(quit);
	fMenuBar->AddItem(fFileMenu);

	fViewMenu = new BMenu("View");
	BMessage* half = new BMessage(kMsgSize);
	half->AddFloat("scale", 0.5f);
	fViewMenu->AddItem(new BMenuItem("Half Size", half, '0'));
	BMessage* actual = new BMessage(kMsgSize);
	actual->AddFloat("scale", 1.0f);
	fViewMenu->AddItem(new BMenuItem("Actual Size", actual, '1'));
	BMessage* twice = new BMessage(kMsgSize);
	twice->AddFloat("scale", 2.0f);
	fViewMenu->AddItem(new BMenuItem("Double Size", twice, '2'));
	BMessage* fit = new BMessage(kMsgSize);
	fit->AddFloat("scale", 0.0f);
	fViewMenu->AddItem(new BMenuItem("Fit to Screen", fit, '3'));
	fViewMenu->AddSeparatorItem();
	fViewMenu->AddItem(item("Full Screen", kMsgToggleFullScreen, 'F'));
	fViewMenu->AddSeparatorItem();
	fViewMenu->AddItem(item("Keep Proportions When Resizing", kMsgToggleSnap));
	fViewMenu->AddItem(item("Always on Top", kMsgToggleOnTop));
	fViewMenu->AddItem(item("Draw at Screen Density", kMsgToggleDevicePixels));
	fViewMenu->AddSeparatorItem();
	fSubtitleSizeMenu = new BMenu("Subtitle Size");
	const struct { const char* label; float scale; } kSizes[] = {
		{"Small", 0.8f}, {"Medium", 1.0f}, {"Large", 1.25f},
		{"Extra Large", 1.6f}};
	for (const auto& size : kSizes) {
		BMessage* message = new BMessage(kMsgSubtitleSize);
		message->AddFloat("scale", size.scale);
		fSubtitleSizeMenu->AddItem(new BMenuItem(size.label, message));
	}
	fSubtitleSizeMenu->SetRadioMode(true);
	fViewMenu->AddItem(fSubtitleSizeMenu);
	fViewMenu->AddItem(item("Use Hardware Decoding", kMsgToggleHardware));
	fMenuBar->AddItem(fViewMenu);

	fPlaybackMenu = new BMenu("Playback");
	fPlayItem = item("Play", kMsgTogglePlay);
	fPlaybackMenu->AddItem(fPlayItem);
	fPlaybackMenu->AddSeparatorItem();
	fPlaybackMenu->AddItem(item("Go to Beginning", kMsgGoToStart));
	fPlaybackMenu->AddItem(item("Go to End", kMsgGoToEnd));
	fPlaybackMenu->AddItem(item("Go to Time…", kMsgGoToTime, 'G'));
	fChapterMenu = new BMenu("Chapters");
	fPlaybackMenu->AddItem(fChapterMenu);
	fPlaybackMenu->AddSeparatorItem();
	fPlaybackMenu->AddItem(item("Step Forward", kMsgStepForward, B_RIGHT_ARROW,
		B_COMMAND_KEY));
	fPlaybackMenu->AddItem(item("Step Backward", kMsgStepBackward,
		B_LEFT_ARROW, B_COMMAND_KEY));
	fPlaybackMenu->AddSeparatorItem();
	fPlaybackMenu->AddItem(item("Fast Forward", kMsgFastForward, B_RIGHT_ARROW,
		B_COMMAND_KEY | B_SHIFT_KEY));
	fPlaybackMenu->AddItem(item("Rewind", kMsgRewind, B_LEFT_ARROW,
		B_COMMAND_KEY | B_SHIFT_KEY));
	fSpeedMenu = _BuildSpeedMenu("Playback Speed");
	fPlaybackMenu->AddItem(fSpeedMenu);
	fPlaybackMenu->AddSeparatorItem();
	fPlaybackMenu->AddItem(item("Loop", kMsgToggleLoop, 'L'));
	fMenuBar->AddItem(fPlaybackMenu);

	fAudioMenu = new BMenu("Audio");
	fMenuBar->AddItem(fAudioMenu);
	fSubtitleMenu = new BMenu("Subtitles");
	fMenuBar->AddItem(fSubtitleMenu);

	BMenu* help = new BMenu("Help");
	BMenuItem* about = item("About airTime", B_ABOUT_REQUESTED);
	about->SetTarget(be_app);
	help->AddItem(about);
	help->AddItem(item("Keyboard Shortcuts", 'keys'));
	fMenuBar->AddItem(help);

	AddChild(fMenuBar);
	_BuildTrackMenus();
}


BMenu*
PlayerWindow::_BuildSpeedMenu(const char* name)
{
	BMenu* menu = new BMenu(name);
	for (double speed : kSpeeds) {
		BString label;
		if (speed == 1.0)
			label = "Normal";
		else
			label.SetToFormat("%g×", speed);
		BMessage* message = new BMessage(kMsgSetRate);
		message->AddDouble("rate", speed);
		menu->AddItem(new BMenuItem(label.String(), message));
	}
	menu->AddSeparatorItem();
	menu->AddItem(item("Faster", kMsgFaster, ']'));
	menu->AddItem(item("Slower", kMsgSlower, '['));
	return menu;
}


void
PlayerWindow::_BuildTrackMenus()
{
	// Audio
	while (BMenuItem* old = fAudioMenu->RemoveItem((int32)0))
		delete old;
	const std::vector<TrackInfo>& audio = fPlayer->AudioTracks();
	int currentAudio = fPlayer->CurrentAudioTrack();
	for (size_t i = 0; i < audio.size(); i++) {
		BMessage* message = new BMessage(kMsgSelectAudio);
		message->AddInt32("index", (int32)i);
		BMenuItem* menuItem = new BMenuItem(audio[i].Label((int)i + 1).String(),
			message);
		menuItem->SetMarked((int)i == currentAudio);
		fAudioMenu->AddItem(menuItem);
	}
	if (audio.empty()) {
		BMenuItem* none = new BMenuItem("No Sound", NULL);
		none->SetEnabled(false);
		fAudioMenu->AddItem(none);
	}
	fAudioMenu->AddSeparatorItem();
	fAudioMenu->AddItem(item("Volume Up", kMsgVolumeUp, B_UP_ARROW));
	fAudioMenu->AddItem(item("Volume Down", kMsgVolumeDown, B_DOWN_ARROW));
	BMenuItem* mute = item("Mute", kMsgToggleMute, 'M', B_SHIFT_KEY);
	mute->SetMarked(settings().muted);
	fAudioMenu->AddItem(mute);

	// Subtitles
	while (BMenuItem* old = fSubtitleMenu->RemoveItem((int32)0))
		delete old;
	std::vector<TrackInfo> subtitles = fPlayer->SubtitleTracks();
	int currentSubtitle = fPlayer->CurrentSubtitleTrack();
	BMessage* off = new BMessage(kMsgSelectSubtitle);
	off->AddInt32("index", -1);
	BMenuItem* offItem = new BMenuItem("Off", off);
	offItem->SetMarked(currentSubtitle < 0);
	fSubtitleMenu->AddItem(offItem);
	for (size_t i = 0; i < subtitles.size(); i++) {
		BMessage* message = new BMessage(kMsgSelectSubtitle);
		message->AddInt32("index", (int32)i);
		BMenuItem* menuItem = new BMenuItem(
			subtitles[i].Label((int)i + 1).String(), message);
		menuItem->SetMarked((int)i == currentSubtitle);
		fSubtitleMenu->AddItem(menuItem);
	}
	fSubtitleMenu->AddSeparatorItem();
	BMenuItem* captions = item("Closed Captions", kMsgToggleCaptions, 'C',
		B_SHIFT_KEY);
	captions->SetEnabled(fPlayer->HasCaptions());
	captions->SetMarked(fPlayer->CaptionsEnabled());
	fSubtitleMenu->AddItem(captions);
	fSubtitleMenu->AddSeparatorItem();
	fSubtitleMenu->AddItem(item("Add Subtitle File…", kMsgAddSubtitleFile));
}


void
PlayerWindow::_BuildRecentMenu()
{
	while (BMenuItem* old = fRecentMenu->RemoveItem((int32)0))
		delete old;
	BMessage refs;
	be_roster->GetRecentDocuments(&refs, 12, NULL, kAppSignature);
	entry_ref ref;
	for (int32 i = 0; refs.FindRef("refs", i, &ref) == B_OK; i++) {
		BEntry entry(&ref);
		if (!entry.Exists())
			continue;
		BMessage* message = new BMessage(B_REFS_RECEIVED);
		message->AddRef("refs", &ref);
		BMenuItem* menuItem = new BMenuItem(ref.name, message);
		menuItem->SetTarget(be_app);
		fRecentMenu->AddItem(menuItem);
	}
	fRecentMenu->SetEnabled(fRecentMenu->CountItems() > 0);
}


void
PlayerWindow::_BuildChapterMenu()
{
	while (BMenuItem* old = fChapterMenu->RemoveItem((int32)0))
		delete old;
	const std::vector<Chapter>& chapters = fPlayer->Chapters();
	bigtime_t position = fPlayer->Position();
	for (size_t i = 0; i < chapters.size(); i++) {
		BString label;
		label.SetToFormat("%s\t%s", chapters[i].title.String(),
			format_time(chapters[i].start).String());
		BMessage* message = new BMessage(kMsgChapter);
		message->AddInt32("index", (int32)i);
		BMenuItem* menuItem = new BMenuItem(chapters[i].title.String(),
			message);
		menuItem->SetMarked(position >= chapters[i].start
			&& (i + 1 == chapters.size() || position < chapters[i + 1].start));
		fChapterMenu->AddItem(menuItem);
	}
	fChapterMenu->SetEnabled(!chapters.empty());
}


void
PlayerWindow::_UpdateMenus()
{
	bool open = fHasFile;
	bool video = open && fPlayer->HasMovingVideo();
	bool playing = open && fPlayer->IsPlaying() && !fPlayer->IsScanning();
	fPlayItem->SetLabel(playing ? "Pause" : "Play");

	for (int32 i = 0; BMenuItem* menuItem = fViewMenu->ItemAt(i); i++) {
		BMessage* message = menuItem->Message();
		if (message == NULL)
			continue;
		switch (message->what) {
			case kMsgSize:
			case kMsgToggleFullScreen:
				menuItem->SetEnabled(video);
				if (message->what == kMsgToggleFullScreen)
					menuItem->SetMarked(fFullScreen);
				break;
			case kMsgToggleSnap:
				menuItem->SetMarked(settings().snapToAspect);
				break;
			case kMsgToggleDevicePixels:
				menuItem->SetMarked(settings().devicePixels);
				break;
			case kMsgToggleOnTop:
				menuItem->SetMarked(fAlwaysOnTop);
				break;
			case kMsgToggleHardware:
				menuItem->SetMarked(settings().hardwareDecoding
					&& !settings().noHardwareThisRun);
				break;
		}
	}
	for (int32 i = 0; BMenuItem* menuItem = fSubtitleSizeMenu->ItemAt(i); i++) {
		float scale;
		if (menuItem->Message()->FindFloat("scale", &scale) == B_OK)
			menuItem->SetMarked(fabsf(scale - settings().subtitleScale) < 0.01f);
	}
	for (int32 i = 0; BMenuItem* menuItem = fPlaybackMenu->ItemAt(i); i++) {
		BMessage* message = menuItem->Message();
		if (message != NULL) {
			menuItem->SetEnabled(open);
			if (message->what == kMsgToggleLoop)
				menuItem->SetMarked(fPlayer->IsLooping());
			if (message->what == kMsgStepForward
				|| message->what == kMsgStepBackward) {
				menuItem->SetEnabled(video);
			}
		}
	}
	fSpeedMenu->SetEnabled(open);
	double rate = fPlayer->Rate();
	for (int32 i = 0; BMenuItem* menuItem = fSpeedMenu->ItemAt(i); i++) {
		double speed;
		if (menuItem->Message() != NULL
			&& menuItem->Message()->FindDouble("rate", &speed) == B_OK) {
			menuItem->SetMarked(!fPlayer->IsScanning()
				&& fabs(speed - rate) < 0.001);
		}
	}
	for (int32 i = 0; BMenuItem* menuItem = fFileMenu->ItemAt(i); i++) {
		BMessage* message = menuItem->Message();
		if (message != NULL && (message->what == kMsgAddSubtitleFile
				|| message->what == kMsgShowInTracker
				|| message->what == kMsgShowInspector)) {
			menuItem->SetEnabled(open);
		}
	}
	_BuildTrackMenus();
	_BuildChapterMenu();
	_BuildRecentMenu();
}


void
PlayerWindow::MenusBeginning()
{
	_UpdateMenus();
}


void
PlayerWindow::_ShowContextMenu(BPoint where)
{
	BPopUpMenu* menu = new BPopUpMenu("context", false, false);
	bool playing = fPlayer->IsPlaying() && !fPlayer->IsScanning();
	menu->AddItem(item(playing ? "Pause" : "Play", kMsgTogglePlay));
	menu->AddItem(item(fFullScreen ? "Exit Full Screen" : "Full Screen",
		kMsgToggleFullScreen));
	menu->AddSeparatorItem();

	BMenu* audio = new BMenu("Audio");
	int currentAudio = fPlayer->CurrentAudioTrack();
	const std::vector<TrackInfo>& audioTracks = fPlayer->AudioTracks();
	for (size_t i = 0; i < audioTracks.size(); i++) {
		BMessage* message = new BMessage(kMsgSelectAudio);
		message->AddInt32("index", (int32)i);
		BMenuItem* menuItem = new BMenuItem(
			audioTracks[i].Label((int)i + 1).String(), message);
		menuItem->SetMarked((int)i == currentAudio);
		audio->AddItem(menuItem);
	}
	audio->SetEnabled(audioTracks.size() > 1);
	menu->AddItem(audio);

	BMenu* subtitles = new BMenu("Subtitles");
	int currentSubtitle = fPlayer->CurrentSubtitleTrack();
	BMessage* off = new BMessage(kMsgSelectSubtitle);
	off->AddInt32("index", -1);
	BMenuItem* offItem = new BMenuItem("Off", off);
	offItem->SetMarked(currentSubtitle < 0);
	subtitles->AddItem(offItem);
	std::vector<TrackInfo> subtitleTracks = fPlayer->SubtitleTracks();
	for (size_t i = 0; i < subtitleTracks.size(); i++) {
		BMessage* message = new BMessage(kMsgSelectSubtitle);
		message->AddInt32("index", (int32)i);
		BMenuItem* menuItem = new BMenuItem(
			subtitleTracks[i].Label((int)i + 1).String(), message);
		menuItem->SetMarked((int)i == currentSubtitle);
		subtitles->AddItem(menuItem);
	}
	if (fPlayer->HasCaptions()) {
		subtitles->AddSeparatorItem();
		BMenuItem* captions = item("Closed Captions", kMsgToggleCaptions);
		captions->SetMarked(fPlayer->CaptionsEnabled());
		subtitles->AddItem(captions);
	}
	menu->AddItem(subtitles);

	BMenu* speed = _BuildSpeedMenu("Speed");
	double rate = fPlayer->Rate();
	for (int32 i = 0; BMenuItem* menuItem = speed->ItemAt(i); i++) {
		double value;
		if (menuItem->Message() != NULL
			&& menuItem->Message()->FindDouble("rate", &value) == B_OK) {
			menuItem->SetMarked(fabs(value - rate) < 0.001);
		}
	}
	menu->AddItem(speed);
	menu->AddSeparatorItem();
	menu->AddItem(item("Movie Inspector", kMsgShowInspector));

	menu->SetTargetForItems(this);
	for (int32 i = 0; BMenuItem* menuItem = menu->ItemAt(i); i++) {
		if (menuItem->Submenu() != NULL)
			menuItem->Submenu()->SetTargetForItems(this);
	}
	menu->SetAsyncAutoDestruct(true);
	menu->Go(where, true, true, true);
}


// #pragma mark - layout and size


float
PlayerWindow::_MenuHeight() const
{
	if (fMenuBar == NULL || fMenuBar->IsHidden())
		return 0;
	return fMenuBar->Frame().Height() + 1;
}


float
PlayerWindow::_ControlsHeight() const
{
	return ControlBar::PreferredHeight(false);
}


void
PlayerWindow::_Layout()
{
	BRect bounds = Bounds();
	float menuHeight = _MenuHeight();
	fFrame->MoveTo(0, menuHeight);
	fFrame->ResizeTo(bounds.Width(), bounds.Height() - menuHeight);
	BRect surface = fFrame->Bounds();

	if (fFullScreen) {
		fVideo->MoveTo(0, 0);
		fVideo->ResizeTo(surface.Width(), surface.Height());
		fFrame->SetGroove(BRect(0, 0, -1, -1));
		BRect video = fVideo->Bounds();
		float hudWidth = std::min(720.0f, std::max(480.0f,
			video.Width() * 0.55f));
		float hudHeight = ControlBar::PreferredHeight(true);
		fHud->ResizeTo(hudWidth, hudHeight);
		fHud->MoveTo(floorf((video.Width() - hudWidth) / 2),
			video.bottom - hudHeight - 36);
		fVideo->UpdateWindowOrigin();
		return;
	}

	float controlsHeight = _ControlsHeight();
	fControls->MoveTo(0, surface.bottom - controlsHeight + 1);
	fControls->ResizeTo(surface.Width(), controlsHeight - 1);

	bool audioOnly = fHasFile && !fPlayer->HasVideo();
	BRect video;
	if (audioOnly) {
		video.Set(0, 0, surface.right, surface.bottom - controlsHeight);
		fFrame->SetGroove(BRect(0, 0, -1, -1));
	} else {
		video.Set(kBorder, kBorder, surface.right - kBorder,
			surface.bottom - controlsHeight);
		fFrame->SetGroove(video);
	}
	fVideo->MoveTo(video.LeftTop());
	fVideo->ResizeTo(video.Width(), video.Height());
	fVideo->UpdateWindowOrigin();
}


void
PlayerWindow::_ApplySizeLimits()
{
	float minWidth = ControlBar::MinimumWidth(false) + 2 * kBorder;
	float chrome = _MenuHeight() + _ControlsHeight();
	float minHeight;
	if (fHasFile && !fPlayer->HasVideo())
		minHeight = chrome + kAudioInfoHeight;
	else
		minHeight = chrome + kBorder + 90;
	if (fFullScreen)
		SetSizeLimits(0, 100000, 0, 100000);
	else
		SetSizeLimits(minWidth - 1, 100000, minHeight - 1, 100000);
}


BRect
PlayerWindow::_ScreenFrame() const
{
	return monitor_frame(fFullScreen ? fSavedFrame : Frame());
}


void
PlayerWindow::_ResizeToVideo(float scale)
{
	if (fFullScreen)
		return;
	BRect screen = _ScreenFrame();
	float chrome = _MenuHeight() + _ControlsHeight();
	float width;
	float height;

	if (!fHasFile) {
		width = kEmptyVideoWidth + 2 * kBorder;
		height = chrome + kBorder + kEmptyVideoHeight;
	} else if (!fPlayer->HasVideo()) {
		width = std::max(420.0f, Bounds().Width() + 1);
		height = chrome + kAudioInfoHeight;
	} else {
		double aspect = fPlayer->DisplayAspect();
		float videoHeight = fPlayer->VideoHeight();
		VideoFramePtr cover = fPlayer->CoverArt();
		if (cover.get() != NULL && !fPlayer->HasMovingVideo()) {
			// Album art: a sensible square, not its full resolution.
			videoHeight = std::min(360, cover->frame->height);
		}
		if (videoHeight <= 0)
			videoHeight = 360;
		float videoWidth = roundf(videoHeight * aspect);

		float maxWidth = screen.Width() * 0.92f - 2 * kBorder;
		float maxHeight = screen.Height() * 0.88f - chrome - kBorder - 30;
		if (scale <= 0) {
			scale = std::min(maxWidth / videoWidth, maxHeight / videoHeight);
		} else {
			float fit = std::min(maxWidth / (videoWidth * scale),
				maxHeight / (videoHeight * scale));
			if (fit < 1)
				scale *= fit;
		}
		videoWidth = roundf(videoWidth * scale);
		videoHeight = roundf(videoHeight * scale);
		float minVideoWidth = ControlBar::MinimumWidth(false);
		if (videoWidth < minVideoWidth) {
			videoHeight = roundf(videoHeight * minVideoWidth / videoWidth);
			videoWidth = minVideoWidth;
		}
		width = videoWidth + 2 * kBorder;
		height = chrome + kBorder + videoHeight;
	}

	fIgnoreResize++;
	ResizeTo(width - 1, height - 1);
	fIgnoreResize--;
	fResizePending = false;

	// Keep the window on the screen.
	BRect frame = Frame();
	float x = frame.left;
	float y = frame.top;
	if (frame.right > screen.right - 4)
		x = std::max(screen.left + 4, screen.right - 4 - frame.Width());
	if (frame.bottom > screen.bottom - 4)
		y = std::max(screen.top + 28, screen.bottom - 4 - frame.Height());
	if (x != frame.left || y != frame.top)
		MoveTo(x, y);
	_Layout();
}


void
PlayerWindow::_SnapToAspect()
{
	if (fFullScreen || !fHasFile || !fPlayer->HasMovingVideo()
		|| !settings().snapToAspect) {
		return;
	}
	BRect bounds = Bounds();
	float videoWidth = bounds.Width() + 1 - 2 * kBorder;
	double aspect = fPlayer->DisplayAspect();
	if (aspect <= 0 || videoWidth <= 0)
		return;
	float videoHeight = roundf(videoWidth / aspect);
	float height = _MenuHeight() + _ControlsHeight() + kBorder + videoHeight;
	if (fabsf(height - (bounds.Height() + 1)) < 2)
		return;
	fIgnoreResize++;
	ResizeTo(bounds.Width(), height - 1);
	fIgnoreResize--;
	_Layout();
}


void
PlayerWindow::FrameResized(float width, float height)
{
	BWindow::FrameResized(width, height);
	_Layout();
	if (fIgnoreResize == 0 && !fFullScreen) {
		fResizePending = true;
		fResizedAt = system_time();
	}
}


void
PlayerWindow::Zoom(BPoint origin, float width, float height)
{
	// The zoom button toggles between actual size and the screen.
	BRect screen = _ScreenFrame();
	if (Frame().Width() > screen.Width() * 0.8f)
		_ResizeToVideo(1.0f);
	else
		_ResizeToVideo(0);
}


void
PlayerWindow::SetFullScreen(bool fullScreen)
{
	_SetFullScreen(fullScreen);
}


void
PlayerWindow::_SetFullScreen(bool fullScreen)
{
	if (fullScreen == fFullScreen)
		return;
	if (fullScreen && (!fHasFile || !fPlayer->HasMovingVideo()))
		return;
	fFullScreen = fullScreen;
	fIgnoreResize++;

	if (fullScreen) {
		fSavedFrame = Frame();
		BRect screen = _ScreenFrame();
		fMenuBar->Hide();
		fControls->Hide();
		_ApplySizeLimits();
		Hide();
		MoveTo(screen.LeftTop());
		ResizeTo(screen.Width(), screen.Height());
		Show();
		SetFeel(B_FLOATING_ALL_WINDOW_FEEL);
		Activate();
		fLastActivity = system_time();
		_ShowHud(true);
	} else {
		_ShowHud(false);
		SetFeel(fAlwaysOnTop ? B_FLOATING_ALL_WINDOW_FEEL
			: B_NORMAL_WINDOW_FEEL);
		Hide();
		fMenuBar->Show();
		fControls->Show();
		MoveTo(fSavedFrame.LeftTop());
		ResizeTo(fSavedFrame.Width(), fSavedFrame.Height());
		_ApplySizeLimits();
		Show();
	}
	fVideo->SetFullScreen(fullScreen);
	fControls->SetFullScreen(fullScreen);
	fHud->SetFullScreen(fullScreen);
	_Layout();
	fVideo->MakeFocus(true);
	fIgnoreResize--;
	fResizePending = false;
}


void
PlayerWindow::_ShowHud(bool show)
{
	if (show == fHudShown)
		return;
	fHudShown = show;
	if (show) {
		_Layout();
		fHud->Show();
		fVideo->SetCoveredBottom(fHud->Frame().top);
		be_app->ShowCursor();
	} else {
		fHud->Hide();
		fVideo->SetCoveredBottom(-1);
		if (fFullScreen)
			be_app->ObscureCursor();
	}
}


// #pragma mark - updates


void
PlayerWindow::_UpdateControls()
{
	if (!fHasFile) {
		fControls->SetMedia(false, false, false);
		return;
	}
	bigtime_t position = fPlayer->Position();
	bigtime_t duration = fPlayer->Duration();
	bool playing = fPlayer->IsPlaying();
	bool scanning = fPlayer->IsScanning();
	double rate = fPlayer->Rate();
	if (!fScrubbing) {
		fControls->SetPosition(position, duration);
		fHud->SetPosition(position, duration);
	}
	fControls->SetPlaying(playing);
	fHud->SetPlaying(playing);
	fControls->SetRate(rate, scanning);
	fHud->SetRate(rate, scanning);
}


void
PlayerWindow::_ApplyDevicePixels()
{
	// Called with the window locked. An app_server that does not know the
	// flag refuses it; the window then stays as it was.
	uint32 flags = (Flags() & ~kDirectDevicePixels) | device_pixels_flag();
	if (flags != Flags())
		SetFlags(flags);
}


void
PlayerWindow::_Pulse()
{
	fPulseCount++;
	if (fHasFile)
		_UpdateControls();

	bigtime_t now = system_time();

	// AIRTIME_KILL_DIRECT_DAEMON=<seconds>: end the frame buffer access the
	// way the app_server's timeout does, to try what follows.
	static const char* sKillDaemon = getenv("AIRTIME_KILL_DIRECT_DAEMON");
	if (sKillDaemon != NULL && !fDaemonKilled
		&& now - fCreated > (bigtime_t)(atof(sKillDaemon) * 1000000)) {
		fDaemonKilled = true;
		fprintf(stderr, "airTime: ending the direct daemon for a test\n");
		kill_direct_daemon(this);
	}

	// No screen saver while a film plays where it can be seen: putting the
	// pointer where it already is counts as using the computer, as Haiku's
	// MediaPlayer does it.
	if (fHasFile && fPlayer->IsPlaying() && fPlayer->HasMovingVideo()
		&& !IsHidden() && !IsMinimized()
		&& now - fScreenSaverKicked > kScreenSaverInterval) {
		fScreenSaverKicked = now;
		BPoint where;
		uint32 buttons;
		fVideo->GetMouse(&where, &buttons, false);
		if (buttons == 0) {
			fVideo->ConvertToScreen(&where);
			set_mouse_position((int32)where.x, (int32)where.y);
		}
	}

	// Fast forward and rewind held down speed up the longer they are held.
	if (fScanHeld) {
		bigtime_t held = now - fScanPressedAt;
		double rate = 2.0;
		if (held > 1200000)
			rate = 4.0;
		if (held > 2400000)
			rate = 8.0;
		if (held > 3600000)
			rate = 16.0;
		if (held > 5000000)
			rate = 32.0;
		rate *= fScanDirection;
		if (fabs(fPlayer->Rate() - rate) > 0.001)
			fPlayer->SetRate(rate);
	}

	// The full screen controller goes away when the mouse rests.
	if (fFullScreen && fHudShown && !fHud->IsTracking()
		&& now - fLastActivity > kHudTimeout) {
		BPoint where;
		uint32 buttons;
		fHud->GetMouse(&where, &buttons, false);
		if (!fHud->Bounds().Contains(where))
			_ShowHud(false);
	}

	// QuickTime kept the window in the film's proportions; do it once the
	// user has let go of the resize corner.
	if (fResizePending && now - fResizedAt > 300000) {
		BPoint where;
		uint32 buttons;
		fFrame->GetMouse(&where, &buttons, false);
		if (buttons == 0) {
			fResizePending = false;
			_SnapToAspect();
		}
	}

	if (fInspector.IsValid() && fPulseCount % 5 == 0)
		_UpdateInspector();
}


void
PlayerWindow::_UpdateInspector()
{
	BMessage update(kMsgInspectorUpdate);
	auto add = [&update](const char* label, const BString& value) {
		update.AddString("label", label);
		update.AddString("value", value);
	};
	if (!fHasFile) {
		add("Source", "No movie");
		fInspector.SendMessage(&update);
		return;
	}
	update.AddString("title", fRef.name);
	BPath path(&fRef);
	add("Source", path.Path());
	add("Format", fPlayer->FormatName());

	BString size;
	int64 bytes = fPlayer->FileSize();
	if (bytes > 1024LL * 1024 * 1024)
		size.SetToFormat("%.2f GB", bytes / (1024.0 * 1024 * 1024));
	else if (bytes > 0)
		size.SetToFormat("%.1f MB", bytes / (1024.0 * 1024));
	add("Data Size", size);
	BString rate;
	if (fPlayer->BitRate() > 0)
		rate.SetToFormat("%.2f Mbit/s", fPlayer->BitRate() / 1e6);
	add("Data Rate", rate);
	add("Duration", format_time(fPlayer->Duration(), true));
	add("Current Time", format_time(fPlayer->Position(), true));

	if (fPlayer->HasMovingVideo()) {
		const TrackInfo& video = fPlayer->VideoTrack();
		BString format;
		format.SetToFormat("%s, %d × %d", video.codec.String(), video.width,
			video.height);
		add("Video", format);
		BString frameRate;
		PlayerStats stats = fPlayer->Stats();
		frameRate.SetToFormat("%.3f fps (playing at %.1f)", video.frameRate,
			stats.displayRate);
		add("Movie FPS", frameRate);
		BString decoder = fPlayer->VideoDecoderName();
		decoder << (fPlayer->VideoDecoderIsHardware() ? " — hardware"
			: " — processor");
		add("Decoder", decoder);
		BString note = fPlayer->HardwareDecoderNote();
		if (note.Length() > 0 && !fPlayer->VideoDecoderIsHardware())
			add("Not in Hardware", note);
		BRect shown = fVideo->VideoFrame();
		BString current;
		current.SetToFormat("%d × %d", shown.IntegerWidth() + 1,
			shown.IntegerHeight() + 1);
		add("Current Size", current);
		BString frames;
		frames.SetToFormat("%" B_PRId64 " shown, %" B_PRId64 " dropped",
			stats.framesShown, stats.framesDropped);
		add("Frames", frames);
	}
	int audioIndex = fPlayer->CurrentAudioTrack();
	if (audioIndex >= 0) {
		const TrackInfo& audio = fPlayer->AudioTracks()[audioIndex];
		add("Audio", audio.Label(audioIndex + 1));
		BString detail;
		detail.SetToFormat("%d Hz, %s", audio.sampleRate,
			fPlayer->AudioDecoderName().String());
		add("Audio Decoder", detail);
	} else if (!fPlayer->HasAudio())
		add("Audio", fPlayer->AudioDecoderName());
	int subtitle = fPlayer->CurrentSubtitleTrack();
	if (subtitle >= 0) {
		std::vector<TrackInfo> tracks = fPlayer->SubtitleTracks();
		add("Subtitles", tracks[subtitle].Label(subtitle + 1));
	} else
		add("Subtitles", "Off");
	if (fPlayer->HasCaptions())
		add("Captions", fPlayer->CaptionsEnabled() ? "On" : "Available");
	BString speed;
	if (fPlayer->IsScanning())
		speed.SetToFormat("scanning at %g×", fPlayer->Rate());
	else
		speed.SetToFormat("%g×", fPlayer->Rate());
	add("Playback Speed", speed);
	fInspector.SendMessage(&update);
}


void
PlayerWindow::_ShowMessage(const char* format, ...)
{
	char text[256];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	fVideo->ShowMessage(text);
}


// #pragma mark - actions


void
PlayerWindow::_HandleScanPress(int direction)
{
	if (!fHasFile)
		return;
	fScanHeld = true;
	fScanDirection = direction;
	fScanPressedAt = system_time();
	fScanWasScanning = fPlayer->IsScanning() || fabs(fPlayer->Rate() - 1) > 0.01;
	fScanStartRate = fPlayer->Rate();
	if (!fScanWasScanning)
		fScanWasPlaying = fPlayer->IsPlaying();
	fPlayer->SetRate(2.0 * direction);
	if (!fPlayer->IsPlaying())
		fPlayer->Play();
}


void
PlayerWindow::_HandleScanRelease(int direction, bigtime_t held)
{
	if (!fScanHeld)
		return;
	fScanHeld = false;
	if (held < 300000) {
		// A click: keep going, faster with each click in the same direction.
		double rate = 2.0 * direction;
		if (fScanWasScanning && fScanStartRate * direction > 0) {
			rate = fScanStartRate * 2;
			if (fabs(rate) > kMaxScanRate)
				rate = 2.0 * direction;
			if (fabs(rate) < 2)
				rate = 2.0 * direction;
		}
		fPlayer->SetRate(rate);
		if (!fPlayer->IsPlaying())
			fPlayer->Play();
		_ShowMessage(direction > 0 ? "Fast forward %g×" : "Rewind %g×",
			fabs(rate));
		return;
	}
	// Held down: back to how it was.
	fPlayer->SetRate(1.0);
	if (fScanWasPlaying)
		fPlayer->Play();
	else
		fPlayer->Pause();
}


void
PlayerWindow::_HandleJKL(int key)
{
	if (!fHasFile)
		return;
	double rate = fPlayer->Rate();
	bool playing = fPlayer->IsPlaying();
	if (key == 0) {
		fPlayer->Pause();
		fPlayer->SetRate(1.0);
		fPlayer->Pause();
		return;
	}
	double next;
	if (key > 0) {
		if (!playing || rate < 0)
			next = 1.0;
		else
			next = std::min(kMaxScanRate, rate < 1 ? 1.0 : rate * 2);
	} else {
		if (!playing || rate > 0)
			next = -1.0;
		else
			next = std::max(-kMaxScanRate, rate * 2);
	}
	fPlayer->SetRate(next);
	if (!fPlayer->IsPlaying())
		fPlayer->Play();
	if (next != 1.0)
		_ShowMessage("%s%g×", next < 0 ? "−" : "", fabs(next));
}


void
PlayerWindow::_StepSpeed(int direction)
{
	double rate = fPlayer->Rate();
	if (fPlayer->IsScanning())
		rate = 1.0;
	int index = 0;
	for (int i = 0; i < kSpeedCount; i++) {
		if (kSpeeds[i] <= rate + 0.001)
			index = i;
	}
	index += direction;
	if (index < 0)
		index = 0;
	if (index >= kSpeedCount)
		index = kSpeedCount - 1;
	fPlayer->SetRate(kSpeeds[index]);
	_ShowMessage("Speed %g×", kSpeeds[index]);
}


void
PlayerWindow::_SetVolume(float volume)
{
	volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
	settings().volume = volume;
	settings().muted = false;
	fPlayer->SetVolume(volume);
	fPlayer->SetMuted(false);
	fControls->SetVolume(volume, false);
	fHud->SetVolume(volume, false);
}


void
PlayerWindow::_HandleClick(int32 clicks)
{
	if (!fHasFile)
		return;
	if (clicks >= 2) {
		// The first click of the two toggled playing; undo that.
		fPlayer->TogglePlaying();
		_SetFullScreen(!fFullScreen);
		return;
	}
	if (fFullScreen && !fHudShown) {
		fLastActivity = system_time();
		_ShowHud(true);
	}
	fPlayer->TogglePlaying();
	_UpdateControls();
}


static bool
is_subtitle_file(const char* name)
{
	BString extension(name);
	int32 dot = extension.FindLast('.');
	if (dot < 0)
		return false;
	extension.Remove(0, dot + 1);
	extension.ToLower();
	return extension == "srt" || extension == "ass" || extension == "ssa"
		|| extension == "vtt" || extension == "idx" || extension == "smi"
		|| extension == "sami";
}


void
PlayerWindow::_HandleDrop(BMessage* message)
{
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		if (is_subtitle_file(ref.name)) {
			if (fHasFile)
				_AddSubtitleFile(ref);
			continue;
		}
		// A film dropped on the window replaces this one.
		BString error;
		if (OpenFile(ref, settings().autoPlay, &error) != B_OK) {
			BString text;
			text.SetToFormat("“%s” cannot be played.\n\n%s", ref.name,
				error.String());
			BAlert* alert = new BAlert("airTime", text.String(), "OK", NULL,
				NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
			alert->Go(NULL);
		}
		break;
	}
}


void
PlayerWindow::_AddSubtitleFile(const entry_ref& ref)
{
	BPath path(&ref);
	BString error;
	if (fPlayer->AddSubtitleFile(path.Path(), &error) != B_OK) {
		BString text;
		text.SetToFormat("The subtitles in “%s” cannot be used: %s.",
			ref.name, error.String());
		BAlert* alert = new BAlert("airTime", text.String(), "OK", NULL, NULL,
			B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->Go(NULL);
		return;
	}
	_ShowMessage("Subtitles: %s", ref.name);
	fVideo->Refresh();
}


void
PlayerWindow::_CycleSubtitles()
{
	std::vector<TrackInfo> tracks = fPlayer->SubtitleTracks();
	int current = fPlayer->CurrentSubtitleTrack();
	int next = current + 1;
	if (next >= (int)tracks.size())
		next = -1;
	fPlayer->SelectSubtitleTrack(next);
	if (next < 0)
		_ShowMessage("Subtitles off");
	else
		_ShowMessage("Subtitles: %s", tracks[next].Label(next + 1).String());
	fVideo->Refresh();
}


void
PlayerWindow::_CycleAudio()
{
	const std::vector<TrackInfo>& tracks = fPlayer->AudioTracks();
	if (tracks.size() < 2)
		return;
	int next = (fPlayer->CurrentAudioTrack() + 1) % (int)tracks.size();
	fPlayer->SelectAudioTrack(next);
	_ShowMessage("Audio: %s", tracks[next].Label(next + 1).String());
}


void
PlayerWindow::_GoToChapter(int step)
{
	const std::vector<Chapter>& chapters = fPlayer->Chapters();
	if (chapters.empty())
		return;
	bigtime_t position = fPlayer->Position();
	int current = 0;
	for (size_t i = 0; i < chapters.size(); i++) {
		if (chapters[i].start <= position + 500000)
			current = (int)i;
	}
	int target = current + step;
	// Going back from a few seconds into a chapter restarts it.
	if (step < 0 && position - chapters[current].start > 3000000)
		target = current;
	if (target < 0)
		target = 0;
	if (target >= (int)chapters.size())
		return;
	fPlayer->Seek(chapters[target].start, true);
	_ShowMessage("%s", chapters[target].title.String());
}


// #pragma mark - messages


bool
PlayerWindow::QuitRequested()
{
	settings().Save();
	if (fInspector.IsValid())
		fInspector.SendMessage(B_QUIT_REQUESTED);
	if (be_app->CountWindows() <= 1 + (fInspector.IsValid() ? 1 : 0))
		be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


void
PlayerWindow::DirectConnected(direct_buffer_info* info)
{
	fVideo->DirectConnected(info);
	BDirectWindow::DirectConnected(info);
}


void
PlayerWindow::WindowActivated(bool active)
{
	BWindow::WindowActivated(active);
	if (active)
		fVideo->MakeFocus(true);
}


void
PlayerWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPulse:
			_Pulse();
			break;

		case kMsgPlayerStateChanged:
		case kMsgPlayerEnded:
			_UpdateControls();
			if (message->what == kMsgPlayerEnded && fFullScreen)
				_ShowHud(true);
			break;
		case kMsgPlayerTracksChanged:
			fVideo->Refresh();
			break;
		case kMsgPlayerDecoderChanged:
			if (fInspector.IsValid())
				_UpdateInspector();
			break;
		case kMsgPlayerError:
		{
			BString error;
			message->FindString("error", &error);
			BAlert* alert = new BAlert("airTime", error.String(), "OK", NULL,
				NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->Go(NULL);
			break;
		}

		case kMsgTogglePlay:
			if (fHasFile) {
				if (fPlayer->IsScanning()) {
					fPlayer->SetRate(1.0);
					fPlayer->Play();
				} else
					fPlayer->TogglePlaying();
				_UpdateControls();
			}
			break;
		case kMsgPlay:
			fPlayer->Play();
			break;
		case kMsgPause:
			fPlayer->Pause();
			break;
		case kMsgGoToStart:
			if (fHasFile)
				fPlayer->Seek(0, true);
			break;
		case kMsgGoToEnd:
			if (fHasFile) {
				fPlayer->Pause();
				fPlayer->Seek(fPlayer->Duration(), true);
			}
			break;
		case kMsgScanPress:
			_HandleScanPress(message->GetInt32("direction", 1));
			break;
		case kMsgScanRelease:
			_HandleScanRelease(message->GetInt32("direction", 1),
				message->GetInt64("held", 0));
			break;
		case kMsgFastForward:
		case kMsgRewind:
		{
			int direction = message->what == kMsgFastForward ? 1 : -1;
			fScanHeld = true;
			fScanDirection = direction;
			fScanPressedAt = system_time();
			fScanWasScanning = fPlayer->IsScanning()
				|| fabs(fPlayer->Rate() - 1) > 0.01;
			fScanStartRate = fPlayer->Rate();
			if (!fScanWasScanning)
				fScanWasPlaying = fPlayer->IsPlaying();
			_HandleScanRelease(direction, 0);
			break;
		}
		case kMsgSeekTo:
		{
			if (!fHasFile)
				break;
			bigtime_t time = message->GetInt64("time", 0);
			bool final = message->GetBool("final", true);
			if (!final) {
				if (!fScrubbing) {
					fScrubbing = true;
					fScrubWasPlaying = fPlayer->IsPlaying()
						&& !fPlayer->IsScanning();
					if (fScrubWasPlaying)
						fPlayer->Pause();
				}
				fPlayer->Seek(time, false);
			} else {
				fPlayer->Seek(time, true);
				if (fScrubbing && fScrubWasPlaying)
					fPlayer->Play();
				fScrubbing = false;
			}
			break;
		}
		case kMsgSeekBy:
			if (fHasFile) {
				bigtime_t target = fPlayer->Position()
					+ message->GetInt64("delta", 0);
				fPlayer->Seek(std::max((bigtime_t)0, target), true);
				_ShowMessage("%s", format_time(std::max((bigtime_t)0,
					target)).String());
			}
			break;
		case kMsgStepForward:
			fPlayer->StepFrame(1);
			break;
		case kMsgStepBackward:
			fPlayer->StepFrame(-1);
			break;
		case kMsgGoToTime:
			if (fHasFile) {
				GoToTimeWindow* window = new GoToTimeWindow(this,
					fPlayer->Position(), fPlayer->Duration());
				window->Show();
			}
			break;
		case kMsgGoToTimeDone:
			fPlayer->Seek(message->GetInt64("time", 0), true);
			break;
		case kMsgChapter:
		{
			int32 index;
			if (message->FindInt32("index", &index) == B_OK) {
				const std::vector<Chapter>& chapters = fPlayer->Chapters();
				if (index >= 0 && index < (int32)chapters.size())
					fPlayer->Seek(chapters[index].start, true);
			} else
				_GoToChapter(message->GetInt32("step", 1));
			break;
		}
		case kMsgSetRate:
		{
			double rate = message->GetDouble("rate", 1.0);
			fPlayer->SetRate(rate);
			if (!fPlayer->IsPlaying() && rate != 1.0)
				fPlayer->Play();
			_ShowMessage(rate == 1.0 ? "Normal speed" : "Speed %g×", rate);
			break;
		}
		case kMsgFaster:
			_StepSpeed(1);
			break;
		case kMsgSlower:
			_StepSpeed(-1);
			break;
		case kMsgJKL:
			_HandleJKL(message->GetInt32("key", 0));
			break;
		case kMsgToggleLoop:
			settings().loop = !fPlayer->IsLooping();
			fPlayer->SetLooping(settings().loop);
			_ShowMessage(settings().loop ? "Loop on" : "Loop off");
			break;

		case kMsgSetVolume:
			_SetVolume(message->GetFloat("volume", 1.0f));
			break;
		case kMsgVolumeUp:
		case kMsgVolumeDown:
		{
			float volume = settings().muted ? 0 : settings().volume;
			volume += message->what == kMsgVolumeUp ? 0.1f : -0.1f;
			_SetVolume(volume);
			_ShowMessage("Volume %d%%", (int)roundf(settings().volume * 100));
			break;
		}
		case kMsgToggleMute:
			settings().muted = !settings().muted;
			fPlayer->SetMuted(settings().muted);
			fControls->SetVolume(settings().volume, settings().muted);
			fHud->SetVolume(settings().volume, settings().muted);
			_ShowMessage(settings().muted ? "Sound off" : "Sound on");
			break;
		case kMsgSelectAudio:
		{
			int32 index = message->GetInt32("index", 0);
			fPlayer->SelectAudioTrack(index);
			const std::vector<TrackInfo>& tracks = fPlayer->AudioTracks();
			if (index >= 0 && index < (int32)tracks.size()) {
				_ShowMessage("Audio: %s",
					tracks[index].Label(index + 1).String());
				if (tracks[index].language.Length() > 0)
					settings().audioLanguage = tracks[index].language;
			}
			break;
		}
		case kMsgCycleAudio:
			_CycleAudio();
			break;
		case kMsgSelectSubtitle:
		{
			int32 index = message->GetInt32("index", -1);
			fPlayer->SelectSubtitleTrack(index);
			std::vector<TrackInfo> tracks = fPlayer->SubtitleTracks();
			if (index >= 0 && index < (int32)tracks.size()) {
				_ShowMessage("Subtitles: %s",
					tracks[index].Label(index + 1).String());
				if (tracks[index].language.Length() > 0)
					settings().subtitleLanguage = tracks[index].language;
			} else {
				_ShowMessage("Subtitles off");
				settings().subtitleLanguage = "off";
			}
			fVideo->Refresh();
			break;
		}
		case kMsgCycleSubtitle:
			_CycleSubtitles();
			break;
		case kMsgToggleCaptions:
			if (fPlayer->HasCaptions()) {
				fPlayer->SetCaptionsEnabled(!fPlayer->CaptionsEnabled());
				_ShowMessage(fPlayer->CaptionsEnabled()
					? "Closed captions on" : "Closed captions off");
				fVideo->Refresh();
			} else
				_ShowMessage("No closed captions");
			break;
		case kMsgAddSubtitleFile:
			if (fSubtitlePanel == NULL) {
				fSubtitlePanel = new BFilePanel(B_OPEN_PANEL,
					new BMessenger(this), NULL, B_FILE_NODE, false,
					new BMessage('subr'));
				fSubtitlePanel->Window()->SetTitle(
					"airTime: Add Subtitle File");
			}
			if (fHasFile) {
				BEntry entry(&fRef);
				BEntry parent;
				entry_ref directory;
				if (entry.GetParent(&parent) == B_OK
					&& parent.GetRef(&directory) == B_OK) {
					fSubtitlePanel->SetPanelDirectory(&directory);
				}
			}
			fSubtitlePanel->Show();
			break;
		case 'subr':
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) == B_OK)
				_AddSubtitleFile(ref);
			break;
		}
		case kMsgSubtitleSize:
			settings().subtitleScale = message->GetFloat("scale", 1.0f);
			fVideo->SetSubtitleScale(settings().subtitleScale);
			break;

		case kMsgToggleFullScreen:
			_SetFullScreen(!fFullScreen);
			break;
		case kMsgExitFullScreen:
			_SetFullScreen(false);
			break;
		case kMsgControlsActivity:
			fLastActivity = system_time();
			if (fFullScreen && !fHudShown)
				_ShowHud(true);
			break;
		case kMsgSize:
			if (fHasFile && !fFullScreen)
				_ResizeToVideo(message->GetFloat("scale", 1.0f));
			break;
		case kMsgToggleOnTop:
			fAlwaysOnTop = !fAlwaysOnTop;
			if (!fFullScreen) {
				SetFeel(fAlwaysOnTop ? B_FLOATING_ALL_WINDOW_FEEL
					: B_NORMAL_WINDOW_FEEL);
			}
			break;
		case kMsgToggleSnap:
			settings().snapToAspect = !settings().snapToAspect;
			if (settings().snapToAspect)
				_SnapToAspect();
			break;
		case kMsgToggleDevicePixels:
			settings().devicePixels = !settings().devicePixels;
			settings().Save();
			_ApplyDevicePixels();
			break;
		case kMsgToggleHardware:
			settings().hardwareDecoding = !settings().hardwareDecoding
				|| settings().noHardwareThisRun;
			settings().noHardwareThisRun = false;
			fPlayer->SetHardwareDecoding(settings().hardwareDecoding);
			_ShowMessage(settings().hardwareDecoding
				? "Hardware decoding on" : "Hardware decoding off");
			break;
		case kMsgToggleTimeDisplay:
			settings().timeDisplay = message->GetInt32("mode", 0);
			fControls->SetTimeDisplay(settings().timeDisplay);
			fHud->SetTimeDisplay(settings().timeDisplay);
			break;
		case kMsgVideoClicked:
			_HandleClick(message->GetInt32("clicks", 1));
			break;
		case 'ctxm':
		{
			BPoint where;
			if (message->FindPoint("where", &where) == B_OK && fHasFile)
				_ShowContextMenu(where);
			break;
		}

		case kMsgShowInspector:
			if (fInspector.IsValid()) {
				fInspector.SendMessage(B_QUIT_REQUESTED);
				fInspector = BMessenger();
			} else {
				BRect frame = settings().inspectorFrame;
				if (!frame.IsValid()) {
					frame.Set(0, 0, 440, 300);
					frame.OffsetTo(Frame().right + 12, Frame().top);
				}
				InspectorWindow* inspector = new InspectorWindow(frame,
					BMessenger(this));
				fInspector = BMessenger(inspector);
				_UpdateInspector();
				inspector->Show();
			}
			break;
		case kMsgInspectorClosed:
			message->FindRect("frame", &settings().inspectorFrame);
			fInspector = BMessenger();
			break;
		case kMsgShowInTracker:
			if (fHasFile) {
				BEntry entry(&fRef);
				BEntry parent;
				entry_ref directory;
				if (entry.GetParent(&parent) == B_OK
					&& parent.GetRef(&directory) == B_OK) {
					BMessage open(B_REFS_RECEIVED);
					open.AddRef("refs", &directory);
					BMessenger("application/x-vnd.Be-TRAK").SendMessage(&open);
				}
			}
			break;
		case kMsgOpenFile:
		case kMsgMakeDefault:
		{
			BMessage forward(*message);
			forward.AddMessenger("window", BMessenger(this));
			be_app->PostMessage(&forward);
			break;
		}
		case 'keys':
		{
			BAlert* alert = new BAlert("Keyboard Shortcuts",
				"Space\tPlay / pause (or click the picture)\n"
				"← →\tStep one frame (Shift: 10 seconds)\n"
				"Option ← →\tGo to the beginning / end\n"
				"↑ ↓\tVolume\n"
				"J K L\tReverse, pause, forward — press again for faster\n"
				"[ ]\tSlower / faster;  \\  normal speed\n"
				"F or double-click\tFull screen;  Esc  leaves it\n"
				"S, A\tNext subtitle track, next audio track\n"
				"C\tClosed captions on / off\n"
				"M\tMute\n"
				"Page Up / Down\tPrevious / next chapter\n"
				"I\tMovie Inspector\n\n"
				"Hold the fast forward or rewind button to scan, faster the "
				"longer it is held; click it to keep scanning.",
				"OK");
			alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
			alert->Go(NULL);
			break;
		}

		case B_SIMPLE_DATA:
		case B_REFS_RECEIVED:
			_HandleDrop(message);
			break;

		case B_GET_PROPERTY:
		case B_SET_PROPERTY:
			if (!_HandleScripting(message))
				BWindow::MessageReceived(message);
			break;

		default:
			BWindow::MessageReceived(message);
	}
}


// #pragma mark - scripting


BHandler*
PlayerWindow::ResolveSpecifier(BMessage* message, int32 index,
	BMessage* specifier, int32 what, const char* property)
{
	BPropertyInfo info(sProperties);
	if (info.FindMatch(message, index, specifier, what, property) >= 0)
		return this;
	return BWindow::ResolveSpecifier(message, index, specifier, what, property);
}


status_t
PlayerWindow::GetSupportedSuites(BMessage* data)
{
	data->AddString("suites", "suite/vnd.airOS-airTime-player");
	BPropertyInfo info(sProperties);
	data->AddFlat("messages", &info);
	return BWindow::GetSupportedSuites(data);
}


static status_t
find_number(BMessage* message, double* value)
{
	int64 int64Value;
	int32 int32Value;
	float floatValue;
	if (message->FindDouble("data", value) == B_OK)
		return B_OK;
	if (message->FindFloat("data", &floatValue) == B_OK) {
		*value = floatValue;
		return B_OK;
	}
	if (message->FindInt64("data", &int64Value) == B_OK) {
		*value = (double)int64Value;
		return B_OK;
	}
	if (message->FindInt32("data", &int32Value) == B_OK) {
		*value = int32Value;
		return B_OK;
	}
	return B_BAD_TYPE;
}


bool
PlayerWindow::_HandleScripting(BMessage* message)
{
	int32 index;
	BMessage specifier;
	int32 what;
	const char* property;
	if (message->GetCurrentSpecifier(&index, &specifier, &what, &property)
			!= B_OK) {
		return false;
	}
	BPropertyInfo info(sProperties);
	if (info.FindMatch(message, index, &specifier, what, property) < 0)
		return false;

	bool get = message->what == B_GET_PROPERTY;
	BMessage reply(B_REPLY);
	status_t status = B_OK;
	BString name(property);

	if (name == "Position") {
		if (get)
			reply.AddInt64("result", fPlayer->Position());
		else {
			// Microseconds, or a time as the Go to Time panel takes it
			// ("1:10:00"): hey sends numbers as 32 bits, which ends at
			// 35 minutes in microseconds.
			double time;
			BString text;
			bigtime_t parsed;
			if (message->FindString("data", &text) == B_OK) {
				status = parse_time(text.String(), &parsed) ? B_OK : B_BAD_VALUE;
				if (status == B_OK)
					fPlayer->Seek(parsed, true);
			} else {
				status = find_number(message, &time);
				if (status == B_OK)
					fPlayer->Seek((bigtime_t)time, true);
			}
		}
	} else if (name == "Duration") {
		reply.AddInt64("result", fPlayer->Duration());
	} else if (name == "Playing") {
		if (get)
			reply.AddBool("result", fPlayer->IsPlaying());
		else {
			bool playing;
			status = message->FindBool("data", &playing);
			if (status == B_OK) {
				if (playing)
					fPlayer->Play();
				else
					fPlayer->Pause();
			}
		}
	} else if (name == "Rate") {
		if (get)
			reply.AddDouble("result", fPlayer->Rate());
		else {
			double rate;
			status = find_number(message, &rate);
			if (status == B_OK) {
				fPlayer->SetRate(rate);
				if (!fPlayer->IsPlaying())
					fPlayer->Play();
			}
		}
	} else if (name == "Volume") {
		if (get)
			reply.AddFloat("result", settings().volume);
		else {
			double volume;
			status = find_number(message, &volume);
			if (status == B_OK)
				_SetVolume((float)volume);
		}
	} else if (name == "Muted") {
		if (get)
			reply.AddBool("result", settings().muted);
		else {
			bool muted;
			status = message->FindBool("data", &muted);
			if (status == B_OK) {
				settings().muted = muted;
				fPlayer->SetMuted(muted);
				fControls->SetVolume(settings().volume, muted);
			}
		}
	} else if (name == "AudioTrack") {
		if (get)
			reply.AddInt32("result", fPlayer->CurrentAudioTrack());
		else {
			double track;
			status = find_number(message, &track);
			if (status == B_OK)
				fPlayer->SelectAudioTrack((int)track);
		}
	} else if (name == "Subtitle") {
		if (get)
			reply.AddInt32("result", fPlayer->CurrentSubtitleTrack());
		else {
			double track;
			status = find_number(message, &track);
			if (status == B_OK) {
				fPlayer->SelectSubtitleTrack((int)track);
				fVideo->Refresh();
			}
		}
	} else if (name == "Captions") {
		if (get)
			reply.AddBool("result", fPlayer->CaptionsEnabled());
		else {
			bool enabled;
			status = message->FindBool("data", &enabled);
			if (status == B_OK) {
				fPlayer->SetCaptionsEnabled(enabled);
				fVideo->Refresh();
			}
		}
	} else if (name == "FullScreen") {
		if (get)
			reply.AddBool("result", fFullScreen);
		else {
			bool fullScreen;
			status = message->FindBool("data", &fullScreen);
			if (status == B_OK)
				_SetFullScreen(fullScreen);
		}
	} else if (name == "File") {
		if (get) {
			BPath path(&fRef);
			reply.AddString("result", fHasFile ? path.Path() : "");
		} else {
			const char* path;
			status = message->FindString("data", &path);
			entry_ref ref;
			if (status == B_OK)
				status = get_ref_for_path(path, &ref);
			BString error;
			if (status == B_OK)
				status = OpenFile(ref, settings().autoPlay, &error);
			if (status != B_OK)
				reply.AddString("message", error);
		}
	} else if (name == "Decoder") {
		BString decoder = fPlayer->VideoDecoderName();
		decoder << (fPlayer->VideoDecoderIsHardware() ? " [hardware]"
			: " [software]");
		BString note = fPlayer->HardwareDecoderNote();
		if (note.Length() > 0)
			decoder << " (" << note << ")";
		decoder << "; audio: " << fPlayer->AudioDecoderName();
		reply.AddString("result", decoder);
	} else if (name == "Stats") {
		PlayerStats stats = fPlayer->Stats();
		BString text;
		text.SetToFormat("position=%.3f rate=%g playing=%d scanning=%d "
			"shown=%" B_PRId64 " dropped=%" B_PRId64 " fps=%.1f "
			"av=%.1fms audiobuf=%.0fms vq=%d aq=%d fq=%d "
			"decode=%.1fms compose=%.1fms draw=%.1fms phases=%d/%d/%d "
			"scanseeks=%d scanshown=%.3f awaiting=%d direct=%d hud=%d "
			"hurry=%d late=%.0fms",
			fPlayer->Position() / 1e6, fPlayer->Rate(), fPlayer->IsPlaying(),
			fPlayer->IsScanning(), stats.framesShown, stats.framesDropped,
			stats.displayRate, stats.avOffset / 1000.0,
			stats.audioBuffered / 1000.0, stats.videoQueued,
			stats.audioQueued, stats.framesQueued, stats.decodeTime / 1000.0,
			stats.composeTime / 1000.0, stats.drawTime / 1000.0,
			stats.demuxPhase, stats.decodePhase, stats.presentPhase,
			stats.scanSeeks, stats.scanShown / 1e6, stats.scanAwaiting,
			fVideo->DrawsDirectly(), fHudShown, stats.hurry,
			stats.lateness / 1000.0);
		reply.AddString("result", text);
	} else if (name == "Tracks") {
		BString text;
		const std::vector<TrackInfo>& audio = fPlayer->AudioTracks();
		for (size_t i = 0; i < audio.size(); i++) {
			text << "audio " << (int32)i << ": "
				<< audio[i].Label((int)i + 1) << "\n";
		}
		std::vector<TrackInfo> subtitles = fPlayer->SubtitleTracks();
		for (size_t i = 0; i < subtitles.size(); i++) {
			text << "subtitle " << (int32)i << ": "
				<< subtitles[i].Label((int)i + 1) << "\n";
		}
		text << "captions: " << (fPlayer->HasCaptions() ? "yes" : "no");
		reply.AddString("result", text);
	} else if (name == "SubtitleText") {
		std::vector<SubtitleEventPtr> events
			= fPlayer->ActiveSubtitles(fPlayer->Position());
		BString text;
		for (const SubtitleEventPtr& event : events) {
			if (text.Length() > 0)
				text << " | ";
			if (event->IsBitmap())
				text << "[picture " << (int32)event->bitmaps.size() << "]";
			else
				text << subtitle_plain_text(*event);
		}
		reply.AddString("result", text);
	} else if (name == "Size") {
		double scale;
		status = find_number(message, &scale);
		if (status == B_OK)
			_ResizeToVideo((float)scale);
	} else if (name == "SubtitleFile") {
		const char* path;
		status = message->FindString("data", &path);
		BString error;
		if (status == B_OK)
			status = fPlayer->AddSubtitleFile(path, &error);
		if (status != B_OK)
			reply.AddString("message", error);
		fVideo->Refresh();
	} else if (name == "HardwareDecoding") {
		if (get)
			reply.AddBool("result", settings().hardwareDecoding
				&& !settings().noHardwareThisRun);
		else {
			bool enabled;
			status = message->FindBool("data", &enabled);
			if (status == B_OK) {
				settings().hardwareDecoding = enabled;
				settings().noHardwareThisRun = false;
				fPlayer->SetHardwareDecoding(enabled);
			}
		}
	} else if (name == "DevicePixels") {
		if (get)
			reply.AddBool("result", settings().devicePixels);
		else {
			bool enabled;
			status = message->FindBool("data", &enabled);
			if (status == B_OK) {
				settings().devicePixels = enabled;
				settings().Save();
				_ApplyDevicePixels();
			}
		}
	} else if (name == "Frame") {
		if (get)
			reply.AddRect("result", Frame());
		else {
			BRect frame;
			status = message->FindRect("data", &frame);
			if (status == B_OK && !fFullScreen) {
				MoveTo(frame.LeftTop());
				ResizeTo(frame.Width(), frame.Height());
			}
		}
	} else if (name == "Loop") {
		if (get)
			reply.AddBool("result", fPlayer->IsLooping());
		else {
			bool loop;
			status = message->FindBool("data", &loop);
			if (status == B_OK) {
				settings().loop = loop;
				fPlayer->SetLooping(loop);
			}
		}
	} else
		return false;

	reply.AddInt32("error", status);
	message->SendReply(&reply);
	return true;
}

}	// namespace airtime
