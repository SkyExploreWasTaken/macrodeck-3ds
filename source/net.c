#include "net.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#ifdef __3DS__
#include <malloc.h>
#define SOC_BUFFERSIZE 0x100000
static u32 *soc_buffer;
#endif

bool net_init(void)
{
#ifdef __3DS__
	soc_buffer = memalign(0x1000, SOC_BUFFERSIZE);
	if (!soc_buffer) return false;
	if (R_FAILED(socInit(soc_buffer, SOC_BUFFERSIZE))) {
		free(soc_buffer);
		soc_buffer = NULL;
		return false;
	}
#endif
	return true;
}

void net_exit(void)
{
#ifdef __3DS__
	socExit();
	free(soc_buffer);
	soc_buffer = NULL;
#endif
}

static char g_net_error[96];

const char *net_error(void)
{
	return g_net_error;
}

void net_set_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_net_error, sizeof g_net_error, fmt, ap);
	va_end(ap);
}

static void set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) flags = 0;
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int tcp_connect(const char *host, int port, int timeout_ms)
{
	g_net_error[0] = 0;
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		struct hostent *he = gethostbyname(host);
		if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
			net_set_error("unknown host");
			return -1;
		}
		memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof addr.sin_addr);
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		net_set_error("no free socket, error %d", errno);
		return -1;
	}
#ifdef __3DS__
	// The 3DS stack tends to hand out the same local port again right away. The computer still holds the last
	// connection on that port in TIME_WAIT and drops the new one, so pick a random high port ourselves.
	{
		struct sockaddr_in local;
		memset(&local, 0, sizeof local);
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		uint16_t r = 0;
		plat_random(&r, sizeof r);
		local.sin_port = htons((unsigned short)(49152 + r % 16000));
		bind(fd, (struct sockaddr *)&local, sizeof local); // on failure the stack picks a port as usual
	}
#endif
	set_nonblocking(fd);

	int rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
	if (rc < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) {
		net_set_error("connect failed, error %d", errno);
		close(fd);
		return -1;
	}
	if (rc < 0) {
		int w = sock_wait(fd, 2, timeout_ms);
		int err = 0;
		socklen_t elen = sizeof err;
		if (w != 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err != 0) {
			net_set_error(err == ECONNREFUSED ? "connection refused" : "connect failed, error %d", err);
			close(fd);
			return -1;
		}
		if (w != 1) {
			net_set_error(w == 0 ? "no answer to connect" : "connect failed while waiting");
			close(fd);
			return -1;
		}
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

void sock_close(int fd)
{
	if (fd >= 0) close(fd);
}

int sock_wait(int fd, int events, int timeout_ms)
{
	struct pollfd p;
	p.fd = fd;
	p.events = (events & 1 ? POLLIN : 0) | (events & 2 ? POLLOUT : 0);
	p.revents = 0;
	int rc = poll(&p, 1, timeout_ms);
	if (rc < 0) return -1;
	if (rc == 0) return 0;
	if (p.revents & (POLLERR | POLLNVAL)) return -1;
	// POLLHUP with pending data still counts as readable; recv() then reports the close.
	return 1;
}

int sock_send_all(int fd, const void *buf, size_t len, int timeout_ms)
{
	const char *p = buf;
	uint64_t deadline = plat_ms() + (uint64_t)timeout_ms;
	while (len > 0) {
		ssize_t n = send(fd, p, len, 0);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
			int left = (int)(deadline - plat_ms());
			if ((int64_t)(deadline - plat_ms()) <= 0) return -1;
			if (sock_wait(fd, 2, left) < 0) return -1;
			continue;
		}
		return -1;
	}
	return 0;
}

int sock_recv(int fd, void *buf, size_t len)
{
	ssize_t n = recv(fd, buf, len, 0);
	if (n > 0) return (int)n;
	if (n == 0) return 0;
	if (errno == EWOULDBLOCK || errno == EAGAIN) return -2;
	return -1;
}

void net_local_ip(char *out, size_t len)
{
	out[0] = 0;
#ifdef __3DS__
	u32 ip = (u32)gethostid();
	if (ip == 0) return;
	struct in_addr a;
	a.s_addr = ip;
	snprintf(out, len, "%s", inet_ntoa(a));
#else
	snprintf(out, len, "127.0.0.1");
#endif
}
