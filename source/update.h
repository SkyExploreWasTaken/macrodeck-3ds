// Checks the repository's Nightly build release and installs it: the .cia through the system installer when
// running as an installed title, or the .3dsx by replacing this file when started from the Homebrew Launcher.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	UPD_IDLE,
	UPD_CHECKING,
	UPD_CURRENT,     // already on the newest build
	UPD_AVAILABLE,
	UPD_DOWNLOADING, // progress is valid
	UPD_INSTALLING,
	UPD_DONE,        // restart the app to use it
	UPD_FAILED,      // message says why
} UpdateState;

typedef struct {
	UpdateState state;
	char latest[16];   // short commit of the newest nightly
	float progress;    // 0..1 while downloading
	char message[96];
} UpdateStatus;

// self_path: where this .3dsx lives (argv[0]), or NULL. Does nothing when started as a CIA.
void update_init(const char *self_path);
void update_check(void);    // runs in the background
void update_install(void);  // runs in the background, only from UPD_AVAILABLE or UPD_FAILED
void update_status(UpdateStatus *out);
void update_exit(void);
