#include "ws.h"
#include "net.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define WS_MAX_MESSAGE (2 * 1024 * 1024)

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const uint8_t *in, size_t len, char *out)
{
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < len) v |= in[i + 2];
		out[o++] = b64chars[(v >> 18) & 63];
		out[o++] = b64chars[(v >> 12) & 63];
		out[o++] = i + 1 < len ? b64chars[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? b64chars[v & 63] : '=';
	}
	out[o] = 0;
}

static int ensure(uint8_t **buf, size_t *cap, size_t need)
{
	if (need <= *cap) return 0;
	size_t nc = *cap ? *cap : 4096;
	while (nc < need) nc *= 2;
	uint8_t *nb = realloc(*buf, nc);
	if (!nb) return -1;
	*buf = nb;
	*cap = nc;
	return 0;
}

static int send_frame(WsConn *ws, int opcode, const uint8_t *data, size_t len)
{
	if (!ws->open) return -1;
	uint8_t header[14];
	size_t h = 0;
	header[h++] = 0x80 | (uint8_t)opcode;
	if (len < 126) {
		header[h++] = 0x80 | (uint8_t)len;
	} else if (len < 65536) {
		header[h++] = 0x80 | 126;
		header[h++] = (uint8_t)(len >> 8);
		header[h++] = (uint8_t)len;
	} else {
		header[h++] = 0x80 | 127;
		for (int i = 7; i >= 0; i--) header[h++] = (uint8_t)((uint64_t)len >> (i * 8));
	}
	uint8_t mask[4];
	plat_random(mask, 4);
	memcpy(header + h, mask, 4);
	h += 4;

	uint8_t *frame = malloc(h + len);
	if (!frame) return -1;
	memcpy(frame, header, h);
	for (size_t i = 0; i < len; i++) frame[h + i] = data[i] ^ mask[i & 3];
	int rc = sock_send_all(ws->fd, frame, h + len, 5000);
	free(frame);
	if (rc != 0) ws->open = false;
	return rc;
}

int ws_send_text(WsConn *ws, const char *text, size_t len)
{
	return send_frame(ws, 0x1, (const uint8_t *)text, len);
}

int ws_connect(WsConn *ws, const char *host, int port, const char *path, const char *subprotocol, int timeout_ms)
{
	memset(ws, 0, sizeof *ws);
	ws->fd = tcp_connect(host, port, timeout_ms);
	if (ws->fd < 0) return -1;

	uint8_t keyraw[16];
	char key[32];
	plat_random(keyraw, sizeof keyraw);
	base64(keyraw, sizeof keyraw, key);

	size_t cap = strlen(path) + strlen(host) + 512;
	char *req = malloc(cap);
	if (!req) goto fail;
	int n = snprintf(req, cap,
		"GET %s HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"User-Agent: MacroDeck3DS/1.0 (Nintendo 3DS)\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Protocol: %s\r\n\r\n",
		path, host, port, key, subprotocol);
	int rc = sock_send_all(ws->fd, req, (size_t)n, timeout_ms);
	free(req);
	if (rc != 0) goto fail;

	// Read the handshake answer; anything after the blank line already belongs to the frame stream.
	uint64_t deadline = plat_ms() + (uint64_t)timeout_ms;
	char head[4096];
	size_t hl = 0;
	char *end = NULL;
	while (!end) {
		if (hl >= sizeof head - 1) goto fail;
		int r = sock_recv(ws->fd, head + hl, sizeof head - 1 - hl);
		if (r > 0) {
			hl += (size_t)r;
			head[hl] = 0;
			end = strstr(head, "\r\n\r\n");
			continue;
		}
		if (r == 0 || r == -1) goto fail;
		if ((int64_t)(deadline - plat_ms()) <= 0) goto fail;
		if (sock_wait(ws->fd, 1, 200) < 0) goto fail;
	}
	if (strncmp(head, "HTTP/1.1 101", 12) != 0) goto fail;

	size_t used = (size_t)(end - head) + 4;
	if (hl > used) {
		if (ensure(&ws->rx, &ws->rx_cap, hl - used) != 0) goto fail;
		memcpy(ws->rx, head + used, hl - used);
		ws->rx_len = hl - used;
	}
	ws->open = true;
	ws->last_rx = plat_ms();
	return 0;

fail:
	sock_close(ws->fd);
	ws->fd = -1;
	free(ws->rx);
	ws->rx = NULL;
	return -1;
}

// Tries to take one frame off the rx buffer. 1 consumed a frame, 0 incomplete, -1 protocol error.
static int take_frame(WsConn *ws)
{
	if (ws->rx_len < 2) return 0;
	uint8_t *p = ws->rx;
	int fin = p[0] & 0x80;
	int opcode = p[0] & 0x0F;
	int masked = p[1] & 0x80;
	uint64_t plen = p[1] & 0x7F;
	size_t h = 2;
	if (plen == 126) {
		if (ws->rx_len < 4) return 0;
		plen = ((uint64_t)p[2] << 8) | p[3];
		h = 4;
	} else if (plen == 127) {
		if (ws->rx_len < 10) return 0;
		plen = 0;
		for (int i = 0; i < 8; i++) plen = (plen << 8) | p[2 + i];
		h = 10;
	}
	if (plen > WS_MAX_MESSAGE) return -1;
	uint8_t mask[4] = {0, 0, 0, 0};
	if (masked) {
		if (ws->rx_len < h + 4) return 0;
		memcpy(mask, p + h, 4);
		h += 4;
	}
	if (ws->rx_len < h + plen) return 0;
	uint8_t *payload = p + h;
	if (masked) {
		for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
	}

	switch (opcode) {
	case 0x0: // continuation
	case 0x1: // text
	case 0x2: // binary
		if (opcode != 0) {
			ws->msg_len = 0;
			ws->msg_opcode = opcode;
		}
		if (ws->msg_len + plen > WS_MAX_MESSAGE) return -1;
		if (ensure(&ws->msg, &ws->msg_cap, ws->msg_len + plen + 1) != 0) return -1;
		memcpy(ws->msg + ws->msg_len, payload, plen);
		ws->msg_len += plen;
		if (fin) {
			ws->msg[ws->msg_len] = 0;
			ws->msg_ready = ws->msg_opcode == 0x1;
		}
		break;
	case 0x8: // close
		ws->open = false;
		break;
	case 0x9: // ping
		send_frame(ws, 0xA, payload, plen);
		break;
	default: // pong and reserved opcodes are ignored
		break;
	}
	size_t consumed = h + plen;
	memmove(ws->rx, ws->rx + consumed, ws->rx_len - consumed);
	ws->rx_len -= consumed;
	return 1;
}

int ws_poll(WsConn *ws, char **out, size_t *out_len)
{
	if (ws->fd < 0) return -1;
	ws->msg_ready = false;

	// Parse what is buffered first so a burst of small frames does not wait on the socket.
	for (;;) {
		int t = take_frame(ws);
		if (t < 0) {
			ws->open = false;
			return -1;
		}
		if (ws->msg_ready) {
			*out = (char *)ws->msg;
			*out_len = ws->msg_len;
			return 1;
		}
		if (t == 0) break;
	}
	if (!ws->open) return -1;

	if (ensure(&ws->rx, &ws->rx_cap, ws->rx_len + 16384) != 0) return -1;
	int r = sock_recv(ws->fd, ws->rx + ws->rx_len, ws->rx_cap - ws->rx_len);
	if (r == 0 || r == -1) {
		ws->open = false;
		return -1;
	}
	if (r > 0) {
		ws->rx_len += (size_t)r;
		ws->last_rx = plat_ms();
		for (;;) {
			int t = take_frame(ws);
			if (t < 0) {
				ws->open = false;
				return -1;
			}
			if (ws->msg_ready) {
				*out = (char *)ws->msg;
				*out_len = ws->msg_len;
				return 1;
			}
			if (t == 0) break;
		}
	}
	return ws->open ? 0 : -1;
}

void ws_close(WsConn *ws)
{
	if (ws->fd >= 0) {
		if (ws->open) {
			uint8_t code[2] = {0x03, 0xE8}; // 1000 normal closure
			send_frame(ws, 0x8, code, 2);
		}
		sock_close(ws->fd);
	}
	ws->fd = -1;
	ws->open = false;
	free(ws->rx);
	free(ws->msg);
	ws->rx = ws->msg = NULL;
	ws->rx_len = ws->rx_cap = ws->msg_len = ws->msg_cap = 0;
}
