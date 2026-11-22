#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
	int fd;
	bool open;
	uint8_t *rx;     // raw bytes not yet parsed into frames
	size_t rx_len, rx_cap;
	uint8_t *msg;    // message being reassembled from fragments
	size_t msg_len, msg_cap;
	int msg_opcode;
	bool msg_ready;
	uint64_t last_rx;
} WsConn;

// Opens ws://host:port/path offering one subprotocol. Returns 0 on success.
int ws_connect(WsConn *ws, const char *host, int port, const char *path, const char *subprotocol, int timeout_ms);

int ws_send_text(WsConn *ws, const char *text, size_t len);

// Pumps the socket. Returns 1 with *out pointing at a NUL-terminated text message (valid until the next
// call), 0 when nothing complete arrived yet, -1 when the connection is gone.
int ws_poll(WsConn *ws, char **out, size_t *out_len);

void ws_close(WsConn *ws);
