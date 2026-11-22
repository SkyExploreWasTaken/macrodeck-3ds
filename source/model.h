// State shared between the network worker (writer) and the UI thread (reader).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "mdns.h"
#include "platform.h"

#define MAX_WIDGETS 96
#define MAX_LINES 3
#define MAX_IMAGES 128
#define MAX_FOUND 8
#define IMAGE_TEX 64

typedef enum {
	PHASE_IDLE,          // nothing to connect to yet: show the setup menu
	PHASE_DISCOVERING,   // browsing the network
	PHASE_PICK_HOST,     // discovery finished, list shown
	PHASE_CONNECTING,    // reaching the host / loading the deck
	PHASE_NEED_PAIRING,  // the host wants a pairing code
	PHASE_PAIRING,       // redeeming the code
	PHASE_ONLINE,        // deck live
	PHASE_RETRY_WAIT,    // lost the host, retrying shortly
	PHASE_ERROR,         // something needs the user (message in error)
} Phase;

typedef struct {
	char text[128];
	uint32_t color; // RGBA8 (0xRRGGBBAA)
	bool has_color;
	bool muted;
} TextLine;

typedef struct {
	char id[40];
	char type[40];
	int x, y, w, h;

	bool has_tree;
	bool transparent;  // the tree asked for no tile face
	bool has_bg;
	uint32_t bg;
	bool has_border;
	uint32_t border;
	int image;         // index into the image table or -1
	bool has_tint;
	uint32_t tint;
	int justify;       // -1 top, 0 center, 1 bottom
	bool has_value;    // sliders, bars, gauges
	float value;       // 0..1
	TextLine lines[MAX_LINES];
	int nlines;
	bool pressable;
	bool slider;       // a ui.slider that takes "change"
	bool vertical;
} WidgetView;

typedef enum {
	IMG_EMPTY = 0,
	IMG_QUEUED,   // worker should fetch
	IMG_DECODED,  // rgba ready, UI thread uploads
	IMG_READY,    // texture live
	IMG_FAILED,
} ImageState;

typedef struct {
	char key[200];   // resourceId|contentHash
	char url[300];   // path on the host
	ImageState state;
	uint8_t *rgba;   // IMAGE_TEX x IMAGE_TEX, row-major, RGBA
	int w, h;        // content size inside the texture
	void *tex;       // UI-side texture object (opaque here)
} ImageSlot;

typedef struct {
	plat_mutex lock;
	uint32_t version;          // bumped on any change the UI should redraw

	Phase phase;
	char status[128];
	char error[192];
	char toast[160];
	uint64_t toast_until;

	char host_name[64];
	char host_addr[140];
	char host_version[40];
	int mode;

	char profile_name[64];
	int profile_index, profile_count;
	char folder_name[64];
	char folder_path[160];
	int cols, rows;
	bool has_folder_bg;
	uint32_t folder_bg;
	bool can_go_back;

	WidgetView widgets[MAX_WIDGETS];
	int nwidgets;

	MdnsHost found[MAX_FOUND];
	int nfound;

	ImageSlot images[MAX_IMAGES];
} Model;

extern Model g_model;

void model_init(void);
void model_touch(void); // call with the lock held after changing anything
void model_toast(const char *fmt, ...);
