#pragma once

#include <stddef.h>
#include <stdbool.h>

bool net_init(void);
void net_exit(void);

// Connects a TCP socket within timeout_ms. Returns the fd (left non-blocking) or -1.
int tcp_connect(const char *host, int port, int timeout_ms);
void sock_close(int fd);

// Sends everything or fails. Returns 0 on success.
int sock_send_all(int fd, const void *buf, size_t len, int timeout_ms);

// Waits for readability (events=1) or writability (events=2). 1 ready, 0 timeout, -1 error.
int sock_wait(int fd, int events, int timeout_ms);

// Reads what is available. >0 bytes, 0 closed, -1 error, -2 would block.
int sock_recv(int fd, void *buf, size_t len);

// The console's own IPv4 address as dotted text, or "" when offline.
void net_local_ip(char *out, size_t len);
