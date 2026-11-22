#include "http.h"
#include "net.h"
#include "platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_RESPONSE (6 * 1024 * 1024)

static const char *find_header_end(const char *buf, size_t len)
{
	for (size_t i = 0; i + 3 < len; i++) {
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return buf + i;
	}
	return NULL;
}

// Header lookup inside the raw header block (case-insensitive name). Copies the trimmed value.
static int header_value(const char *headers, const char *name, char *out, size_t out_len, const char **resume)
{
	size_t nlen = strlen(name);
	const char *p = resume && *resume ? *resume : headers;
	while (p && *p) {
		const char *eol = strstr(p, "\r\n");
		if (!eol) eol = p + strlen(p);
		if ((size_t)(eol - p) > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
			const char *v = p + nlen + 1;
			while (v < eol && (*v == ' ' || *v == '\t')) v++;
			size_t vlen = (size_t)(eol - v);
			if (vlen >= out_len) vlen = out_len - 1;
			memcpy(out, v, vlen);
			out[vlen] = 0;
			if (resume) *resume = *eol ? eol + 2 : eol;
			return 1;
		}
		p = *eol ? eol + 2 : NULL;
	}
	if (resume) *resume = NULL;
	return 0;
}

static size_t dechunk(char *data, size_t len)
{
	size_t in = 0, out = 0;
	while (in < len) {
		char *end;
		unsigned long size = strtoul(data + in, &end, 16);
		const char *crlf = strstr(data + in, "\r\n");
		if (!crlf) break;
		in = (size_t)(crlf - data) + 2;
		(void)end;
		if (size == 0) break;
		if (in + size > len) size = len - in;
		memmove(data + out, data + in, size);
		out += size;
		in += size + 2;
	}
	data[out] = 0;
	return out;
}

void http_free(HttpResponse *rs)
{
	free(rs->body);
	rs->body = NULL;
	rs->body_len = 0;
}

void http_do(const char *host, int port, const HttpRequest *rq, HttpResponse *rs, int timeout_ms)
{
	memset(rs, 0, sizeof *rs);
	rs->status = -1;

	int fd = tcp_connect(host, port, timeout_ms < 5000 ? timeout_ms : 5000);
	if (fd < 0) return;

	size_t cap = 1024 + rq->body_len + (rq->bearer ? strlen(rq->bearer) : 0) + (rq->cookie ? strlen(rq->cookie) : 0);
	char *req = malloc(cap);
	if (!req) {
		sock_close(fd);
		return;
	}
	int n = snprintf(req, cap,
		"%s %s HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"User-Agent: MacroDeck3DS/1.0 (Nintendo 3DS)\r\n"
		"Accept: %s\r\n"
		"Accept-Encoding: identity\r\n"
		"Connection: close\r\n",
		rq->method, rq->path, host, port, rq->accept ? rq->accept : "application/json");
	if (rq->bearer && *rq->bearer) n += snprintf(req + n, cap - n, "Authorization: Bearer %s\r\n", rq->bearer);
	if (rq->cookie && *rq->cookie) n += snprintf(req + n, cap - n, "Cookie: %s\r\n", rq->cookie);
	if (rq->extra_header) n += snprintf(req + n, cap - n, "%s\r\n", rq->extra_header);
	if (rq->body || strcmp(rq->method, "POST") == 0) {
		n += snprintf(req + n, cap - n, "Content-Type: application/json\r\nContent-Length: %u\r\n",
			(unsigned)rq->body_len);
	}
	n += snprintf(req + n, cap - n, "\r\n");
	if (rq->body && rq->body_len) {
		memcpy(req + n, rq->body, rq->body_len);
		n += (int)rq->body_len;
	}

	int sent = sock_send_all(fd, req, (size_t)n, timeout_ms);
	free(req);
	if (sent != 0) {
		sock_close(fd);
		return;
	}

	size_t len = 0, bufcap = 16384;
	char *buf = malloc(bufcap + 1);
	if (!buf) {
		sock_close(fd);
		return;
	}
	uint64_t deadline = plat_ms() + (uint64_t)timeout_ms;
	long content_length = -1;
	size_t header_len = 0;
	int chunked = 0;
	for (;;) {
		if (len == bufcap) {
			if (bufcap >= MAX_RESPONSE) break;
			bufcap *= 2;
			char *nb = realloc(buf, bufcap + 1);
			if (!nb) break;
			buf = nb;
		}
		int r = sock_recv(fd, buf + len, bufcap - len);
		if (r > 0) {
			len += (size_t)r;
			if (!header_len) {
				buf[len] = 0;
				const char *he = find_header_end(buf, len);
				if (he) {
					header_len = (size_t)(he - buf) + 4;
					char tmp[32];
					char saved = buf[header_len - 2];
					buf[header_len - 2] = 0;
					if (header_value(buf, "Content-Length", tmp, sizeof tmp, NULL)) content_length = atol(tmp);
					if (header_value(buf, "Transfer-Encoding", tmp, sizeof tmp, NULL) && strstr(tmp, "chunked")) chunked = 1;
					buf[header_len - 2] = saved;
				}
			}
			if (header_len && content_length >= 0 && len >= header_len + (size_t)content_length) break;
			if (header_len && chunked && len >= 5 && memcmp(buf + len - 5, "0\r\n\r\n", 5) == 0) break;
			continue;
		}
		if (r == 0) break;
		if (r == -1) break;
		int left = (int)(deadline - plat_ms());
		if ((int64_t)(deadline - plat_ms()) <= 0) break;
		if (sock_wait(fd, 1, left > 250 ? 250 : left) < 0) break;
	}
	sock_close(fd);
	buf[len] = 0;

	if (!header_len || len < 12 || strncmp(buf, "HTTP/1.", 7) != 0) {
		free(buf);
		return;
	}
	rs->status = atoi(buf + 9);
	buf[header_len - 2] = 0; // terminate header block

	header_value(buf, "Content-Type", rs->content_type, sizeof rs->content_type, NULL);

	const char *resume = NULL;
	char cookie[512];
	for (;;) {
		if (!header_value(buf, "Set-Cookie", cookie, sizeof cookie, &resume)) break;
		if (strncmp(cookie, "md_refresh", 10) == 0) {
			char *eq = strchr(cookie, '=');
			char *semi = strchr(cookie, ';');
			if (semi) *semi = 0;
			if (eq && eq[1]) {
				*eq = 0;
				snprintf(rs->refresh_cookie_name, sizeof rs->refresh_cookie_name, "%s", cookie);
				snprintf(rs->refresh_cookie_value, sizeof rs->refresh_cookie_value, "%s", eq + 1);
			}
		}
		if (!resume) break;
	}

	size_t body_len = len - header_len;
	memmove(buf, buf + header_len, body_len);
	buf[body_len] = 0;
	if (chunked) body_len = dechunk(buf, body_len);
	else if (content_length >= 0 && (size_t)content_length < body_len) body_len = (size_t)content_length;
	buf[body_len] = 0;
	rs->body = buf;
	rs->body_len = body_len;
}
