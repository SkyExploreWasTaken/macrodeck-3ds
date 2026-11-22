#include "config.h"
#include "platform.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 8193

static void path_of(char *out, size_t len)
{
	snprintf(out, len, "%s/config.json", plat_data_dir());
}

static void new_client_id(char *out, size_t len)
{
	uint8_t raw[16];
	plat_random(raw, sizeof raw);
	size_t o = 0;
	for (int i = 0; i < 16 && o + 2 < len; i++) o += (size_t)snprintf(out + o, len - o, "%02x", raw[i]);
}

void config_defaults(Config *c)
{
	memset(c, 0, sizeof *c);
	c->mode = CONN_MODE_AUTO;
	c->port = DEFAULT_PORT;
	c->screen_off_min = 3;
	c->pixel_shift = true;
	new_client_id(c->client_id, sizeof c->client_id);
}

static void read_str(cJSON *root, const char *key, char *out, size_t len)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
	if (cJSON_IsString(v) && v->valuestring) snprintf(out, len, "%s", v->valuestring);
}

bool config_load(Config *c)
{
	config_defaults(c);
	char path[256];
	path_of(path, sizeof path);
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 64 * 1024) {
		fclose(f);
		return false;
	}
	char *buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return false;
	}
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = 0;
	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root) return false;

	cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
	if (cJSON_IsNumber(mode)) c->mode = mode->valueint == CONN_MODE_MANUAL ? CONN_MODE_MANUAL : CONN_MODE_AUTO;
	cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
	if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint < 65536) c->port = port->valueint;
	cJSON *off = cJSON_GetObjectItemCaseSensitive(root, "screenOffMinutes");
	if (cJSON_IsNumber(off) && off->valueint >= 0 && off->valueint <= 60) c->screen_off_min = off->valueint;
	cJSON *shift = cJSON_GetObjectItemCaseSensitive(root, "pixelShift");
	if (cJSON_IsBool(shift)) c->pixel_shift = cJSON_IsTrue(shift);
	read_str(root, "host", c->host, sizeof c->host);
	read_str(root, "hostName", c->host_name, sizeof c->host_name);
	read_str(root, "clientId", c->client_id, sizeof c->client_id);
	read_str(root, "accessToken", c->access_token, sizeof c->access_token);
	read_str(root, "refreshCookieName", c->refresh_cookie_name, sizeof c->refresh_cookie_name);
	read_str(root, "refreshCookieValue", c->refresh_cookie_value, sizeof c->refresh_cookie_value);
	read_str(root, "deviceId", c->device_id, sizeof c->device_id);
	read_str(root, "deviceSecret", c->device_secret, sizeof c->device_secret);
	read_str(root, "startupProfileId", c->startup_profile_id, sizeof c->startup_profile_id);
	read_str(root, "lastProfileId", c->last_profile_id, sizeof c->last_profile_id);
	cJSON_Delete(root);
	if (!c->client_id[0]) new_client_id(c->client_id, sizeof c->client_id);
	return true;
}

bool config_save(const Config *c)
{
	plat_mkdirs(plat_data_dir());
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "mode", c->mode);
	cJSON_AddNumberToObject(root, "screenOffMinutes", c->screen_off_min);
	cJSON_AddBoolToObject(root, "pixelShift", c->pixel_shift);
	cJSON_AddStringToObject(root, "host", c->host);
	cJSON_AddNumberToObject(root, "port", c->port);
	cJSON_AddStringToObject(root, "hostName", c->host_name);
	cJSON_AddStringToObject(root, "clientId", c->client_id);
	cJSON_AddStringToObject(root, "accessToken", c->access_token);
	cJSON_AddStringToObject(root, "refreshCookieName", c->refresh_cookie_name);
	cJSON_AddStringToObject(root, "refreshCookieValue", c->refresh_cookie_value);
	cJSON_AddStringToObject(root, "deviceId", c->device_id);
	cJSON_AddStringToObject(root, "deviceSecret", c->device_secret);
	cJSON_AddStringToObject(root, "startupProfileId", c->startup_profile_id);
	cJSON_AddStringToObject(root, "lastProfileId", c->last_profile_id);
	char *text = cJSON_Print(root);
	cJSON_Delete(root);
	if (!text) return false;

	char path[256], tmp[260];
	path_of(path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	bool ok = false;
	if (f) {
		ok = fwrite(text, 1, strlen(text), f) == strlen(text);
		fclose(f);
		if (ok) {
			remove(path);
			ok = rename(tmp, path) == 0;
		}
	}
	free(text);
	return ok;
}

void config_forget_session(Config *c)
{
	c->access_token[0] = 0;
	c->refresh_cookie_name[0] = 0;
	c->refresh_cookie_value[0] = 0;
	c->startup_profile_id[0] = 0;
	c->last_profile_id[0] = 0;
	// The device id/secret stay: re-pairing with them makes the host treat this as the same device.
}
