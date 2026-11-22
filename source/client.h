// The network worker: talks to the Macro Deck 3 host and keeps g_model current.
#pragma once

#include "config.h"

typedef enum {
	CMD_DISCOVER,      // browse the network
	CMD_CONNECT,       // a = host, b = display name, n = port
	CMD_PAIR,          // a = six-digit pairing code
	CMD_PRESS,         // a = widget id, b = event name (press, long-press, press-start, press-end)
	CMD_SLIDER,        // a = widget id, n = level * 10000
	CMD_BACK,          // previous folder (or parent)
	CMD_PROFILE_STEP,  // n = +1 / -1
	CMD_RECONNECT,
	CMD_FORGET,        // drop the pairing with this host
	CMD_SET_MODE,      // n = ConnMode
	CMD_STARTUP,       // run the launch policy (auto-discovery or saved address)
	CMD_SET_SCREEN,    // n = screen-off minutes | (pixel shift ? 0x100 : 0); just saved
} CommandType;

void client_start(const Config *initial);
void client_stop(void);
void client_cmd(CommandType type, const char *a, const char *b, int n);

// The console is going to sleep (waking = false) or just woke up (waking = true). Errors from the dropped
// connection are kept off the screen around this, and waking reconnects straight away.
void client_sleep_notice(bool waking);

// Snapshot of the worker's config (for the UI's menus).
void client_config(Config *out);
