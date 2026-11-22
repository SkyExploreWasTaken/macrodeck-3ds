#pragma once

#include <stdbool.h>

typedef enum {
	CONN_MODE_AUTO = 0,   // browse the network on launch, then connect
	CONN_MODE_MANUAL = 1, // always use the saved address
} ConnMode;

typedef struct {
	int mode;
	char host[128];
	int port;
	char host_name[64];

	char client_id[40];
	char access_token[1200];
	char refresh_cookie_name[48];
	char refresh_cookie_value[400];
	char device_id[48];
	char device_secret[160];
	char startup_profile_id[48];
	char last_profile_id[48];
} Config;

void config_defaults(Config *c);
bool config_load(Config *c);
bool config_save(const Config *c);
// Drops everything the host gave us (tokens, device credential) but keeps the address.
void config_forget_session(Config *c);
