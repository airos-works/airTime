/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_APP_H
#define AIRTIME_APP_H


#include <Application.h>
#include <Messenger.h>


class BFilePanel;


namespace airtime {

class PlayerWindow;


class App : public BApplication {
public:
								App();
	virtual						~App();

	virtual	void				ArgvReceived(int32 argc, char** argv);
	virtual	void				RefsReceived(BMessage* message);
	virtual	void				ReadyToRun();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				AboutRequested();
	virtual	bool				QuitRequested();

	// Makes airTime the preferred application for the film and music
	// types; returns how many types changed.
	static	int32				RegisterAsDefaultPlayer(bool force);

private:
			void				_HandleMediaKey(BMessage* message);
			PlayerWindow*		_NewWindow();
			PlayerWindow*		_EmptyWindow();
			void				_Open(const entry_ref& ref,
									PlayerWindow* window);
			BRect				_NextFrame();

			BFilePanel*			fOpenPanel;
			BMessenger			fOpenTarget;
			int32				fWindowCount;
			bool				fFullScreen;
			bigtime_t			fStartAt;
			bool				fPlay;
};

}	// namespace airtime

#endif	// AIRTIME_APP_H
