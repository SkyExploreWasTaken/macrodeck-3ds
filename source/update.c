#include "update.h"
#include "platform.h"

#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cacert_bin.h"

#ifndef APP_BUILD_STR
#define APP_BUILD_STR "dev"
#endif

#define RELEASE_API "https://api.github.com/repos/SkyExploreWasTaken/macrodeck-3ds/releases/tags/nightly"
#define USER_AGENT "MacroDeck3DS/" APP_BUILD_STR " (Nintendo 3DS)"
#define DEFAULT_3DSX_PATH "sdmc:/3ds/MacroDeck3DS.3dsx"

static plat_mutex g_lock;
static UpdateStatus g_status;
static plat_thread *g_thread;
static bool g_curl_ready;
static char g_self[256];
static char g_cia_url[512];
static char g_3dsx_url[512];

static void set_status(UpdateState st, const char *msg)
{
	plat_mutex_lock(&g_lock);
	g_status.state = st;
	snprintf(g_status.message, sizeof g_status.message, "%s", msg ? msg : "");
	plat_mutex_unlock(&g_lock);
}

void update_init(const char *self_path)
{
	plat_mutex_init(&g_lock);
	memset(&g_status, 0, sizeof g_status);
	snprintf(g_self, sizeof g_self, "%s", self_path && strncmp(self_path, "sdmc:/", 6) == 0 ? self_path : DEFAULT_3DSX_PATH);
	g_curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

static void join_previous(void)
{
	if (g_thread) {
		plat_thread_join(g_thread);
		g_thread = NULL;
	}
}

void update_exit(void)
{
	join_previous();
	if (g_curl_ready) curl_global_cleanup();
}

void update_status(UpdateStatus *out)
{
	plat_mutex_lock(&g_lock);
	*out = g_status;
	plat_mutex_unlock(&g_lock);
}

/* ---------- curl plumbing ---------- */

typedef struct {
	char *data;
	size_t len;
} MemBuf;

static size_t mem_write(char *ptr, size_t size, size_t n, void *ud)
{
	MemBuf *m = ud;
	size_t add = size * n;
	if (m->len + add > 512 * 1024) return 0;
	char *nd = realloc(m->data, m->len + add + 1);
	if (!nd) return 0;
	memcpy(nd + m->len, ptr, add);
	m->data = nd;
	m->len += add;
	m->data[m->len] = 0;
	return add;
}

static int on_progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
	(void)ud, (void)ut, (void)un;
	if (total > 0) {
		plat_mutex_lock(&g_lock);
		g_status.progress = (float)now / (float)total;
		plat_mutex_unlock(&g_lock);
	}
	return 0;
}

static CURL *new_handle(const char *url)
{
	CURL *c = curl_easy_init();
	if (!c) return NULL;
	struct curl_blob ca = {.data = (void *)cacert_bin, .len = cacert_bin_size, .flags = CURL_BLOB_NOCOPY};
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	return c;
}

static void fail_with(const char *what, CURLcode rc)
{
	char msg[96];
	snprintf(msg, sizeof msg, "%s: %s", what, curl_easy_strerror(rc));
	set_status(UPD_FAILED, msg);
}

/* ---------- check ---------- */

static void check_thread(void *arg)
{
	(void)arg;
	MemBuf body = {0};
	CURL *c = new_handle(RELEASE_API);
	if (!c) {
		set_status(UPD_FAILED, "Could not start the download");
		return;
	}
	struct curl_slist *hdr = curl_slist_append(NULL, "Accept: application/vnd.github+json");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
	CURLcode rc = curl_easy_perform(c);
	curl_easy_cleanup(c);
	curl_slist_free_all(hdr);
	if (rc != CURLE_OK) {
		free(body.data);
		fail_with("Update check failed", rc);
		return;
	}

	cJSON *rel = cJSON_Parse(body.data);
	free(body.data);
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(rel, "name");
	char latest[16] = "";
	// The release is named "Nightly build <commit> (<date>)".
	if (cJSON_IsString(name) && strncmp(name->valuestring, "Nightly build ", 14) == 0) {
		sscanf(name->valuestring + 14, "%15[0-9a-f]", latest);
	}
	g_cia_url[0] = g_3dsx_url[0] = 0;
	const cJSON *asset;
	cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(rel, "assets"))
	{
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(asset, "name");
		const cJSON *u = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
		if (!cJSON_IsString(n) || !cJSON_IsString(u)) continue;
		size_t nl = strlen(n->valuestring);
		if (nl > 4 && strcmp(n->valuestring + nl - 4, ".cia") == 0) snprintf(g_cia_url, sizeof g_cia_url, "%s", u->valuestring);
		if (nl > 5 && strcmp(n->valuestring + nl - 5, ".3dsx") == 0) snprintf(g_3dsx_url, sizeof g_3dsx_url, "%s", u->valuestring);
	}
	cJSON_Delete(rel);

	bool as_cia = !envIsHomebrew();
	if (!latest[0] || !(as_cia ? g_cia_url[0] : g_3dsx_url[0])) {
		set_status(UPD_FAILED, "No nightly build found");
		return;
	}
	plat_mutex_lock(&g_lock);
	snprintf(g_status.latest, sizeof g_status.latest, "%s", latest);
	plat_mutex_unlock(&g_lock);
	// Builds compare by commit: a different commit on the nightly means there is something newer to get.
	if (strncmp(latest, APP_BUILD_STR, strlen(latest)) == 0) set_status(UPD_CURRENT, NULL);
	else set_status(UPD_AVAILABLE, NULL);
}

void update_check(void)
{
	plat_mutex_lock(&g_lock);
	UpdateState st = g_status.state;
	plat_mutex_unlock(&g_lock);
	if (!g_curl_ready || st == UPD_CHECKING || st == UPD_DOWNLOADING || st == UPD_INSTALLING) return;
	join_previous();
	set_status(UPD_CHECKING, NULL);
	g_thread = plat_thread_start(check_thread, NULL);
	if (!g_thread) set_status(UPD_FAILED, "Could not start the update check");
}

/* ---------- install ---------- */

typedef struct {
	Handle cia;
	u64 offset;
	FILE *file;
	bool failed;
} Sink;

static size_t sink_write(char *ptr, size_t size, size_t n, void *ud)
{
	Sink *s = ud;
	size_t len = size * n;
	if (s->file) {
		if (fwrite(ptr, 1, len, s->file) != len) {
			s->failed = true;
			return 0;
		}
		return len;
	}
	u32 written = 0;
	if (R_FAILED(FSFILE_Write(s->cia, &written, s->offset, ptr, (u32)len, 0)) || written != len) {
		s->failed = true;
		return 0;
	}
	s->offset += len;
	return len;
}

static void install_thread(void *arg)
{
	(void)arg;
	bool as_cia = !envIsHomebrew();
	Sink sink = {0};
	char tmp[272];

	if (as_cia) {
		if (R_FAILED(amInit())) {
			set_status(UPD_FAILED, "The system installer is not available");
			return;
		}
		if (R_FAILED(AM_StartCiaInstall(MEDIATYPE_SD, &sink.cia))) {
			amExit();
			set_status(UPD_FAILED, "The system installer refused to start");
			return;
		}
	} else {
		snprintf(tmp, sizeof tmp, "%s.new", g_self);
		sink.file = fopen(tmp, "wb");
		if (!sink.file) {
			set_status(UPD_FAILED, "Can't write to the SD card");
			return;
		}
	}

	CURL *c = new_handle(as_cia ? g_cia_url : g_3dsx_url);
	CURLcode rc = CURLE_FAILED_INIT;
	if (c) {
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_write);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
		rc = curl_easy_perform(c);
		curl_easy_cleanup(c);
	}
	bool ok = rc == CURLE_OK && !sink.failed;

	if (as_cia) {
		if (ok) {
			set_status(UPD_INSTALLING, NULL);
			ok = R_SUCCEEDED(AM_FinishCiaInstall(sink.cia));
			if (!ok) set_status(UPD_FAILED, "The system installer rejected the update");
		} else {
			AM_CancelCIAInstall(sink.cia);
		}
		amExit();
	} else {
		if (fclose(sink.file) != 0) ok = false;
		if (ok) {
			set_status(UPD_INSTALLING, NULL);
			remove(g_self);
			if (rename(tmp, g_self) != 0) {
				ok = false;
				set_status(UPD_FAILED, "Could not replace the app file");
			}
		} else {
			remove(tmp);
		}
	}
	if (ok) set_status(UPD_DONE, NULL);
	else {
		plat_mutex_lock(&g_lock);
		bool reported = g_status.state == UPD_FAILED;
		plat_mutex_unlock(&g_lock);
		if (!reported) {
			if (rc != CURLE_OK) fail_with("Download failed", rc);
			else set_status(UPD_FAILED, "Saving the update failed");
		}
	}
}

void update_install(void)
{
	plat_mutex_lock(&g_lock);
	UpdateState st = g_status.state;
	plat_mutex_unlock(&g_lock);
	if (st != UPD_AVAILABLE && st != UPD_FAILED) return;
	if (!g_cia_url[0] && !g_3dsx_url[0]) {
		update_check();
		return;
	}
	join_previous();
	plat_mutex_lock(&g_lock);
	g_status.progress = 0;
	plat_mutex_unlock(&g_lock);
	set_status(UPD_DOWNLOADING, NULL);
	g_thread = plat_thread_start(install_thread, NULL);
	if (!g_thread) set_status(UPD_FAILED, "Could not start the download");
}
