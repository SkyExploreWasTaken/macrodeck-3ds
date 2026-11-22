#include "client.h"
#include "cJSON.h"
#include "http.h"
#include "imgdecode.h"
#include "mdns.h"
#include "model.h"
#include "net.h"
#include "platform.h"
#include "uitree.h"
#include "ws.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_VERSION "1.0.0"
#define HTTP_TIMEOUT 12000
#define STALE_AFTER_MS 60000
#define MAX_PENDING 64
#define MAX_HISTORY 16
#define QUEUE_LEN 32

Model g_model;

/* ---------- model helpers (UI thread may call model_toast too) ---------- */

void model_init(void)
{
	memset(&g_model, 0, sizeof g_model);
	plat_mutex_init(&g_model.lock);
	g_model.cols = 5;
	g_model.rows = 3;
}

void model_touch(void) { g_model.version++; }

void model_toast(const char *fmt, ...)
{
	plat_mutex_lock(&g_model.lock);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_model.toast, sizeof g_model.toast, fmt, ap);
	va_end(ap);
	g_model.toast_until = plat_ms() + 3500;
	model_touch();
	plat_mutex_unlock(&g_model.lock);
}

/* ---------- worker state ---------- */

typedef struct {
	char id[40];
	char type[40];
	int x, y, w, h;
	char session[72];
	cJSON *tree;     // root node of the session's tree
	int revision;
	bool opening;
	int failures;
	uint64_t retry_at;
} WWidget;

typedef enum { P_REGISTER, P_OPEN, P_ATTACH, P_EVENT, P_OTHER } PendingKind;

typedef struct {
	bool used;
	char id[48];
	PendingKind kind;
	char widget_id[40];
	char session[72];
	uint64_t sent_at;
} Pending;

typedef struct {
	CommandType type;
	char a[128];
	char b[96];
	int n;
} Command;

static struct {
	Config cfg;
	plat_mutex cfg_lock;
	plat_thread *thread;
	volatile bool running;

	Command queue[QUEUE_LEN];
	int q_head, q_tail;
	plat_mutex q_lock;

	Phase phase;
	WsConn ws;
	bool ws_up;
	bool welcomed;
	bool registered;
	uint64_t retry_at;
	uint64_t reload_at;
	bool reload_profiles;

	cJSON *profiles;  // sorted array
	int profile_idx;
	cJSON *folders;   // array for the current profile
	char folder_id[48];
	char history[MAX_HISTORY][48];
	int nhistory;
	char last_reported[48];
	char nav_token[96];

	WWidget w[MAX_WIDGETS];
	int nw;
	Pending pending[MAX_PENDING];
	unsigned seq;
} S;

/* ---------- small utilities ---------- */

static void set_phase(Phase p, const char *fmt, ...)
{
	S.phase = p;
	plat_mutex_lock(&g_model.lock);
	g_model.phase = p;
	if (fmt) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(g_model.status, sizeof g_model.status, fmt, ap);
		va_end(ap);
	}
	if (p != PHASE_ERROR && p != PHASE_NEED_PAIRING && p != PHASE_RETRY_WAIT) g_model.error[0] = 0;
	model_touch();
	plat_mutex_unlock(&g_model.lock);
}

static void set_error(const char *fmt, ...)
{
	plat_mutex_lock(&g_model.lock);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_model.error, sizeof g_model.error, fmt, ap);
	va_end(ap);
	model_touch();
	plat_mutex_unlock(&g_model.lock);
}

static void save_config(void)
{
	plat_mutex_lock(&S.cfg_lock);
	config_save(&S.cfg);
	plat_mutex_unlock(&S.cfg_lock);
}

static void publish_host(void)
{
	plat_mutex_lock(&g_model.lock);
	snprintf(g_model.host_name, sizeof g_model.host_name, "%s", S.cfg.host_name[0] ? S.cfg.host_name : S.cfg.host);
	if (S.cfg.host[0]) snprintf(g_model.host_addr, sizeof g_model.host_addr, "%s:%d", S.cfg.host, S.cfg.port);
	else g_model.host_addr[0] = 0;
	g_model.mode = S.cfg.mode;
	model_touch();
	plat_mutex_unlock(&g_model.lock);
}

static const char *jstr(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int jint(const cJSON *o, const char *k, int def)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsNumber(v) ? v->valueint : def;
}

static void url_encode(const char *in, char *out, size_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *in && o + 4 < len; in++) {
		unsigned char c = (unsigned char)*in;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
			c == '.' || c == '~') {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = 0;
}

/* ---------- HTTP with the session token ---------- */

static bool apply_token_response(const HttpResponse *rs)
{
	cJSON *j = cJSON_Parse(rs->body ? rs->body : "");
	if (!j) return false;
	const char *token = jstr(j, "accessToken");
	if (!token) {
		cJSON_Delete(j);
		return false;
	}
	plat_mutex_lock(&S.cfg_lock);
	snprintf(S.cfg.access_token, sizeof S.cfg.access_token, "%s", token);
	if (rs->refresh_cookie_name[0]) {
		snprintf(S.cfg.refresh_cookie_name, sizeof S.cfg.refresh_cookie_name, "%s", rs->refresh_cookie_name);
		snprintf(S.cfg.refresh_cookie_value, sizeof S.cfg.refresh_cookie_value, "%s", rs->refresh_cookie_value);
	}
	const cJSON *device = cJSON_GetObjectItemCaseSensitive(j, "device");
	if (cJSON_IsObject(device)) {
		const char *id = jstr(device, "deviceId");
		const char *secret = jstr(device, "deviceSecret");
		const char *startup = jstr(device, "startupProfileId");
		if (id) snprintf(S.cfg.device_id, sizeof S.cfg.device_id, "%s", id);
		// Only sent when freshly minted; the stored one stands otherwise.
		if (secret && *secret) snprintf(S.cfg.device_secret, sizeof S.cfg.device_secret, "%s", secret);
		snprintf(S.cfg.startup_profile_id, sizeof S.cfg.startup_profile_id, "%s", startup ? startup : "");
	}
	config_save(&S.cfg);
	plat_mutex_unlock(&S.cfg_lock);
	cJSON_Delete(j);
	return true;
}

// 1 refreshed, 0 the host refused (sign in again), -1 unreachable.
static int try_refresh(void)
{
	if (!S.cfg.refresh_cookie_name[0] || !S.cfg.refresh_cookie_value[0]) return 0;
	char cookie[480];
	snprintf(cookie, sizeof cookie, "%s=%s", S.cfg.refresh_cookie_name, S.cfg.refresh_cookie_value);
	HttpRequest rq = {.method = "POST", .path = "/api/auth/refresh", .cookie = cookie, .body = "", .body_len = 0};
	HttpResponse rs;
	http_do(S.cfg.host, S.cfg.port, &rq, &rs, 30000);
	int result;
	if (rs.status < 0) result = -1;
	else if (rs.status >= 200 && rs.status < 300) result = apply_token_response(&rs) ? 1 : -1;
	else result = 0;
	http_free(&rs);
	return result;
}

// Performs an authenticated call, refreshing the token once on 401. status -401 means "pair again".
static void api(const char *method, const char *path, const char *body, const char *accept, const char *extra,
	HttpResponse *rs)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		HttpRequest rq = {
			.method = method,
			.path = path,
			.bearer = S.cfg.access_token,
			.accept = accept,
			.extra_header = extra,
			.body = body,
			.body_len = body ? strlen(body) : 0,
		};
		http_do(S.cfg.host, S.cfg.port, &rq, rs, HTTP_TIMEOUT);
		if (rs->status != 401 || attempt == 1) return;
		http_free(rs);
		int r = try_refresh();
		if (r == 0) {
			rs->status = -401;
			return;
		}
		if (r < 0) {
			rs->status = -1;
			return;
		}
	}
}

/* ---------- WebSocket envelopes ---------- */

static Pending *pending_add(PendingKind kind, const char *widget_id, const char *session)
{
	for (int i = 0; i < MAX_PENDING; i++) {
		if (!S.pending[i].used) {
			Pending *p = &S.pending[i];
			memset(p, 0, sizeof *p);
			p->used = true;
			p->kind = kind;
			uint8_t rnd[4];
			plat_random(rnd, sizeof rnd);
			snprintf(p->id, sizeof p->id, "%u-%02x%02x%02x%02x", ++S.seq, rnd[0], rnd[1], rnd[2], rnd[3]);
			if (widget_id) snprintf(p->widget_id, sizeof p->widget_id, "%s", widget_id);
			if (session) snprintf(p->session, sizeof p->session, "%s", session);
			p->sent_at = plat_ms();
			return p;
		}
	}
	return NULL;
}

static void send_envelope(cJSON *env)
{
	char *text = cJSON_PrintUnformatted(env);
	if (text) {
		if (ws_send_text(&S.ws, text, strlen(text)) != 0) S.ws_up = false;
		free(text);
	}
}

// Takes ownership of payload.
static bool ws_request(const char *type, cJSON *payload, PendingKind kind, const char *widget_id, const char *session)
{
	if (!S.ws_up) {
		cJSON_Delete(payload);
		return false;
	}
	Pending *p = pending_add(kind, widget_id, session);
	if (!p) {
		cJSON_Delete(payload);
		return false;
	}
	cJSON *env = cJSON_CreateObject();
	cJSON_AddNumberToObject(env, "protocolVersion", 1);
	cJSON_AddStringToObject(env, "kind", "request");
	cJSON_AddStringToObject(env, "type", type);
	cJSON_AddStringToObject(env, "id", p->id);
	if (payload) cJSON_AddItemToObject(env, "payload", payload);
	send_envelope(env);
	cJSON_Delete(env);
	return true;
}

static void ws_simple(const char *kind, const char *correlation)
{
	cJSON *env = cJSON_CreateObject();
	cJSON_AddNumberToObject(env, "protocolVersion", 1);
	cJSON_AddStringToObject(env, "kind", kind);
	if (correlation) cJSON_AddStringToObject(env, "correlationId", correlation);
	send_envelope(env);
	cJSON_Delete(env);
}

/* ---------- images ---------- */

static int resolve_image(const cJSON *resource, void *ctx)
{
	(void)ctx;
	const char *id = jstr(resource, "resourceId");
	if (!id || !*id) return -1;
	const char *hash = jstr(resource, "contentHash");
	char key[200];
	snprintf(key, sizeof key, "%s|%s", id, hash ? hash : "");
	int free_slot = -1;
	for (int i = 0; i < MAX_IMAGES; i++) {
		ImageSlot *s = &g_model.images[i];
		if (s->state == IMG_EMPTY) {
			if (free_slot < 0) free_slot = i;
			continue;
		}
		if (strcmp(s->key, key) == 0) return s->state == IMG_FAILED ? -1 : i;
	}
	if (free_slot < 0) return -1;
	ImageSlot *s = &g_model.images[free_slot];
	snprintf(s->key, sizeof s->key, "%s", key);
	char enc_id[160], enc_hash[100];
	url_encode(id, enc_id, sizeof enc_id);
	url_encode(hash ? hash : "", enc_hash, sizeof enc_hash);
	if (hash) snprintf(s->url, sizeof s->url, "/api/ui/resources/%s?v=%s&size=%d", enc_id, enc_hash, IMAGE_TEX);
	else snprintf(s->url, sizeof s->url, "/api/ui/resources/%s?size=%d", enc_id, IMAGE_TEX);
	s->state = IMG_QUEUED;
	return free_slot;
}

static bool fetch_one_image(void)
{
	char url[300];
	int slot = -1;
	plat_mutex_lock(&g_model.lock);
	for (int i = 0; i < MAX_IMAGES; i++) {
		if (g_model.images[i].state == IMG_QUEUED) {
			slot = i;
			snprintf(url, sizeof url, "%s", g_model.images[i].url);
			break;
		}
	}
	plat_mutex_unlock(&g_model.lock);
	if (slot < 0) return false;

	HttpResponse rs;
	api("GET", url, NULL, "image/png,image/jpeg,image/gif,image/webp,*/*;q=0.5", NULL, &rs);
	uint8_t *rgba = NULL;
	int w = 0, h = 0;
	if (rs.status == 200 && rs.body_len) rgba = image_decode_fit((const uint8_t *)rs.body, rs.body_len, IMAGE_TEX, &w, &h);
	http_free(&rs);

	plat_mutex_lock(&g_model.lock);
	ImageSlot *s = &g_model.images[slot];
	if (s->state == IMG_QUEUED) {
		if (rgba) {
			s->rgba = rgba;
			s->w = w;
			s->h = h;
			s->state = IMG_DECODED;
			rgba = NULL;
		} else {
			s->state = IMG_FAILED;
		}
		model_touch();
	}
	plat_mutex_unlock(&g_model.lock);
	free(rgba);
	return true;
}

/* ---------- deck model ---------- */

static cJSON *current_profile(void)
{
	return cJSON_GetArrayItem(S.profiles, S.profile_idx);
}

static cJSON *folder_by_id(const char *id)
{
	cJSON *f;
	cJSON_ArrayForEach(f, S.folders) {
		const char *fid = jstr(f, "id");
		if (fid && strcmp(fid, id) == 0) return f;
	}
	return NULL;
}

static WWidget *widget_by_id(const char *id)
{
	for (int i = 0; i < S.nw; i++) {
		if (strcmp(S.w[i].id, id) == 0) return &S.w[i];
	}
	return NULL;
}

static WWidget *widget_by_session(const char *session)
{
	for (int i = 0; i < S.nw; i++) {
		if (S.w[i].session[0] && strcmp(S.w[i].session, session) == 0) return &S.w[i];
	}
	return NULL;
}

static void folder_path(const char *id, char *out, size_t len)
{
	// Walks up the parents: "Root / Streaming / Scenes".
	const char *chain[8];
	int n = 0;
	cJSON *f = folder_by_id(id);
	while (f && n < 8) {
		chain[n++] = jstr(f, "name");
		const char *parent = jstr(f, "parentId");
		f = parent ? folder_by_id(parent) : NULL;
	}
	size_t o = 0;
	out[0] = 0;
	for (int i = n - 1; i >= 0; i--) {
		if (o + 4 >= len) break;
		o += (size_t)snprintf(out + o, len - o, "%s%s", chain[i] ? chain[i] : "?", i ? " / " : "");
	}
}

static void publish_deck(void)
{
	cJSON *profile = current_profile();
	cJSON *folder = folder_by_id(S.folder_id);

	plat_mutex_lock(&g_model.lock);
	snprintf(g_model.profile_name, sizeof g_model.profile_name, "%s", profile && jstr(profile, "name") ? jstr(profile, "name") : "");
	g_model.profile_index = S.profile_idx;
	g_model.profile_count = cJSON_GetArraySize(S.profiles);
	snprintf(g_model.folder_name, sizeof g_model.folder_name, "%s", folder && jstr(folder, "name") ? jstr(folder, "name") : "");
	folder_path(S.folder_id, g_model.folder_path, sizeof g_model.folder_path);
	int cols = folder ? jint(folder, "columns", 0) : 0;
	int rows = folder ? jint(folder, "rows", 0) : 0;
	if (cols <= 0 && profile) cols = jint(profile, "defaultColumns", 0);
	if (rows <= 0 && profile) rows = jint(profile, "defaultRows", 0);
	g_model.cols = cols > 0 ? cols : 5;
	g_model.rows = rows > 0 ? rows : 3;
	g_model.has_folder_bg = folder && parse_color(jstr(folder, "backgroundColor"), &g_model.folder_bg);
	g_model.can_go_back = S.nhistory > 0 || (folder && jstr(folder, "parentId"));

	g_model.nwidgets = S.nw;
	for (int i = 0; i < S.nw; i++) {
		WWidget *w = &S.w[i];
		WidgetView *v = &g_model.widgets[i];
		snprintf(v->id, sizeof v->id, "%s", w->id);
		snprintf(v->type, sizeof v->type, "%s", w->type);
		v->x = w->x;
		v->y = w->y;
		v->w = w->w;
		v->h = w->h;
		tree_extract(w->tree, v, resolve_image, NULL);
	}
	model_touch();
	plat_mutex_unlock(&g_model.lock);
}

static void publish_widget(WWidget *w)
{
	int i = (int)(w - S.w);
	plat_mutex_lock(&g_model.lock);
	if (i < g_model.nwidgets && strcmp(g_model.widgets[i].id, w->id) == 0) {
		tree_extract(w->tree, &g_model.widgets[i], resolve_image, NULL);
		model_touch();
	}
	plat_mutex_unlock(&g_model.lock);
}

static void close_session(WWidget *w)
{
	if (w->session[0] && S.ws_up) {
		cJSON *args = cJSON_CreateArray();
		cJSON_AddItemToArray(args, cJSON_CreateString(w->session));
		ws_request("CloseUiSession", args, P_OTHER, NULL, NULL);
	}
	w->session[0] = 0;
	cJSON_Delete(w->tree);
	w->tree = NULL;
	w->revision = 0;
	w->opening = false;
}

static void open_session(WWidget *w)
{
	if (!S.ws_up || !S.registered || w->opening || w->session[0]) return;
	if (w->retry_at && plat_ms() < w->retry_at) return;
	cJSON *args = cJSON_CreateArray();
	cJSON *req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "widgetId", w->id);
	cJSON_AddItemToArray(args, req);
	if (ws_request("OpenWidgetUiSession", args, P_OPEN, w->id, NULL)) w->opening = true;
}

static void open_missing_sessions(void)
{
	for (int i = 0; i < S.nw; i++) open_session(&S.w[i]);
}

static void report_folder(bool resync)
{
	if (!S.ws_up || !S.registered || !S.folder_id[0]) return;
	if (!resync && strcmp(S.last_reported, S.folder_id) == 0) return;
	snprintf(S.last_reported, sizeof S.last_reported, "%s", S.folder_id);
	cJSON *req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "folderId", S.folder_id);
	cJSON_AddStringToObject(req, "clientId", S.cfg.client_id);
	if (S.nav_token[0]) cJSON_AddStringToObject(req, "navigationToken", S.nav_token);
	cJSON_AddBoolToObject(req, "isResync", resync);
	S.nav_token[0] = 0;
	ws_request("ReportFolderChanged", req, P_OTHER, NULL, NULL);
}

// Rebuilds the widget list from the folder, keeping live sessions of widgets that are still there.
static void show_folder(const char *id, bool push_history)
{
	cJSON *folder = folder_by_id(id);
	if (!folder) return;
	bool same = strcmp(S.folder_id, id) == 0;
	if (push_history && !same && S.folder_id[0]) {
		if (S.nhistory == MAX_HISTORY) {
			memmove(S.history[0], S.history[1], sizeof S.history[0] * (MAX_HISTORY - 1));
			S.nhistory--;
		}
		snprintf(S.history[S.nhistory++], sizeof S.history[0], "%s", S.folder_id);
	}
	snprintf(S.folder_id, sizeof S.folder_id, "%s", id);

	static WWidget old[MAX_WIDGETS];
	int nold = S.nw;
	memcpy(old, S.w, sizeof(WWidget) * (size_t)nold);
	S.nw = 0;

	const cJSON *widget;
	cJSON_ArrayForEach(widget, cJSON_GetObjectItemCaseSensitive(folder, "widgets")) {
		if (S.nw >= MAX_WIDGETS) break;
		const char *wid = jstr(widget, "id");
		if (!wid) continue;
		WWidget *w = &S.w[S.nw++];
		memset(w, 0, sizeof *w);
		for (int i = 0; i < nold; i++) {
			if (old[i].id[0] && strcmp(old[i].id, wid) == 0) {
				*w = old[i];
				old[i].id[0] = 0; // taken over
				break;
			}
		}
		snprintf(w->id, sizeof w->id, "%s", wid);
		snprintf(w->type, sizeof w->type, "%s", jstr(widget, "type") ? jstr(widget, "type") : "");
		w->x = jint(widget, "positionX", 0);
		w->y = jint(widget, "positionY", 0);
		w->w = jint(widget, "width", 1);
		w->h = jint(widget, "height", 1);
		if (w->w < 1) w->w = 1;
		if (w->h < 1) w->h = 1;
	}
	for (int i = 0; i < nold; i++) {
		if (old[i].id[0]) close_session(&old[i]);
	}
	publish_deck();
	open_missing_sessions();
	report_folder(false);
}

static void close_all_sessions(void)
{
	for (int i = 0; i < S.nw; i++) close_session(&S.w[i]);
}

static void sort_profiles(void)
{
	// Insertion sort by "order" (a handful of profiles at most).
	int n = cJSON_GetArraySize(S.profiles);
	cJSON *sorted = cJSON_CreateArray();
	for (int k = 0; k < n; k++) {
		cJSON *best = NULL;
		cJSON *p;
		cJSON_ArrayForEach(p, S.profiles) {
			if (!best || jint(p, "order", 0) < jint(best, "order", 0)) best = p;
		}
		if (!best) break;
		cJSON_AddItemToArray(sorted, cJSON_DetachItemViaPointer(S.profiles, best));
	}
	cJSON_Delete(S.profiles);
	S.profiles = sorted;
}

static int profile_index_of(const char *id)
{
	int i = 0;
	cJSON *p;
	cJSON_ArrayForEach(p, S.profiles) {
		const char *pid = jstr(p, "id");
		if (id && pid && strcmp(pid, id) == 0) return i;
		i++;
	}
	return -1;
}

// Returns false (and sets the phase) when the host refused or could not be reached.
static bool handle_api_failure(const HttpResponse *rs, const char *what)
{
	if (rs->status == -401) {
		plat_mutex_lock(&S.cfg_lock);
		config_forget_session(&S.cfg);
		plat_mutex_unlock(&S.cfg_lock);
		save_config();
		set_phase(PHASE_NEED_PAIRING, "Pair this 3DS with Macro Deck");
		return false;
	}
	if (rs->status < 0) {
		set_error("Lost the connection to %s (%s).", S.cfg.host, net_error());
	} else {
		set_error("The host answered %d while loading %s.", rs->status, what);
	}
	S.retry_at = plat_ms() + 4000;
	set_phase(PHASE_RETRY_WAIT, "Retrying shortly...");
	return false;
}

static bool load_profiles(void)
{
	HttpResponse rs;
	api("GET", "/api/profiles", NULL, NULL, NULL, &rs);
	if (rs.status != 200) {
		bool r = handle_api_failure(&rs, "profiles");
		http_free(&rs);
		return r;
	}
	cJSON *j = cJSON_Parse(rs.body);
	http_free(&rs);
	cJSON *list = cJSON_DetachItemFromObjectCaseSensitive(j, "profiles");
	cJSON_Delete(j);
	if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) {
		cJSON_Delete(list);
		set_error("Macro Deck has no profiles yet.");
		S.retry_at = plat_ms() + 10000;
		set_phase(PHASE_RETRY_WAIT, "Waiting for a profile...");
		return false;
	}
	cJSON_Delete(S.profiles);
	S.profiles = list;
	sort_profiles();
	return true;
}

static bool load_folders(const char *profile_id)
{
	char path[160], enc[100];
	url_encode(profile_id, enc, sizeof enc);
	snprintf(path, sizeof path, "/api/folders?profileId=%s", enc);
	HttpResponse rs;
	api("GET", path, NULL, NULL, NULL, &rs);
	if (rs.status != 200) {
		bool r = handle_api_failure(&rs, "folders");
		http_free(&rs);
		return r;
	}
	cJSON *j = cJSON_Parse(rs.body);
	http_free(&rs);
	cJSON *list = cJSON_DetachItemFromObjectCaseSensitive(j, "folders");
	cJSON_Delete(j);
	if (!cJSON_IsArray(list)) {
		cJSON_Delete(list);
		return false;
	}
	cJSON_Delete(S.folders);
	S.folders = list;
	return true;
}

static const char *start_folder(void)
{
	cJSON *f;
	cJSON_ArrayForEach(f, S.folders) {
		if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "isDefault"))) return jstr(f, "id");
	}
	f = cJSON_GetArrayItem(S.folders, 0);
	return f ? jstr(f, "id") : NULL;
}

static bool switch_profile(int idx, const char *folder_id)
{
	cJSON *p = cJSON_GetArrayItem(S.profiles, idx);
	if (!p) return false;
	const char *pid = jstr(p, "id");
	if (!pid || !load_folders(pid)) return false;
	S.profile_idx = idx;
	close_all_sessions();
	S.nw = 0;
	S.nhistory = 0;
	S.folder_id[0] = 0;
	plat_mutex_lock(&S.cfg_lock);
	snprintf(S.cfg.last_profile_id, sizeof S.cfg.last_profile_id, "%s", pid);
	plat_mutex_unlock(&S.cfg_lock);
	save_config();
	const char *target = folder_id && folder_by_id(folder_id) ? folder_id : start_folder();
	if (target) {
		char id[48];
		snprintf(id, sizeof id, "%s", target);
		show_folder(id, false);
	} else {
		publish_deck();
	}
	return true;
}

/* ---------- connection lifecycle ---------- */

static void drop_socket(void)
{
	if (S.ws.fd >= 0 || S.ws_up) ws_close(&S.ws);
	S.ws.fd = -1;
	S.ws_up = false;
	S.welcomed = false;
	S.registered = false;
	S.last_reported[0] = 0;
	memset(S.pending, 0, sizeof S.pending);
	// Sessions die with the connection; trees stay on screen until the new ones arrive.
	for (int i = 0; i < S.nw; i++) {
		S.w[i].session[0] = 0;
		S.w[i].opening = false;
		S.w[i].failures = 0;
		S.w[i].retry_at = 0;
	}
}

static bool open_socket(void)
{
	HttpResponse rs;
	api("POST", "/api/ui-websocket/tickets", "{}", NULL, "X-MacroDeck-Ui-Protocol: 1", &rs);
	if (rs.status != 200) {
		bool r = handle_api_failure(&rs, "a connection ticket");
		http_free(&rs);
		return r;
	}
	cJSON *j = cJSON_Parse(rs.body);
	http_free(&rs);
	const char *ticket = jstr(j, "value");
	if (!ticket) {
		cJSON_Delete(j);
		set_error("The host sent no connection ticket.");
		S.retry_at = plat_ms() + 4000;
		set_phase(PHASE_RETRY_WAIT, "Retrying shortly...");
		return false;
	}
	char enc[400], path[460];
	url_encode(ticket, enc, sizeof enc);
	snprintf(path, sizeof path, "/ws/ui?ticket=%s", enc);
	cJSON_Delete(j);

	if (ws_connect(&S.ws, S.cfg.host, S.cfg.port, path, "macrodeck.ui.v1", 8000) != 0) {
		set_error("Could not open the live connection.");
		S.retry_at = plat_ms() + 4000;
		set_phase(PHASE_RETRY_WAIT, "Retrying shortly...");
		return false;
	}
	S.ws_up = true;
	ws_simple("hello", NULL);
	return true;
}

static void handle_message(char *text);

// Full sign-in-to-live sequence. Leaves the phase at ONLINE, NEED_PAIRING or RETRY_WAIT.
static void do_connect(void)
{
	drop_socket();
	publish_host();
	if (!S.cfg.host[0]) {
		set_phase(PHASE_IDLE, "Pick a computer to connect to");
		return;
	}
	set_phase(PHASE_CONNECTING, "Contacting %s...", S.cfg.host_name[0] ? S.cfg.host_name : S.cfg.host);

	HttpRequest rq = {.method = "GET", .path = "/api/auth/status"};
	HttpResponse rs;
	http_do(S.cfg.host, S.cfg.port, &rq, &rs, 6000);
	if (rs.status != 200) {
		if (rs.status < 0) set_error("Can't reach %s:%d (%s). Is Macro Deck running?", S.cfg.host, S.cfg.port, net_error());
		else set_error("%s:%d is not a Macro Deck 3 host.", S.cfg.host, S.cfg.port);
		http_free(&rs);
		S.retry_at = plat_ms() + 5000;
		set_phase(PHASE_RETRY_WAIT, "Retrying shortly...");
		return;
	}
	cJSON *status = cJSON_Parse(rs.body);
	http_free(&rs);
	const char *version = jstr(status, "version");
	plat_mutex_lock(&g_model.lock);
	snprintf(g_model.host_version, sizeof g_model.host_version, "%s", version ? version : "");
	plat_mutex_unlock(&g_model.lock);
	cJSON_Delete(status);

	if (!S.cfg.access_token[0]) {
		set_phase(PHASE_NEED_PAIRING, "Pair this 3DS with Macro Deck");
		return;
	}

	set_phase(PHASE_CONNECTING, "Loading your deck...");
	if (!load_profiles()) return;

	int idx = profile_index_of(S.cfg.last_profile_id);
	if (idx < 0) idx = profile_index_of(S.cfg.startup_profile_id);
	if (idx < 0) idx = 0;
	const char *pid = jstr(cJSON_GetArrayItem(S.profiles, idx), "id");
	if (!pid || !load_folders(pid)) return;
	S.profile_idx = idx;

	set_phase(PHASE_CONNECTING, "Going live...");
	if (!open_socket()) return;

	// Wait for the host's welcome, answering anything else that arrives meanwhile.
	uint64_t deadline = plat_ms() + 15000;
	while (!S.welcomed && plat_ms() < deadline && S.ws_up) {
		char *msg;
		size_t len;
		int r = ws_poll(&S.ws, &msg, &len);
		if (r < 0) {
			S.ws_up = false;
			break;
		}
		if (r == 1) handle_message(msg);
		else plat_sleep_ms(10);
	}
	if (!S.welcomed) {
		drop_socket();
		set_error("The host did not finish the handshake.");
		S.retry_at = plat_ms() + 4000;
		set_phase(PHASE_RETRY_WAIT, "Retrying shortly...");
		return;
	}

	cJSON *args = cJSON_CreateArray();
	cJSON_AddItemToArray(args, cJSON_CreateString(S.cfg.client_id));
	cJSON_AddItemToArray(args, cJSON_CreateArray());
	ws_request("RegisterClient", args, P_REGISTER, NULL, NULL);

	// Keep the folder we were on across reconnects when it still exists.
	char keep[48];
	snprintf(keep, sizeof keep, "%s", S.folder_id);
	const char *target = keep[0] && folder_by_id(keep) ? keep : start_folder();
	set_phase(PHASE_ONLINE, "Connected");
	if (target) {
		char id[48];
		snprintf(id, sizeof id, "%s", target);
		S.folder_id[0] = 0;
		show_folder(id, false);
	} else {
		publish_deck();
	}
}

static void do_pair(const char *code)
{
	set_phase(PHASE_PAIRING, "Pairing...");
	cJSON *body = cJSON_CreateObject();
	cJSON_AddStringToObject(body, "token", code);
	cJSON *device = cJSON_AddObjectToObject(body, "device");
	if (S.cfg.device_id[0]) cJSON_AddStringToObject(device, "deviceId", S.cfg.device_id);
	if (S.cfg.device_secret[0]) cJSON_AddStringToObject(device, "deviceSecret", S.cfg.device_secret);
	cJSON_AddStringToObject(device, "clientType", "native");
	cJSON_AddStringToObject(device, "proposedName", "Nintendo 3DS");
	cJSON_AddStringToObject(device, "platform", "Nintendo 3DS");
	cJSON_AddStringToObject(device, "formFactor", "phone");
	cJSON_AddStringToObject(device, "appVersion", APP_VERSION);
	char *text = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);

	HttpRequest rq = {
		.method = "POST",
		.path = "/api/auth/device-enrollment/redeem",
		.body = text,
		.body_len = strlen(text),
	};
	HttpResponse rs;
	http_do(S.cfg.host, S.cfg.port, &rq, &rs, HTTP_TIMEOUT);
	free(text);

	if (rs.status >= 200 && rs.status < 300 && apply_token_response(&rs)) {
		http_free(&rs);
		model_toast("Paired with %s", S.cfg.host_name[0] ? S.cfg.host_name : S.cfg.host);
		do_connect();
		return;
	}
	if (rs.status == 429) set_error("Too many wrong codes. Wait a moment, then try the new code.");
	else if (rs.status == 401 || rs.status == 400) set_error("That code is wrong or has expired.");
	else if (rs.status < 0) set_error("Can't reach %s:%d while pairing (%s).", S.cfg.host, S.cfg.port, net_error());
	else set_error("Pairing failed (HTTP %d).", rs.status);
	http_free(&rs);
	set_phase(PHASE_NEED_PAIRING, "Pair this 3DS with Macro Deck");
}

static void do_discover(bool then_connect)
{
	set_phase(PHASE_DISCOVERING, "Looking for Macro Deck on your network...");
	MdnsHost found[MAX_FOUND];
	int n = mdns_discover(found, MAX_FOUND, 3000);
	plat_mutex_lock(&g_model.lock);
	memcpy(g_model.found, found, sizeof(MdnsHost) * (size_t)n);
	g_model.nfound = n;
	model_touch();
	plat_mutex_unlock(&g_model.lock);

	if (then_connect) {
		// Prefer the computer we paired with before, even if its address changed.
		for (int i = 0; i < n; i++) {
			bool same_name = S.cfg.host_name[0] && strcmp(found[i].name, S.cfg.host_name) == 0;
			bool same_addr = strcmp(found[i].ip, S.cfg.host) == 0 && found[i].port == S.cfg.port;
			if (same_name || same_addr) {
				plat_mutex_lock(&S.cfg_lock);
				snprintf(S.cfg.host, sizeof S.cfg.host, "%s", found[i].ip);
				S.cfg.port = found[i].port;
				snprintf(S.cfg.host_name, sizeof S.cfg.host_name, "%s", found[i].name);
				plat_mutex_unlock(&S.cfg_lock);
				save_config();
				do_connect();
				return;
			}
		}
		if (n == 1 && !S.cfg.host[0]) {
			plat_mutex_lock(&S.cfg_lock);
			snprintf(S.cfg.host, sizeof S.cfg.host, "%s", found[0].ip);
			S.cfg.port = found[0].port;
			snprintf(S.cfg.host_name, sizeof S.cfg.host_name, "%s", found[0].name);
			plat_mutex_unlock(&S.cfg_lock);
			save_config();
			do_connect();
			return;
		}
		if (n == 0 && S.cfg.host[0]) {
			do_connect();
			return;
		}
	}
	set_phase(PHASE_PICK_HOST, n ? "Pick your computer" : "No computers found");
}

/* ---------- inbound messages ---------- */

static void on_tree(const cJSON *body)
{
	const char *session = jstr(body, "sessionId");
	const cJSON *rev = cJSON_GetObjectItemCaseSensitive(body, "revision");
	const cJSON *tree = cJSON_GetObjectItemCaseSensitive(body, "tree");
	if (!session || !tree) return;
	WWidget *w = widget_by_session(session);
	if (!w) return;
	const cJSON *root = cJSON_GetObjectItemCaseSensitive(tree, "root");
	if (!root) root = tree;
	cJSON_Delete(w->tree);
	w->tree = cJSON_Duplicate(root, 1);
	w->revision = cJSON_IsNumber(rev) ? rev->valueint : jint(tree, "revision", 0);
	w->failures = 0;
	publish_widget(w);
}

static void reopen(WWidget *w, bool backoff)
{
	if (w->session[0] && S.ws_up) {
		cJSON *args = cJSON_CreateArray();
		cJSON_AddItemToArray(args, cJSON_CreateString(w->session));
		ws_request("CloseUiSession", args, P_OTHER, NULL, NULL);
	}
	w->session[0] = 0;
	w->opening = false;
	if (backoff) {
		w->failures++;
		w->retry_at = plat_ms() + (uint64_t)(w->failures < 6 ? 500 << w->failures : 30000);
	}
	open_session(w);
}

static void on_patch(const cJSON *body)
{
	const char *session = jstr(body, "sessionId");
	if (!session) return;
	WWidget *w = widget_by_session(session);
	if (!w) return;
	if (!tree_apply_patch(&w->tree, &w->revision, cJSON_GetObjectItemCaseSensitive(body, "patch"))) {
		// Out of step with the host: a fresh session sends the whole tree again.
		reopen(w, true);
		return;
	}
	publish_widget(w);
}

static void on_navigation(const cJSON *body)
{
	const char *command = jstr(body, "command");
	const char *folder = jstr(body, "folderId");
	const char *profile = jstr(body, "profileId");
	const char *token = jstr(body, "navigationToken");
	snprintf(S.nav_token, sizeof S.nav_token, "%s", token ? token : "");
	if (!command) return;
	if (strcmp(command, "changeTo") == 0 && folder) {
		char id[48];
		snprintf(id, sizeof id, "%s", folder);
		if (folder_by_id(id)) {
			show_folder(id, true);
		} else if (profile) {
			int idx = profile_index_of(profile);
			if (idx >= 0) switch_profile(idx, id);
		}
	} else if (strcmp(command, "parent") == 0) {
		cJSON *f = folder_by_id(S.folder_id);
		const char *parent = f ? jstr(f, "parentId") : NULL;
		if (parent) {
			char id[48];
			snprintf(id, sizeof id, "%s", parent);
			show_folder(id, true);
		}
	} else if (strcmp(command, "back") == 0) {
		if (S.nhistory > 0) {
			char id[48];
			snprintf(id, sizeof id, "%s", S.history[--S.nhistory]);
			show_folder(id, false);
		}
	}
}

static void on_notification(const char *type, const cJSON *body)
{
	if (strcmp(type, "UiSessionTreeUpdatedEvent") == 0) {
		on_tree(body);
	} else if (strcmp(type, "UiSessionPatchedEvent") == 0) {
		on_patch(body);
	} else if (strcmp(type, "UiSessionInvalidatedEvent") == 0 || strcmp(type, "UiSessionClosedEvent") == 0) {
		const char *session = jstr(body, "sessionId");
		WWidget *w = session ? widget_by_session(session) : NULL;
		if (w) {
			w->session[0] = 0; // already gone host-side
			reopen(w, true);
		}
	} else if (strcmp(type, "FolderNavigationEvent") == 0) {
		on_navigation(body);
	} else if (strncmp(type, "Folder", 6) == 0 || strncmp(type, "Widget", 6) == 0 ||
		strcmp(type, "ProfileUpdatedEvent") == 0) {
		// Edits made in Macro Deck: reload the folders shortly (coalesces bursts of events).
		if (strcmp(type, "WidgetStateInvalidatedEvent") != 0) S.reload_at = plat_ms() + 300;
	} else if (strcmp(type, "ProfileCreatedEvent") == 0 || strcmp(type, "ProfileDeletedEvent") == 0 ||
		strcmp(type, "ProfilesReorderedEvent") == 0) {
		S.reload_profiles = true;
		S.reload_at = plat_ms() + 300;
	} else if (strcmp(type, "DeviceSessionRevokedEvent") == 0) {
		drop_socket();
		plat_mutex_lock(&S.cfg_lock);
		config_forget_session(&S.cfg);
		plat_mutex_unlock(&S.cfg_lock);
		save_config();
		set_error("This 3DS was signed out from Macro Deck.");
		set_phase(PHASE_NEED_PAIRING, "Pair this 3DS with Macro Deck");
	} else if (strcmp(type, "ActionExecutionStatusEvent") == 0) {
		const char *st = jstr(body, "status");
		if (st && (strcmp(st, "Failed") == 0 || strcmp(st, "PartiallyFailed") == 0)) model_toast("An action failed");
	} else if (strcmp(type, "HostLockStateChangedEvent") == 0) {
		const cJSON *locked = cJSON_GetObjectItemCaseSensitive(body, "locked");
		if (cJSON_IsTrue(locked)) model_toast("Macro Deck is locked");
	} else if (strcmp(type, "NotificationShownEvent") == 0) {
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(body, "notification");
		const char *title = jstr(n ? n : body, "title");
		if (title) model_toast("%s", title);
	}
}

static void on_response(const cJSON *env, bool is_error)
{
	const char *cid = jstr(env, "correlationId");
	if (!cid) return;
	Pending *p = NULL;
	for (int i = 0; i < MAX_PENDING; i++) {
		if (S.pending[i].used && strcmp(S.pending[i].id, cid) == 0) {
			p = &S.pending[i];
			break;
		}
	}
	if (!p) return;
	Pending done = *p;
	p->used = false;
	const cJSON *payload = cJSON_GetObjectItemCaseSensitive(env, "payload");

	switch (done.kind) {
	case P_REGISTER:
		S.registered = !is_error;
		if (S.registered) {
			open_missing_sessions();
			report_folder(true);
		}
		break;
	case P_OPEN: {
		WWidget *w = widget_by_id(done.widget_id);
		if (!w) {
			// The widget left while we waited: close what the host opened for it.
			const char *sid = jstr(payload, "sessionId");
			if (!is_error && sid && S.ws_up) {
				cJSON *args = cJSON_CreateArray();
				cJSON_AddItemToArray(args, cJSON_CreateString(sid));
				ws_request("CloseUiSession", args, P_OTHER, NULL, NULL);
			}
			break;
		}
		w->opening = false;
		const char *sid = jstr(payload, "sessionId");
		if (is_error || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "accepted")) || !sid) {
			w->failures++;
			w->retry_at = plat_ms() + (uint64_t)(w->failures < 6 ? 1000 << w->failures : 30000);
			break;
		}
		snprintf(w->session, sizeof w->session, "%s", sid);
		cJSON *args = cJSON_CreateArray();
		cJSON *req = cJSON_CreateObject();
		cJSON_AddStringToObject(req, "sessionId", sid);
		cJSON_AddItemToArray(args, req);
		ws_request("AttachUiSession", args, P_ATTACH, w->id, sid);
		break;
	}
	case P_ATTACH: {
		WWidget *w = widget_by_id(done.widget_id);
		if (w && strcmp(w->session, done.session) == 0 &&
			(is_error || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "accepted")))) {
			w->session[0] = 0;
			w->failures++;
			w->retry_at = plat_ms() + (uint64_t)(w->failures < 6 ? 1000 << w->failures : 30000);
		}
		break;
	}
	case P_EVENT:
		if (is_error || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "accepted"))) {
			const char *code = jstr(payload, "code");
			if (!code || strcmp(code, "HOST_LOCKED") != 0) model_toast("Macro Deck did not take that press");
		}
		break;
	default:
		break;
	}
}

static void handle_message(char *text)
{
#ifndef __3DS__
	if (getenv("MD3DS_TRACE")) fprintf(stderr, "<< %.400s\n", text);
#endif
	cJSON *env = cJSON_Parse(text);
	if (!env) return;
	const char *kind = jstr(env, "kind");
	if (kind) {
		if (strcmp(kind, "welcome") == 0) {
			S.welcomed = true;
		} else if (strcmp(kind, "ping") == 0) {
			ws_simple("pong", jstr(env, "id"));
		} else if (strcmp(kind, "response") == 0) {
			on_response(env, false);
		} else if (strcmp(kind, "error") == 0) {
			on_response(env, true);
		} else if (strcmp(kind, "message") == 0) {
			const char *type = jstr(env, "type");
			const cJSON *payload = cJSON_GetObjectItemCaseSensitive(env, "payload");
			if (type) on_notification(type, payload);
		}
	}
	cJSON_Delete(env);
}

/* ---------- user actions ---------- */

static void execute_fallback(WWidget *w, const char *trigger)
{
	cJSON *body = cJSON_CreateObject();
	cJSON_AddStringToObject(body, "widgetId", w->id);
	cJSON_AddStringToObject(body, "folderId", S.folder_id);
	cJSON_AddStringToObject(body, "triggerType", trigger);
	cJSON_AddStringToObject(body, "clientId", S.cfg.client_id);
	char *text = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);
	HttpResponse rs;
	api("POST", "/api/actions/execute", text, NULL, NULL, &rs);
	free(text);
	if (rs.status != 200) model_toast("That press did not reach Macro Deck");
	else {
		cJSON *j = cJSON_Parse(rs.body);
		const char *st = jstr(j, "status");
		const cJSON *ok = cJSON_GetObjectItemCaseSensitive(j, "success");
		if ((st && (strcmp(st, "Failed") == 0 || strcmp(st, "PartiallyFailed") == 0)) || cJSON_IsFalse(ok)) {
			model_toast("An action failed");
		}
		cJSON_Delete(j);
	}
	http_free(&rs);
}

static void send_ui_event(WWidget *w, const char *node, const char *name, cJSON *data)
{
	cJSON *args = cJSON_CreateArray();
	cJSON *req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "sessionId", w->session);
	cJSON_AddStringToObject(req, "nodeId", node);
	cJSON_AddStringToObject(req, "name", name);
	if (data) cJSON_AddItemToObject(req, "data", data);
	cJSON_AddNumberToObject(req, "revision", w->revision);
	cJSON_AddItemToArray(args, req);
	ws_request("SendUiEvent", args, strcmp(name, "press") == 0 || strcmp(name, "long-press") == 0 ? P_EVENT : P_OTHER,
		w->id, w->session);
}

static void do_press(const char *widget_id, const char *event)
{
	WWidget *w = widget_by_id(widget_id);
	if (!w) return;
	bool gesture = strcmp(event, "press") == 0 || strcmp(event, "long-press") == 0;
	if (!w->session[0] || !w->tree) {
		// No live tree: an action button can still run its flows through the REST endpoint.
		if (gesture && strcmp(w->type, "ActionButton") == 0) {
			execute_fallback(w, strcmp(event, "press") == 0 ? "onShortPress" : "onLongPress");
		}
		return;
	}
	const char *node = tree_find_event_node(w->tree, event);
	if (!node && strcmp(event, "long-press") == 0) {
		// Nothing listens for a hold: treat it as an ordinary press.
		event = "press";
		node = tree_find_event_node(w->tree, event);
	}
	if (!node) {
		if (!gesture) return;
		node = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(w->tree, "id"))
			? cJSON_GetObjectItemCaseSensitive(w->tree, "id")->valuestring
			: NULL;
		if (!node) return;
	}
	char nid[136];
	snprintf(nid, sizeof nid, "%s", node);
	send_ui_event(w, nid, event, NULL);
}

static void do_slider(const char *widget_id, double level)
{
	WWidget *w = widget_by_id(widget_id);
	if (!w || !w->session[0] || !w->tree) return;
	bool vertical = false;
	const char *node = tree_find_slider(w->tree, &vertical);
	if (!node) return;
	char nid[136];
	snprintf(nid, sizeof nid, "%s", node);
	send_ui_event(w, nid, "change", cJSON_CreateNumber(level));
}

static void do_back(void)
{
	if (S.nhistory > 0) {
		char id[48];
		snprintf(id, sizeof id, "%s", S.history[--S.nhistory]);
		show_folder(id, false);
		return;
	}
	cJSON *f = folder_by_id(S.folder_id);
	const char *parent = f ? jstr(f, "parentId") : NULL;
	if (parent) {
		char id[48];
		snprintf(id, sizeof id, "%s", parent);
		show_folder(id, false);
	}
}

static void reload_deck(void)
{
	if (S.reload_profiles) {
		S.reload_profiles = false;
		cJSON *p = current_profile();
		char keep[48];
		snprintf(keep, sizeof keep, "%s", p && jstr(p, "id") ? jstr(p, "id") : "");
		if (!load_profiles()) return;
		int idx = profile_index_of(keep);
		if (idx < 0) {
			switch_profile(0, NULL);
			return;
		}
		S.profile_idx = idx;
	}
	cJSON *p = current_profile();
	const char *pid = p ? jstr(p, "id") : NULL;
	if (!pid || !load_folders(pid)) return;
	char keep[48];
	snprintf(keep, sizeof keep, "%s", S.folder_id);
	if (folder_by_id(keep)) {
		show_folder(keep, false);
	} else {
		const char *start = start_folder();
		S.nhistory = 0;
		if (start) {
			char id[48];
			snprintf(id, sizeof id, "%s", start);
			show_folder(id, false);
		}
	}
}

static void process_command(const Command *c)
{
	switch (c->type) {
	case CMD_DISCOVER:
		drop_socket();
		do_discover(false);
		break;
	case CMD_STARTUP:
		if (S.cfg.mode == CONN_MODE_AUTO) do_discover(true);
		else do_connect();
		break;
	case CMD_CONNECT: {
		bool changed = strcmp(S.cfg.host, c->a) != 0 || S.cfg.port != c->n;
		plat_mutex_lock(&S.cfg_lock);
		if (changed) {
			// A different computer: its tokens are not ours to reuse.
			config_forget_session(&S.cfg);
			S.cfg.device_id[0] = 0;
			S.cfg.device_secret[0] = 0;
		}
		snprintf(S.cfg.host, sizeof S.cfg.host, "%s", c->a);
		snprintf(S.cfg.host_name, sizeof S.cfg.host_name, "%s", c->b);
		S.cfg.port = c->n;
		plat_mutex_unlock(&S.cfg_lock);
		save_config();
		if (changed) {
			close_all_sessions();
			S.nw = 0;
			S.folder_id[0] = 0;
			S.nhistory = 0;
			publish_deck();
		}
		do_connect();
		break;
	}
	case CMD_PAIR:
		if (S.cfg.host[0]) do_pair(c->a);
		break;
	case CMD_PRESS:
		if (S.phase == PHASE_ONLINE) do_press(c->a, c->b);
		break;
	case CMD_SLIDER:
		if (S.phase == PHASE_ONLINE) do_slider(c->a, c->n / 10000.0);
		break;
	case CMD_BACK:
		if (S.phase == PHASE_ONLINE) do_back();
		break;
	case CMD_PROFILE_STEP: {
		if (S.phase != PHASE_ONLINE) break;
		int n = cJSON_GetArraySize(S.profiles);
		if (n > 1) switch_profile(((S.profile_idx + c->n) % n + n) % n, NULL);
		break;
	}
	case CMD_RECONNECT:
		do_connect();
		break;
	case CMD_FORGET:
		drop_socket();
		close_all_sessions();
		S.nw = 0;
		S.folder_id[0] = 0;
		if (S.cfg.access_token[0]) {
			HttpRequest rq = {.method = "POST", .path = "/api/auth/logout", .body = "", .body_len = 0};
			char cookie[480];
			snprintf(cookie, sizeof cookie, "%s=%s", S.cfg.refresh_cookie_name, S.cfg.refresh_cookie_value);
			rq.cookie = S.cfg.refresh_cookie_name[0] ? cookie : NULL;
			HttpResponse rs;
			http_do(S.cfg.host, S.cfg.port, &rq, &rs, 4000);
			http_free(&rs);
		}
		plat_mutex_lock(&S.cfg_lock);
		config_forget_session(&S.cfg);
		plat_mutex_unlock(&S.cfg_lock);
		save_config();
		publish_deck();
		set_phase(S.cfg.host[0] ? PHASE_NEED_PAIRING : PHASE_IDLE,
			S.cfg.host[0] ? "Pair this 3DS with Macro Deck" : "Pick a computer to connect to");
		break;
	case CMD_SET_MODE:
		plat_mutex_lock(&S.cfg_lock);
		S.cfg.mode = c->n;
		plat_mutex_unlock(&S.cfg_lock);
		save_config();
		publish_host();
		break;
	}
}

static bool pop_command(Command *out)
{
	bool got = false;
	plat_mutex_lock(&S.q_lock);
	if (S.q_head != S.q_tail) {
		*out = S.queue[S.q_head];
		S.q_head = (S.q_head + 1) % QUEUE_LEN;
		got = true;
	}
	plat_mutex_unlock(&S.q_lock);
	return got;
}

void client_cmd(CommandType type, const char *a, const char *b, int n)
{
	plat_mutex_lock(&S.q_lock);
	int next = (S.q_tail + 1) % QUEUE_LEN;
	if (next != S.q_head) {
		Command *c = &S.queue[S.q_tail];
		memset(c, 0, sizeof *c);
		c->type = type;
		if (a) snprintf(c->a, sizeof c->a, "%s", a);
		if (b) snprintf(c->b, sizeof c->b, "%s", b);
		c->n = n;
		S.q_tail = next;
	}
	plat_mutex_unlock(&S.q_lock);
}

void client_config(Config *out)
{
	plat_mutex_lock(&S.cfg_lock);
	*out = S.cfg;
	plat_mutex_unlock(&S.cfg_lock);
}

/* ---------- worker loop ---------- */

static void worker(void *arg)
{
	(void)arg;
	while (S.running) {
		bool busy = false;
		Command c;
		while (pop_command(&c)) {
			process_command(&c);
			busy = true;
		}

		if (S.phase == PHASE_RETRY_WAIT && plat_ms() >= S.retry_at) do_connect();

		if (S.phase == PHASE_ONLINE) {
			if (S.ws_up) {
				for (int i = 0; i < 64 && S.ws_up; i++) {
					char *msg;
					size_t len;
					int r = ws_poll(&S.ws, &msg, &len);
					if (r < 0) {
						S.ws_up = false;
						break;
					}
					if (r == 0) break;
					handle_message(msg);
					busy = true;
				}
				if (S.ws_up && plat_ms() - S.ws.last_rx > STALE_AFTER_MS) S.ws_up = false;
			}
			if (!S.ws_up) {
				drop_socket();
				set_error("Lost the live connection.");
				S.retry_at = plat_ms() + 2000;
				set_phase(PHASE_RETRY_WAIT, "Reconnecting...");
				continue;
			}
			if (S.reload_at && plat_ms() >= S.reload_at) {
				S.reload_at = 0;
				reload_deck();
				busy = true;
			}
			// Sessions refused earlier (or still missing) get another go once their backoff passes.
			open_missing_sessions();
			if (fetch_one_image()) busy = true;
		}
		if (!busy) plat_sleep_ms(8);
	}
	drop_socket();
}

void client_start(const Config *initial)
{
	memset(&S, 0, sizeof S);
	S.ws.fd = -1;
	S.cfg = *initial;
	plat_mutex_init(&S.cfg_lock);
	plat_mutex_init(&S.q_lock);
	S.running = true;
	publish_host();
	set_phase(PHASE_IDLE, "Starting...");
	S.thread = plat_thread_start(worker, NULL);
}

void client_stop(void)
{
	S.running = false;
	plat_thread_join(S.thread);
	S.thread = NULL;
	close_all_sessions();
	cJSON_Delete(S.profiles);
	cJSON_Delete(S.folders);
	S.profiles = S.folders = NULL;
}
