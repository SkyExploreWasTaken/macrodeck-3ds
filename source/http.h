#pragma once

#include <stddef.h>

typedef struct {
	const char *method;       // "GET", "POST"
	const char *path;         // "/api/..."
	const char *bearer;       // access token or NULL
	const char *cookie;       // "name=value" or NULL
	const char *accept;       // NULL = application/json
	const char *extra_header; // one full "Name: value" line without CRLF, or NULL
	const char *body;         // JSON body or NULL
	size_t body_len;
} HttpRequest;

typedef struct {
	int status;       // HTTP status, or -1 when the host could not be reached / the answer was garbage
	char *body;       // NUL-terminated, malloc'd (may be NULL)
	size_t body_len;
	char content_type[64];
	char refresh_cookie_name[48];  // set when the host sent an md_refresh* cookie with a value
	char refresh_cookie_value[400];
} HttpResponse;

// Performs one request on a fresh connection ("Connection: close"). Always fill/free the response.
void http_do(const char *host, int port, const HttpRequest *rq, HttpResponse *rs, int timeout_ms);
void http_free(HttpResponse *rs);
