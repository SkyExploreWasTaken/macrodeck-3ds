// Macro Deck 3 client for the Nintendo 3DS: status on the top screen, the deck on the touch screen.
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "client.h"
#include "config.h"
#include "model.h"
#include "net.h"
#include "platform.h"
#include "update.h"

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240

#define COL_BG        C2D_Color32(0x0E, 0x11, 0x16, 0xFF)
#define COL_PANEL     C2D_Color32(0x1A, 0x1F, 0x29, 0xFF)
#define COL_PANEL_HI  C2D_Color32(0x26, 0x2D, 0x3B, 0xFF)
#define COL_TILE      C2D_Color32(0x2A, 0x30, 0x3C, 0xFF)
#define COL_EMPTY     C2D_Color32(0x16, 0x1A, 0x22, 0xFF)
#define COL_TEXT      C2D_Color32(0xF2, 0xF4, 0xF8, 0xFF)
#define COL_MUTED     C2D_Color32(0x9A, 0xA3, 0xB5, 0xFF)
#define COL_ACCENT    C2D_Color32(0x4F, 0x7C, 0xFF, 0xFF)
#define COL_GOOD      C2D_Color32(0x3C, 0xD0, 0x7A, 0xFF)
#define COL_WARN      C2D_Color32(0xF5, 0xB7, 0x2E, 0xFF)
#define COL_BAD       C2D_Color32(0xF0, 0x52, 0x52, 0xFF)
#define COL_SHADOW    C2D_Color32(0x00, 0x00, 0x00, 0x90)

#define LONG_PRESS_MS 600

/* ---------- drawing helpers ---------- */

static C2D_TextBuf g_dyn;     // rebuilt every frame (chrome, menus)
static float g_line_feed = 30.0f;

static u32 rgba_to_c2d(uint32_t rgba)
{
	return C2D_Color32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
}

static float luminance(uint32_t rgba)
{
	return (0.299f * ((rgba >> 24) & 0xFF) + 0.587f * ((rgba >> 16) & 0xFF) + 0.114f * ((rgba >> 8) & 0xFF)) / 255.0f;
}

static void rrect(float x, float y, float w, float h, float r, u32 color)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (r < 1) {
		C2D_DrawRectSolid(x, y, 0, w, h, color);
		return;
	}
	C2D_DrawRectSolid(x + r, y, 0, w - 2 * r, h, color);
	C2D_DrawRectSolid(x, y + r, 0, r, h - 2 * r, color);
	C2D_DrawRectSolid(x + w - r, y + r, 0, r, h - 2 * r, color);
	C2D_DrawCircleSolid(x + r, y + r, 0, r, color);
	C2D_DrawCircleSolid(x + w - r, y + r, 0, r, color);
	C2D_DrawCircleSolid(x + r, y + h - r, 0, r, color);
	C2D_DrawCircleSolid(x + w - r, y + h - r, 0, r, color);
}

static void rrect_outline(float x, float y, float w, float h, float t, u32 color)
{
	C2D_DrawRectSolid(x, y, 0, w, t, color);
	C2D_DrawRectSolid(x, y + h - t, 0, w, t, color);
	C2D_DrawRectSolid(x, y, 0, t, h, color);
	C2D_DrawRectSolid(x + w - t, y, 0, t, h, color);
}

// Draws text with the system font. flags: C2D_AlignLeft/Center/Right. Returns the drawn width.
static float text(const char *s, float x, float y, float scale, u32 color, u32 flags)
{
	if (!s || !*s) return 0;
	C2D_Text t;
	C2D_TextParse(&t, g_dyn, s);
	C2D_TextOptimize(&t);
	float w = 0, h = 0;
	C2D_TextGetDimensions(&t, scale, scale, &w, &h);
	C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0, scale, scale, color);
	return w;
}

// Same, but shrinks to max_w and ellipsizes if even the smallest scale does not fit.
static void text_fit(const char *s, float x, float y, float scale, float min_scale, float max_w, u32 color, u32 flags)
{
	if (!s || !*s) return;
	C2D_Text t;
	C2D_TextParse(&t, g_dyn, s);
	float w, h;
	C2D_TextGetDimensions(&t, 1, 1, &w, &h);
	if (w * scale > max_w) scale = max_w / w;
	if (scale >= min_scale) {
		C2D_TextOptimize(&t);
		C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0, scale, scale, color);
		return;
	}
	char buf[160];
	snprintf(buf, sizeof buf, "%s", s);
	size_t len = strlen(buf);
	// Jump close to the right length first; every parse spends glyphs from the frame's text buffer.
	size_t guess = (size_t)(len * (max_w / (w * min_scale))) + 2;
	if (guess < len) {
		len = guess;
		while (len > 0 && (buf[len] & 0xC0) == 0x80) len--;
		buf[len] = 0;
	}
	while (len > 1) {
		len--;
		while (len > 0 && (buf[len] & 0xC0) == 0x80) len--; // stay on UTF-8 boundaries
		buf[len] = 0;
		char tmp[170];
		snprintf(tmp, sizeof tmp, "%s…", buf);
		C2D_TextParse(&t, g_dyn, tmp);
		C2D_TextGetDimensions(&t, min_scale, min_scale, &w, &h);
		if (w <= max_w) break;
	}
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0, min_scale, min_scale, color);
}

/* ---------- textures for widget artwork ---------- */

static C3D_Tex g_tex[MAX_IMAGES];
static Tex3DS_SubTexture g_sub[MAX_IMAGES];
static bool g_tex_live[MAX_IMAGES];

static void upload_image(int slot, const uint8_t *rgba, int w, int h)
{
	C3D_Tex *tex = &g_tex[slot];
	if (!g_tex_live[slot]) {
		if (!C3D_TexInit(tex, IMAGE_TEX, IMAGE_TEX, GPU_RGBA8)) return;
		C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		g_tex_live[slot] = true;
	}
	u8 *dst = tex->data;
	memset(dst, 0, IMAGE_TEX * IMAGE_TEX * 4);
	for (int y = 0; y < IMAGE_TEX; y++) {
		for (int x = 0; x < IMAGE_TEX; x++) {
			// 8x8 Morton-ordered tiles, as the PICA200 expects.
			u32 di = ((((y >> 3) * (IMAGE_TEX >> 3) + (x >> 3)) << 6) +
				((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))) * 4;
			const uint8_t *s = rgba + ((size_t)y * IMAGE_TEX + x) * 4;
			dst[di + 0] = s[3];
			dst[di + 1] = s[2];
			dst[di + 2] = s[1];
			dst[di + 3] = s[0];
		}
	}
	C3D_TexFlush(tex);
	g_sub[slot].width = (u16)w;
	g_sub[slot].height = (u16)h;
	g_sub[slot].left = 0.0f;
	g_sub[slot].top = 1.0f;
	g_sub[slot].right = (float)w / IMAGE_TEX;
	g_sub[slot].bottom = 1.0f - (float)h / IMAGE_TEX;
}

static void pump_images(void)
{
	for (int i = 0; i < MAX_IMAGES; i++) {
		uint8_t *rgba = NULL;
		int w = 0, h = 0;
		plat_mutex_lock(&g_model.lock);
		ImageSlot *s = &g_model.images[i];
		if (s->state == IMG_DECODED) {
			rgba = s->rgba;
			s->rgba = NULL;
			w = s->w;
			h = s->h;
			s->state = IMG_READY;
		}
		plat_mutex_unlock(&g_model.lock);
		if (rgba) {
			upload_image(i, rgba, w, h);
			free(rgba);
		}
	}
}

/* ---------- UI state ---------- */

typedef enum {
	ACT_NONE,
	ACT_DISCOVER,
	ACT_MANUAL,
	ACT_PAIR,
	ACT_TOGGLE_MODE,
	ACT_FORGET,
	ACT_RECONNECT,
	ACT_CLOSE_MENU,
	ACT_EXIT,
	ACT_MENU,
	ACT_UPDATE,
	ACT_HOST, // + index
} Action;

typedef struct {
	float x, y, w, h;
	char label[96];
	char sub[96];
	int action;
	int arg;
	bool primary;
} Button;

static Model V;            // the UI thread's snapshot of g_model
static uint32_t g_seen_version = 0xFFFFFFFF;
static bool g_menu;
static bool g_quit;

static UpdateStatus g_upd;
static UpdateState g_upd_prev = UPD_IDLE;

static Button g_buttons[16];
static int g_nbuttons;

// Touch tracking on the deck.
static int g_touch_widget = -1;
static u64 g_touch_start;
static bool g_long_fired;
static bool g_touch_slider;
static float g_slider_level;
static bool g_touch_button;
static int g_touch_button_idx = -1;
static bool g_touch_inside;

static void snapshot(void)
{
	plat_mutex_lock(&g_model.lock);
	if (g_model.version != g_seen_version) {
		// Copy everything except the lock and the image table (handled by pump_images).
		size_t head = offsetof(Model, version);
		size_t tail = offsetof(Model, images);
		memcpy((char *)&V + head, (char *)&g_model + head, tail - head);
		g_seen_version = g_model.version;
	}
	plat_mutex_unlock(&g_model.lock);
}

static void add_button(float x, float y, float w, float h, const char *label, const char *sub, int action, int arg,
	bool primary)
{
	if (g_nbuttons >= (int)(sizeof g_buttons / sizeof g_buttons[0])) return;
	Button *b = &g_buttons[g_nbuttons++];
	b->x = x;
	b->y = y;
	b->w = w;
	b->h = h;
	snprintf(b->label, sizeof b->label, "%s", label);
	snprintf(b->sub, sizeof b->sub, "%s", sub ? sub : "");
	b->action = action;
	b->arg = arg;
	b->primary = primary;
}

/* ---------- keyboard prompts ---------- */

static void prompt_manual_address(void)
{
	Config cfg;
	client_config(&cfg);
	SwkbdState kb;
	char host[128];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof host - 1);
	swkbdSetHintText(&kb, "Computer address, e.g. 192.168.1.20");
	swkbdSetInitialText(&kb, cfg.host);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	swkbdSetFeatures(&kb, SWKBD_PREDICTIVE_INPUT);
	if (swkbdInputText(&kb, host, sizeof host) != SWKBD_BUTTON_CONFIRM) return;

	// Trim spaces and an optional scheme; accept "host:port" in one go.
	char *h = host;
	while (*h == ' ') h++;
	if (strncmp(h, "http://", 7) == 0) h += 7;
	char *slash = strchr(h, '/');
	if (slash) *slash = 0;
	size_t len = strlen(h);
	while (len && h[len - 1] == ' ') h[--len] = 0;
	int port = cfg.port > 0 ? cfg.port : 8193;
	char *colon = strrchr(h, ':');
	if (colon) {
		*colon = 0;
		int p = atoi(colon + 1);
		if (p > 0 && p < 65536) port = p;
	} else {
		char portbuf[8], init[8];
		snprintf(init, sizeof init, "%d", port);
		swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 5);
		swkbdSetHintText(&kb, "Port (Macro Deck: Settings > Network)");
		swkbdSetInitialText(&kb, init);
		swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
		if (swkbdInputText(&kb, portbuf, sizeof portbuf) != SWKBD_BUTTON_CONFIRM) return;
		int p = atoi(portbuf);
		if (p > 0 && p < 65536) port = p;
	}
	if (!*h) return;
	client_cmd(CMD_SET_MODE, NULL, NULL, CONN_MODE_MANUAL);
	client_cmd(CMD_CONNECT, h, strcmp(h, cfg.host) == 0 ? cfg.host_name : "", port);
}

static void prompt_pairing_code(void)
{
	SwkbdState kb;
	char code[8];
	swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 6);
	swkbdSetHintText(&kb, "6-digit pairing code");
	swkbdSetValidation(&kb, SWKBD_FIXEDLEN, 0, 0);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "Pair", true);
	if (swkbdInputText(&kb, code, sizeof code) != SWKBD_BUTTON_CONFIRM) return;
	if (strlen(code) != 6) return;
	client_cmd(CMD_PAIR, code, NULL, 0);
}

static void run_action(int action, int arg)
{
	switch (action) {
	case ACT_DISCOVER:
		g_menu = false;
		client_cmd(CMD_DISCOVER, NULL, NULL, 0);
		break;
	case ACT_MANUAL:
		g_menu = false;
		prompt_manual_address();
		break;
	case ACT_PAIR:
		g_menu = false;
		prompt_pairing_code();
		break;
	case ACT_TOGGLE_MODE:
		client_cmd(CMD_SET_MODE, NULL, NULL, V.mode == CONN_MODE_AUTO ? CONN_MODE_MANUAL : CONN_MODE_AUTO);
		break;
	case ACT_FORGET:
		g_menu = false;
		client_cmd(CMD_FORGET, NULL, NULL, 0);
		break;
	case ACT_RECONNECT:
		g_menu = false;
		client_cmd(CMD_RECONNECT, NULL, NULL, 0);
		break;
	case ACT_CLOSE_MENU:
		g_menu = false;
		break;
	case ACT_MENU:
		g_menu = true;
		break;
	case ACT_EXIT:
		g_quit = true;
		break;
	case ACT_UPDATE:
		if (g_upd.state == UPD_AVAILABLE) update_install();
		else if (g_upd.state == UPD_DONE) g_quit = true;
		else if (g_upd.state == UPD_FAILED && g_upd.latest[0]) update_install();
		else update_check();
		break;
	case ACT_HOST:
		if (arg >= 0 && arg < V.nfound) {
			client_cmd(CMD_CONNECT, V.found[arg].ip, V.found[arg].name, V.found[arg].port);
		}
		break;
	}
}

/* ---------- bottom screen layouts ---------- */

static void layout_menu(void)
{
	float x = 12, w = BOT_W - 24, h = 25, y = 24, gap = 3;
	add_button(x, y, w, h, "Find computers on the network", NULL, ACT_DISCOVER, 0, false);
	y += h + gap;
	add_button(x, y, w, h, "Enter address manually", NULL, ACT_MANUAL, 0, false);
	y += h + gap;
	add_button(x, y, w, h, V.mode == CONN_MODE_AUTO ? "On launch: find on network" : "On launch: use saved address",
		NULL, ACT_TOGGLE_MODE, 0, false);
	y += h + gap;
	add_button(x, y, w, h, "Pair with a new code", NULL, ACT_PAIR, 0, false);
	y += h + gap;
	add_button(x, y, (w - gap) / 2, h, "Reconnect", NULL, ACT_RECONNECT, 0, false);
	add_button(x + (w + gap) / 2, y, (w - gap) / 2, h, "Forget pairing", NULL, ACT_FORGET, 0, false);
	y += h + gap;
	char upd[96];
	switch (g_upd.state) {
	case UPD_CHECKING: snprintf(upd, sizeof upd, "Checking for updates..."); break;
	case UPD_CURRENT: snprintf(upd, sizeof upd, "Up to date (%s). Check again", APP_BUILD_STR); break;
	case UPD_AVAILABLE: snprintf(upd, sizeof upd, "Install update %s", g_upd.latest); break;
	case UPD_DOWNLOADING: snprintf(upd, sizeof upd, "Downloading update... %d%%", (int)(g_upd.progress * 100)); break;
	case UPD_INSTALLING: snprintf(upd, sizeof upd, "Installing update..."); break;
	case UPD_DONE: snprintf(upd, sizeof upd, "Updated! Tap to close, then reopen"); break;
	case UPD_FAILED: snprintf(upd, sizeof upd, "Update failed. Tap to retry"); break;
	default: snprintf(upd, sizeof upd, "Check for updates"); break;
	}
	add_button(x, y, w, h, upd, NULL, ACT_UPDATE, 0, g_upd.state == UPD_AVAILABLE || g_upd.state == UPD_DONE);
	y += h + gap;
	add_button(x, y, (w - gap) / 2, h, "Close menu", NULL, ACT_CLOSE_MENU, 0, true);
	add_button(x + (w + gap) / 2, y, (w - gap) / 2, h, "Exit", NULL, ACT_EXIT, 0, false);
}

static void layout_hosts(void)
{
	float x = 12, w = BOT_W - 24, y = 30, h = 32, gap = 4;
	int shown = V.nfound < 4 ? V.nfound : 4;
	for (int i = 0; i < shown; i++) {
		char sub[64];
		snprintf(sub, sizeof sub, "%s:%d%s%s", V.found[i].ip, V.found[i].port, V.found[i].version[0] ? "  v" : "",
			V.found[i].version);
		add_button(x, y, w, h, V.found[i].name, sub, ACT_HOST, i, i == 0);
		y += h + gap;
	}
	float by = SCR_H - 36;
	add_button(x, by, (w - gap) / 2, 28, "Search again", NULL, ACT_DISCOVER, 0, V.nfound == 0);
	add_button(x + (w + gap) / 2, by, (w - gap) / 2, 28, "Enter address", NULL, ACT_MANUAL, 0, false);
}

static void layout_idle(void)
{
	float x = 24, w = BOT_W - 48;
	add_button(x, 92, w, 40, "Find my computer", "Macro Deck must show itself on the network", ACT_DISCOVER, 0, true);
	add_button(x, 142, w, 40, "Enter address manually", "IP and port from Settings > Network", ACT_MANUAL, 0, false);
}

static void layout_pairing(void)
{
	add_button(40, 150, BOT_W - 80, 40, "Enter pairing code", NULL, ACT_PAIR, 0, true);
	add_button(40, 198, (BOT_W - 88) / 2, 28, "Other computer", NULL, ACT_DISCOVER, 0, false);
	add_button(48 + (BOT_W - 88) / 2, 198, (BOT_W - 88) / 2, 28, "Enter address", NULL, ACT_MANUAL, 0, false);
}

static void layout_retry(void)
{
	add_button(40, 160, (BOT_W - 88) / 2, 30, "Retry now", NULL, ACT_RECONNECT, 0, true);
	add_button(48 + (BOT_W - 88) / 2, 160, (BOT_W - 88) / 2, 30, "Menu", NULL, ACT_MENU, 0, false);
}

static bool deck_visible(void)
{
	return !g_menu && V.nwidgets > 0 &&
		(V.phase == PHASE_ONLINE || V.phase == PHASE_CONNECTING || V.phase == PHASE_RETRY_WAIT);
}

static void build_layout(void)
{
	g_nbuttons = 0;
	if (g_menu) {
		layout_menu();
		return;
	}
	if (deck_visible()) return;
	switch (V.phase) {
	case PHASE_PICK_HOST: layout_hosts(); break;
	case PHASE_IDLE: layout_idle(); break;
	case PHASE_NEED_PAIRING: layout_pairing(); break;
	case PHASE_RETRY_WAIT:
	case PHASE_ERROR: layout_retry(); break;
	default: break;
	}
}

static void draw_buttons(void)
{
	for (int i = 0; i < g_nbuttons; i++) {
		Button *b = &g_buttons[i];
		bool hot = g_touch_button && g_touch_button_idx == i && g_touch_inside;
		u32 fill = b->primary ? COL_ACCENT : (hot ? COL_PANEL_HI : COL_PANEL);
		if (b->primary && hot) fill = C2D_Color32(0x6B, 0x92, 0xFF, 0xFF);
		rrect(b->x, b->y, b->w, b->h, 6, fill);
		if (hot && !b->primary) rrect_outline(b->x, b->y, b->w, b->h, 1, COL_ACCENT);
		if (b->sub[0]) {
			text_fit(b->label, b->x + b->w / 2, b->y + 3, 0.55f, 0.35f, b->w - 12, COL_TEXT, C2D_AlignCenter);
			text_fit(b->sub, b->x + b->w / 2, b->y + b->h - 15, 0.42f, 0.3f, b->w - 12, b->primary ? COL_TEXT : COL_MUTED,
				C2D_AlignCenter);
		} else {
			text_fit(b->label, b->x + b->w / 2, b->y + (b->h - g_line_feed * 0.5f) / 2, 0.5f, 0.35f, b->w - 12,
				COL_TEXT, C2D_AlignCenter);
		}
	}
}

/* ---------- the deck ---------- */

typedef struct {
	float x, y, w, h;
} Rect;

static void grid_metrics(float *cell_w, float *cell_h, float *ox, float *oy, float *gap)
{
	int cols = V.cols > 0 ? V.cols : 5, rows = V.rows > 0 ? V.rows : 3;
	float g = cols > 6 || rows > 5 ? 2 : 4;
	float margin = 4;
	float cw = (BOT_W - 2 * margin - (cols - 1) * g) / cols;
	float ch = (SCR_H - 2 * margin - (rows - 1) * g) / rows;
	*cell_w = cw;
	*cell_h = ch;
	*ox = margin;
	*oy = margin;
	*gap = g;
}

static Rect widget_rect(const WidgetView *w)
{
	float cw, ch, ox, oy, g;
	grid_metrics(&cw, &ch, &ox, &oy, &g);
	Rect r;
	r.x = ox + w->x * (cw + g);
	r.y = oy + w->y * (ch + g);
	r.w = w->w * cw + (w->w - 1) * g;
	r.h = w->h * ch + (w->h - 1) * g;
	return r;
}

static int widget_at(int px, int py)
{
	for (int i = V.nwidgets - 1; i >= 0; i--) {
		const WidgetView *w = &V.widgets[i];
		if (w->x >= V.cols || w->y >= V.rows) continue;
		Rect r = widget_rect(w);
		if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) return i;
	}
	return -1;
}

static float slider_level_at(const WidgetView *w, int px, int py)
{
	Rect r = widget_rect(w);
	float level = w->vertical ? 1.0f - (py - r.y) / r.h : (px - r.x) / r.w;
	if (level < 0) level = 0;
	if (level > 1) level = 1;
	return level;
}

static void draw_tile(int idx)
{
	const WidgetView *w = &V.widgets[idx];
	if (w->x >= V.cols || w->y >= V.rows) return;
	Rect r = widget_rect(w);
	float radius = r.w < 40 || r.h < 40 ? 4 : 7;
	bool pressed = idx == g_touch_widget;

	uint32_t face = w->has_bg ? w->bg : 0x2A303CFF;
	if (!w->transparent) rrect(r.x, r.y, r.w, r.h, radius, w->has_bg ? rgba_to_c2d(w->bg) : COL_TILE);
	if (w->has_border) rrect_outline(r.x, r.y, r.w, r.h, 2, rgba_to_c2d(w->border));

	// Artwork across the box, contained, behind the labels.
	if (w->image >= 0 && w->image < MAX_IMAGES && g_tex_live[w->image]) {
		C2D_Image img = {&g_tex[w->image], &g_sub[w->image]};
		float iw = g_sub[w->image].width, ih = g_sub[w->image].height;
		float pad = r.w < 48 ? 2 : 4;
		float s = (r.w - 2 * pad) / iw;
		if ((r.h - 2 * pad) / ih < s) s = (r.h - 2 * pad) / ih;
		float dx = r.x + (r.w - iw * s) / 2, dy = r.y + (r.h - ih * s) / 2;
		if (w->has_tint) {
			C2D_ImageTint tint;
			C2D_PlainImageTint(&tint, rgba_to_c2d(w->tint | 0xFF), 1.0f);
			C2D_DrawImageAt(img, dx, dy, 0, &tint, s, s);
		} else {
			C2D_DrawImageAt(img, dx, dy, 0, NULL, s, s);
		}
	} else if (w->image >= 0) {
		// Still loading: a quiet placeholder dot.
		C2D_DrawCircleSolid(r.x + r.w / 2, r.y + r.h / 2, 0, 3, C2D_Color32(0xFF, 0xFF, 0xFF, 0x40));
	}

	// Level bar for sliders, bars and gauges.
	if (w->has_value) {
		float level = (pressed && g_touch_slider) ? g_slider_level : w->value;
		u32 track = C2D_Color32(0x00, 0x00, 0x00, 0x50);
		u32 fill = luminance(face) > 0.6f ? C2D_Color32(0x20, 0x20, 0x20, 0xFF) : COL_ACCENT;
		if (w->vertical) {
			float bw = r.w * 0.18f < 6 ? 6 : r.w * 0.18f;
			float bx = r.x + r.w - bw - 4, by = r.y + 4, bh = r.h - 8;
			rrect(bx, by, bw, bh, bw / 2, track);
			rrect(bx, by + bh * (1 - level), bw, bh * level, bw / 2, fill);
		} else {
			float bh = r.h * 0.12f < 5 ? 5 : r.h * 0.12f;
			float bx = r.x + 4, by = r.y + r.h - bh - 4, bw = r.w - 8;
			rrect(bx, by, bw, bh, bh / 2, track);
			rrect(bx, by, bw * level, bh, bh / 2, fill);
		}
	}

	// Labels: shrink to fit the tile, centred, with a legibility shadow.
	if (w->nlines > 0) {
		float pad = 3;
		float avail_w = r.w - 2 * pad;
		float avail_h = r.h - 2 * pad - (w->has_value && !w->vertical ? 8 : 0);
		float per_line = avail_h / w->nlines;
		float scale = per_line / g_line_feed;
		if (scale > 0.55f) scale = 0.55f;
		float line_h = g_line_feed * scale;
		float block = line_h * w->nlines;
		float y0 = r.y + pad + (avail_h - block) / 2;
		if (w->justify < 0) y0 = r.y + pad;
		else if (w->justify > 0) y0 = r.y + r.h - pad - block - (w->has_value && !w->vertical ? 8 : 0);
		u32 default_color = luminance(face) > 0.6f && !w->transparent ? C2D_Color32(0x10, 0x10, 0x10, 0xFF) : COL_TEXT;
		for (int i = 0; i < w->nlines; i++) {
			const TextLine *l = &w->lines[i];
			u32 color = l->has_color ? rgba_to_c2d(l->color) : (l->muted ? COL_MUTED : default_color);
			float cx = r.x + r.w / 2, cy = y0 + i * line_h;
			if (w->image >= 0) text_fit(l->text, cx + 1, cy + 1, scale, 0.28f, avail_w, COL_SHADOW, C2D_AlignCenter);
			text_fit(l->text, cx, cy, scale, 0.28f, avail_w, color, C2D_AlignCenter);
		}
	}

	if (pressed) rrect(r.x, r.y, r.w, r.h, radius, C2D_Color32(0xFF, 0xFF, 0xFF, 0x40));
}

static void draw_deck(void)
{
	float cw, ch, ox, oy, g;
	grid_metrics(&cw, &ch, &ox, &oy, &g);
	// Empty cells first, so the grid reads as a grid.
	for (int y = 0; y < V.rows; y++) {
		for (int x = 0; x < V.cols; x++) {
			rrect(ox + x * (cw + g), oy + y * (ch + g), cw, ch, cw < 40 ? 3 : 6, COL_EMPTY);
		}
	}
	for (int i = 0; i < V.nwidgets; i++) draw_tile(i);
	if (V.phase != PHASE_ONLINE) {
		C2D_DrawRectSolid(0, 0, 0, BOT_W, SCR_H, C2D_Color32(0, 0, 0, 0x90));
		text(V.status, BOT_W / 2, SCR_H / 2 - 10, 0.55f, COL_TEXT, C2D_AlignCenter);
	}
}

static void draw_bottom(void)
{
	u32 bg = V.has_folder_bg && deck_visible() ? rgba_to_c2d(V.folder_bg | 0xFF) : COL_BG;
	C2D_DrawRectSolid(0, 0, 0, BOT_W, SCR_H, bg);

	if (g_menu) {
		text("Menu", BOT_W / 2, 4, 0.55f, COL_TEXT, C2D_AlignCenter);
		draw_buttons();
		return;
	}
	if (deck_visible()) {
		draw_deck();
		return;
	}

	switch (V.phase) {
	case PHASE_IDLE:
		text("Macro Deck for 3DS", BOT_W / 2, 18, 0.75f, COL_TEXT, C2D_AlignCenter);
		text("Connect to Macro Deck 3 on your computer.", BOT_W / 2, 52, 0.45f, COL_MUTED, C2D_AlignCenter);
		break;
	case PHASE_DISCOVERING:
		text("Searching your network...", BOT_W / 2, 96, 0.6f, COL_TEXT, C2D_AlignCenter);
		text("Turn on \"Show on the local network\"", BOT_W / 2, 128, 0.42f, COL_MUTED, C2D_AlignCenter);
		text("in Macro Deck: Settings > Network.", BOT_W / 2, 144, 0.42f, COL_MUTED, C2D_AlignCenter);
		break;
	case PHASE_PICK_HOST:
		text(V.nfound ? "Pick your computer" : "No computers found", BOT_W / 2, 6, 0.55f, COL_TEXT, C2D_AlignCenter);
		if (!V.nfound) {
			text("Check that Macro Deck is running and that", BOT_W / 2, 70, 0.42f, COL_MUTED, C2D_AlignCenter);
			text("\"Show on the local network\" is on, or enter", BOT_W / 2, 86, 0.42f, COL_MUTED, C2D_AlignCenter);
			text("the address shown in Settings > Network.", BOT_W / 2, 102, 0.42f, COL_MUTED, C2D_AlignCenter);
		}
		break;
	case PHASE_NEED_PAIRING:
		text("Pair this 3DS", BOT_W / 2, 10, 0.7f, COL_TEXT, C2D_AlignCenter);
		text("On your computer, open Macro Deck and click", BOT_W / 2, 48, 0.42f, COL_MUTED, C2D_AlignCenter);
		text("the network icon (top right). Type the", BOT_W / 2, 64, 0.42f, COL_MUTED, C2D_AlignCenter);
		text("6-digit pairing code it shows.", BOT_W / 2, 80, 0.42f, COL_MUTED, C2D_AlignCenter);
		if (V.error[0]) text_fit(V.error, BOT_W / 2, 112, 0.45f, 0.32f, BOT_W - 20, COL_BAD, C2D_AlignCenter);
		break;
	case PHASE_CONNECTING:
	case PHASE_PAIRING:
	case PHASE_ONLINE: {
		text(V.status, BOT_W / 2, 100, 0.6f, COL_TEXT, C2D_AlignCenter);
		// A small spinner of dots.
		int t = (int)((osGetTime() / 120) % 8);
		for (int i = 0; i < 8; i++) {
			float a = i * 3.14159f / 4;
			u32 c = i == t ? COL_ACCENT : C2D_Color32(0xFF, 0xFF, 0xFF, 0x30);
			C2D_DrawCircleSolid(BOT_W / 2 + 14 * cosf(a), 150 + 14 * sinf(a), 0, 3, c);
		}
		if (V.phase == PHASE_ONLINE) text("This folder is empty.", BOT_W / 2, 180, 0.45f, COL_MUTED, C2D_AlignCenter);
		break;
	}
	case PHASE_RETRY_WAIT:
	case PHASE_ERROR:
		text(V.status, BOT_W / 2, 70, 0.6f, COL_TEXT, C2D_AlignCenter);
		if (V.error[0]) text_fit(V.error, BOT_W / 2, 104, 0.45f, 0.3f, BOT_W - 20, COL_BAD, C2D_AlignCenter);
		break;
	}
	draw_buttons();
}

/* ---------- top screen ---------- */

static void draw_battery(float x, float y)
{
	u8 level = 0, charging = 0;
	PTMU_GetBatteryLevel(&level);
	PTMU_GetBatteryChargeState(&charging);
	rrect_outline(x, y, 24, 12, 1, COL_MUTED);
	C2D_DrawRectSolid(x + 24, y + 3, 0, 2, 6, COL_MUTED);
	float fill = (level > 5 ? 5 : level) / 5.0f;
	u32 c = level <= 1 ? COL_BAD : (charging ? COL_GOOD : COL_TEXT);
	C2D_DrawRectSolid(x + 2, y + 2, 0, 20 * fill, 8, c);
}

static void draw_wifi(float x, float y)
{
	u8 strength = osGetWifiStrength();
	for (int i = 0; i < 3; i++) {
		float h = 4 + i * 3;
		C2D_DrawRectSolid(x + i * 5, y + 12 - h, 0, 3, h, i < strength ? COL_TEXT : C2D_Color32(0xFF, 0xFF, 0xFF, 0x40));
	}
}

static void phase_badge(u32 *color, const char **label)
{
	switch (V.phase) {
	case PHASE_ONLINE: *color = COL_GOOD; *label = "Connected"; break;
	case PHASE_CONNECTING: *color = COL_WARN; *label = "Connecting"; break;
	case PHASE_PAIRING: *color = COL_WARN; *label = "Pairing"; break;
	case PHASE_NEED_PAIRING: *color = COL_ACCENT; *label = "Needs pairing"; break;
	case PHASE_DISCOVERING: *color = COL_ACCENT; *label = "Searching"; break;
	case PHASE_PICK_HOST: *color = COL_ACCENT; *label = "Choose a computer"; break;
	case PHASE_RETRY_WAIT: *color = COL_BAD; *label = "Offline"; break;
	case PHASE_ERROR: *color = COL_BAD; *label = "Error"; break;
	default: *color = COL_MUTED; *label = "Not connected"; break;
	}
}

static void draw_top(void)
{
	C2D_DrawRectSolid(0, 0, 0, TOP_W, SCR_H, COL_BG);

	// Header
	C2D_DrawRectSolid(0, 0, 0, TOP_W, 26, COL_PANEL);
	text("Macro Deck", 10, 4, 0.55f, COL_TEXT, C2D_AlignLeft);
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char clock[16];
	snprintf(clock, sizeof clock, "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
	text(clock, TOP_W - 60, 5, 0.5f, COL_TEXT, C2D_AlignRight);
	draw_wifi(TOP_W - 54, 7);
	draw_battery(TOP_W - 34, 7);

	// Connection
	u32 badge;
	const char *label;
	phase_badge(&badge, &label);
	C2D_DrawCircleSolid(18, 44, 0, 5, badge);
	text(label, 30, 35, 0.5f, badge, C2D_AlignLeft);
	if (V.host_version[0]) {
		char v[48];
		snprintf(v, sizeof v, "Macro Deck %s", V.host_version);
		text(v, TOP_W - 10, 36, 0.42f, COL_MUTED, C2D_AlignRight);
	}

	const char *host = V.host_name[0] ? V.host_name : "No computer selected";
	text_fit(host, 12, 56, 0.8f, 0.45f, TOP_W - 24, COL_TEXT, C2D_AlignLeft);
	if (V.host_addr[0]) text(V.host_addr, 12, 84, 0.45f, COL_MUTED, C2D_AlignLeft);

	// Deck location
	C2D_DrawRectSolid(10, 106, 0, TOP_W - 20, 1, COL_PANEL_HI);
	if (V.phase == PHASE_ONLINE || V.nwidgets > 0) {
		text("", 12, 114, 0.6f, V.profile_count > 1 ? COL_TEXT : COL_PANEL_HI, C2D_AlignLeft);
		text("", TOP_W - 12, 114, 0.6f, V.profile_count > 1 ? COL_TEXT : COL_PANEL_HI, C2D_AlignRight);
		char profile[96];
		snprintf(profile, sizeof profile, "%s", V.profile_name);
		text_fit(profile, TOP_W / 2, 113, 0.65f, 0.4f, TOP_W - 80, COL_TEXT, C2D_AlignCenter);
		if (V.profile_count > 1) {
			// Page dots for profiles.
			int n = V.profile_count > 12 ? 12 : V.profile_count;
			float start = TOP_W / 2 - (n - 1) * 5;
			for (int i = 0; i < n; i++) {
				C2D_DrawCircleSolid(start + i * 10, 140, 0, 2.5f, i == V.profile_index ? COL_ACCENT : COL_PANEL_HI);
			}
		}
		text_fit(V.folder_path[0] ? V.folder_path : V.folder_name, TOP_W / 2, 150, 0.48f, 0.32f, TOP_W - 30, COL_MUTED,
			C2D_AlignCenter);
	} else {
		text_fit(V.status, TOP_W / 2, 122, 0.55f, 0.35f, TOP_W - 30, COL_MUTED, C2D_AlignCenter);
	}

	// Errors and toasts
	if (V.error[0] && V.phase != PHASE_ONLINE) {
		text_fit(V.error, TOP_W / 2, 176, 0.45f, 0.3f, TOP_W - 30, COL_BAD, C2D_AlignCenter);
	}
	if (V.toast[0] && osGetTime() < V.toast_until) {
		rrect(40, 172, TOP_W - 80, 24, 8, COL_PANEL_HI);
		text_fit(V.toast, TOP_W / 2, 176, 0.48f, 0.32f, TOP_W - 100, COL_TEXT, C2D_AlignCenter);
	}

	// Footer: controls and the console's own address
	C2D_DrawRectSolid(0, SCR_H - 22, 0, TOP_W, 22, COL_PANEL);
	text("START Menu    Back   SELECT Reconnect", 10, SCR_H - 19, 0.42f, COL_MUTED, C2D_AlignLeft);
	char ip[32], right[64];
	net_local_ip(ip, sizeof ip);
#ifndef APP_BUILD_STR
#define APP_BUILD_STR "dev"
#endif
	snprintf(right, sizeof right, "%s  ·  %s", ip[0] ? ip : "No Wi-Fi", APP_BUILD_STR);
	text(right, TOP_W - 10, SCR_H - 19, 0.42f, ip[0] ? COL_MUTED : COL_BAD, C2D_AlignRight);
}

/* ---------- input ---------- */

static bool in_button(const Button *b, int px, int py)
{
	return px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h;
}

// Menu and status buttons are touch-only: a tap runs the button it started and ended on.
static void handle_buttons_input(u32 down, u32 held, u32 up, touchPosition *touch)
{
	if (g_nbuttons == 0) return;
	if (down & KEY_TOUCH) {
		g_touch_button = false;
		for (int i = 0; i < g_nbuttons; i++) {
			if (in_button(&g_buttons[i], touch->px, touch->py)) {
				g_touch_button = true;
				g_touch_button_idx = i;
				g_touch_inside = true;
			}
		}
	} else if ((held & KEY_TOUCH) && g_touch_button && g_touch_button_idx < g_nbuttons) {
		g_touch_inside = in_button(&g_buttons[g_touch_button_idx], touch->px, touch->py);
	}
	if ((up & KEY_TOUCH) && g_touch_button) {
		g_touch_button = false;
		if (g_touch_inside && g_touch_button_idx < g_nbuttons) {
			Button b = g_buttons[g_touch_button_idx];
			run_action(b.action, b.arg);
		}
	}
}

static void handle_deck_input(u32 down, u32 held, u32 up, touchPosition *touch)
{
	if (down & KEY_TOUCH) {
		g_touch_widget = widget_at(touch->px, touch->py);
		g_touch_start = osGetTime();
		g_long_fired = false;
		g_touch_slider = false;
		if (g_touch_widget >= 0) {
			const WidgetView *w = &V.widgets[g_touch_widget];
			if (w->slider) {
				g_touch_slider = true;
				g_slider_level = slider_level_at(w, touch->px, touch->py);
			} else {
				client_cmd(CMD_PRESS, w->id, "press-start", 0);
			}
		}
	}
	if (g_touch_widget >= 0 && g_touch_widget < V.nwidgets) {
		const WidgetView *w = &V.widgets[g_touch_widget];
		if ((held & KEY_TOUCH) && g_touch_slider) {
			g_slider_level = slider_level_at(w, touch->px, touch->py);
		} else if ((held & KEY_TOUCH) && !g_long_fired && osGetTime() - g_touch_start >= LONG_PRESS_MS) {
			g_long_fired = true;
			client_cmd(CMD_PRESS, w->id, "long-press", 0);
		}
		if (up & KEY_TOUCH) {
			if (g_touch_slider) {
				client_cmd(CMD_SLIDER, w->id, NULL, (int)(g_slider_level * 10000));
			} else {
				if (!g_long_fired) client_cmd(CMD_PRESS, w->id, "press", 0);
				client_cmd(CMD_PRESS, w->id, "press-end", 0);
			}
			g_touch_widget = -1;
			g_touch_slider = false;
		}
	} else if (up & KEY_TOUCH) {
		g_touch_widget = -1;
	}

	if (down & KEY_B) client_cmd(CMD_BACK, NULL, NULL, 0);
	if (down & KEY_L) client_cmd(CMD_PROFILE_STEP, NULL, NULL, -1);
	if (down & KEY_R) client_cmd(CMD_PROFILE_STEP, NULL, NULL, 1);
}

static aptHookCookie g_apt_cookie;

static void on_apt(APT_HookType type, void *param)
{
	(void)param;
	// Sockets die while the console sleeps; keep that from flashing an error and reconnect on wake.
	if (type == APTHOOK_ONSLEEP) client_sleep_notice(false);
	else if (type == APTHOOK_ONWAKEUP) client_sleep_notice(true);
}

int main(int argc, char **argv)
{
	gfxInitDefault();
	ptmuInit();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(12288);
	C2D_Prepare();
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	g_dyn = C2D_TextBufNew(16384);
	CFNT_s *font = fontGetSystemFont();
	if (font) g_line_feed = fontGetInfo(font)->lineFeed;

	// Keep the network alive while the lid is closed would drain the battery; we just reconnect on wake.
	aptSetSleepAllowed(true);
	aptHook(&g_apt_cookie, on_apt, NULL);

	model_init();
	memset(&V, 0, sizeof V);
	V.cols = 5;
	V.rows = 3;

	bool net_ok = net_init();
	Config cfg;
	config_load(&cfg);
	config_save(&cfg); // persists the generated client id on first launch
	client_start(&cfg);
	if (net_ok) client_cmd(CMD_STARTUP, NULL, NULL, 0);
	else model_toast("Could not start networking. Is Wi-Fi on?");
	update_init(argc > 0 ? argv[0] : NULL);
	if (net_ok) update_check();

	while (aptMainLoop() && !g_quit) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
		touchPosition touch;
		hidTouchRead(&touch);

		snapshot();
		pump_images();
		update_status(&g_upd);
		if (g_upd.state != g_upd_prev) {
			if (g_upd.state == UPD_AVAILABLE && g_upd_prev == UPD_CHECKING) {
				model_toast("Update %s available: START > Install update", g_upd.latest);
			}
			if (g_upd.state == UPD_DONE) model_toast("Update installed. Close and reopen the app.");
			// Background checks fail quietly (no Wi-Fi, GitHub down); a failed install says why on the top screen.
			if (g_upd.state == UPD_FAILED && g_upd_prev != UPD_CHECKING) model_toast("%s", g_upd.message);
			g_upd_prev = g_upd.state;
		}
		build_layout();

		if (down & KEY_START) {
			g_menu = !g_menu;
			g_touch_button = false;
			g_touch_widget = -1;
			build_layout();
		} else if (down & KEY_SELECT) {
			client_cmd(CMD_RECONNECT, NULL, NULL, 0);
		} else if (deck_visible()) {
			handle_deck_input(down, held, up, &touch);
		} else {
			if ((down & KEY_B) && g_menu) g_menu = false;
			else if ((down & KEY_B) && V.phase == PHASE_PICK_HOST) client_cmd(CMD_RECONNECT, NULL, NULL, 0);
			else handle_buttons_input(down, held, up, &touch);
		}

		C2D_TextBufClear(g_dyn);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, COL_BG);
		C2D_SceneBegin(top);
		draw_top();
		C2D_TargetClear(bottom, COL_BG);
		C2D_SceneBegin(bottom);
		draw_bottom();
		C3D_FrameEnd(0);
	}

	client_stop();
	update_exit();
	aptUnhook(&g_apt_cookie);
	net_exit();
	for (int i = 0; i < MAX_IMAGES; i++) {
		if (g_tex_live[i]) C3D_TexDelete(&g_tex[i]);
	}
	C2D_TextBufDelete(g_dyn);
	C2D_Fini();
	C3D_Fini();
	ptmuExit();
	gfxExit();
	return 0;
}
