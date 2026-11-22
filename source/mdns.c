// Minimal DNS-SD browser for the "_macrodeck._tcp" service Macro Deck 3 advertises.
// Queries are sent from an ephemeral port, so responders answer us by unicast ("legacy unicast",
// RFC 6762 section 6.7); we also join the mDNS group on 5353 when the platform allows it.
#include "mdns.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define SERVICE "_macrodeck._tcp.local"

typedef struct {
	char instance[128];
	char target[128];
	int port;
	char txt_name[64];
	char txt_version[24];
	uint32_t src_ip;
} Instance;

typedef struct {
	char host[128];
	uint32_t ip;
} ARecord;

static int build_query(uint8_t *buf, uint16_t id)
{
	int n = 0;
	buf[n++] = (uint8_t)(id >> 8);
	buf[n++] = (uint8_t)id;
	buf[n++] = 0; buf[n++] = 0;   // flags: standard query
	buf[n++] = 0; buf[n++] = 1;   // QDCOUNT
	for (int i = 0; i < 6; i++) buf[n++] = 0;
	const char *p = SERVICE;
	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);
		buf[n++] = (uint8_t)len;
		memcpy(buf + n, p, len);
		n += (int)len;
		p += len;
		if (*p == '.') p++;
	}
	buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 12;      // PTR
	buf[n++] = 0x80; buf[n++] = 0x01; // QU bit + IN
	return n;
}

// Reads a (possibly compressed) name at *off into out ("a.b.c"). Returns 0 on success.
static int read_name(const uint8_t *pkt, int len, int *off, char *out, size_t out_len)
{
	int pos = *off, jumped = 0, hops = 0;
	size_t o = 0;
	out[0] = 0;
	while (pos < len) {
		uint8_t l = pkt[pos];
		if (l == 0) {
			pos++;
			break;
		}
		if ((l & 0xC0) == 0xC0) {
			if (pos + 1 >= len || ++hops > 16) return -1;
			int ptr = ((l & 0x3F) << 8) | pkt[pos + 1];
			if (!jumped) *off = pos + 2;
			jumped = 1;
			pos = ptr;
			continue;
		}
		pos++;
		if (pos + l > len) return -1;
		if (o && o + 1 < out_len) out[o++] = '.';
		for (int i = 0; i < l && o + 1 < out_len; i++) out[o++] = (char)pkt[pos + i];
		out[o] = 0;
		pos += l;
	}
	if (!jumped) *off = pos;
	return 0;
}

static Instance *find_instance(Instance *list, int *count, int max, const char *name, int create)
{
	for (int i = 0; i < *count; i++) {
		if (strcasecmp(list[i].instance, name) == 0) return &list[i];
	}
	if (!create || *count >= max) return NULL;
	Instance *in = &list[(*count)++];
	memset(in, 0, sizeof *in);
	snprintf(in->instance, sizeof in->instance, "%s", name);
	return in;
}

static void parse_packet(const uint8_t *pkt, int len, uint32_t src_ip, Instance *inst, int *ninst, int maxinst,
	ARecord *arecs, int *nrec, int maxrec)
{
	if (len < 12) return;
	int qd = (pkt[4] << 8) | pkt[5];
	int rr = ((pkt[6] << 8) | pkt[7]) + ((pkt[8] << 8) | pkt[9]) + ((pkt[10] << 8) | pkt[11]);
	int off = 12;
	char name[256];
	for (int i = 0; i < qd; i++) {
		if (read_name(pkt, len, &off, name, sizeof name) != 0) return;
		off += 4;
	}
	for (int i = 0; i < rr && off + 10 <= len; i++) {
		if (read_name(pkt, len, &off, name, sizeof name) != 0) return;
		if (off + 10 > len) return;
		int type = (pkt[off] << 8) | pkt[off + 1];
		int rdlen = (pkt[off + 8] << 8) | pkt[off + 9];
		off += 10;
		if (off + rdlen > len) return;
		int rd = off;
		char data[256];
		if (type == 12 && strcasecmp(name, SERVICE) == 0) { // PTR -> instance
			int p = rd;
			if (read_name(pkt, len, &p, data, sizeof data) == 0) {
				Instance *in = find_instance(inst, ninst, maxinst, data, 1);
				if (in && !in->src_ip) in->src_ip = src_ip;
			}
		} else if (type == 33 && rdlen >= 6) { // SRV
			Instance *in = find_instance(inst, ninst, maxinst, name, 1);
			if (in) {
				in->port = (pkt[rd + 4] << 8) | pkt[rd + 5];
				int p = rd + 6;
				read_name(pkt, len, &p, in->target, sizeof in->target);
				if (!in->src_ip) in->src_ip = src_ip;
			}
		} else if (type == 16) { // TXT
			Instance *in = find_instance(inst, ninst, maxinst, name, 1);
			int p = rd;
			while (in && p < rd + rdlen) {
				int l = pkt[p++];
				if (p + l > rd + rdlen) break;
				char kv[128];
				int c = l < (int)sizeof kv - 1 ? l : (int)sizeof kv - 1;
				memcpy(kv, pkt + p, c);
				kv[c] = 0;
				if (strncasecmp(kv, "name=", 5) == 0) snprintf(in->txt_name, sizeof in->txt_name, "%s", kv + 5);
				if (strncasecmp(kv, "version=", 8) == 0) snprintf(in->txt_version, sizeof in->txt_version, "%s", kv + 8);
				p += l;
			}
		} else if (type == 1 && rdlen == 4 && *nrec < maxrec) { // A
			ARecord *a = &arecs[(*nrec)++];
			snprintf(a->host, sizeof a->host, "%s", name);
			memcpy(&a->ip, pkt + rd, 4);
		}
		off = rd + rdlen;
	}
}

static int open_socket(int port, int join)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in local;
	memset(&local, 0, sizeof local);
	local.sin_family = AF_INET;
	local.sin_port = htons((unsigned short)port);
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) {
		close(fd);
		return -1;
	}
#ifdef IP_ADD_MEMBERSHIP
	if (join) {
		struct ip_mreq mreq;
		memset(&mreq, 0, sizeof mreq);
		mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
		mreq.imr_interface.s_addr = htonl(INADDR_ANY);
		setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);
	}
#else
	(void)join;
#endif
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
	return fd;
}

int mdns_discover(MdnsHost *out, int max, int timeout_ms)
{
	int fds[2];
	fds[0] = open_socket(0, 0);       // legacy unicast answers come back here
	fds[1] = open_socket(5353, 1);    // multicast answers, when we may bind 5353
	if (fds[0] < 0 && fds[1] < 0) return 0;

	uint8_t query[128];
	uint16_t id;
	plat_random(&id, sizeof id);
	int qlen = build_query(query, id);
	struct sockaddr_in group;
	memset(&group, 0, sizeof group);
	group.sin_family = AF_INET;
	group.sin_port = htons(5353);
	group.sin_addr.s_addr = inet_addr("224.0.0.251");

	Instance inst[16];
	ARecord arecs[32];
	int ninst = 0, nrec = 0;
	uint8_t *pkt = malloc(9000);
	if (!pkt) {
		for (int i = 0; i < 2; i++) if (fds[i] >= 0) close(fds[i]);
		return 0;
	}

	uint64_t start = plat_ms();
	uint64_t next_send = start;
	int sends = 0;
	while (plat_ms() - start < (uint64_t)timeout_ms) {
		if (plat_ms() >= next_send && sends < 3) {
			for (int i = 0; i < 2; i++) {
				if (fds[i] >= 0) sendto(fds[i], query, qlen, 0, (struct sockaddr *)&group, sizeof group);
			}
			sends++;
			next_send = plat_ms() + 700;
		}
		struct pollfd p[2];
		int np = 0;
		for (int i = 0; i < 2; i++) {
			if (fds[i] >= 0) {
				p[np].fd = fds[i];
				p[np].events = POLLIN;
				p[np].revents = 0;
				np++;
			}
		}
		if (poll(p, np, 100) <= 0) continue;
		for (int i = 0; i < np; i++) {
			if (!(p[i].revents & POLLIN)) continue;
			for (;;) {
				struct sockaddr_in from;
				socklen_t flen = sizeof from;
				int n = (int)recvfrom(p[i].fd, pkt, 9000, 0, (struct sockaddr *)&from, &flen);
				if (n <= 0) break;
				if (n < 12 || !(pkt[2] & 0x80)) continue; // only responses
				parse_packet(pkt, n, from.sin_addr.s_addr, inst, &ninst, 16, arecs, &nrec, 32);
			}
		}
	}
	free(pkt);
	for (int i = 0; i < 2; i++) if (fds[i] >= 0) close(fds[i]);

	int count = 0;
	for (int i = 0; i < ninst && count < max; i++) {
		Instance *in = &inst[i];
		if (!in->port) continue;
		uint32_t ip = 0;
		for (int r = 0; r < nrec; r++) {
			if (in->target[0] && strcasecmp(arecs[r].host, in->target) == 0) {
				ip = arecs[r].ip;
				break;
			}
		}
		if (!ip) ip = in->src_ip;
		if (!ip) continue;
		MdnsHost *h = &out[count];
		memset(h, 0, sizeof *h);
		struct in_addr a;
		a.s_addr = ip;
		snprintf(h->ip, sizeof h->ip, "%s", inet_ntoa(a));
		h->port = in->port;
		if (in->txt_name[0]) {
			snprintf(h->name, sizeof h->name, "%s", in->txt_name);
		} else {
			snprintf(h->name, sizeof h->name, "%s", in->instance);
			char *dot = strstr(h->name, "._macrodeck");
			if (dot) *dot = 0;
		}
		snprintf(h->version, sizeof h->version, "%s", in->txt_version);
		int dup = 0;
		for (int k = 0; k < count; k++) {
			if (strcmp(out[k].ip, h->ip) == 0 && out[k].port == h->port) dup = 1;
		}
		if (!dup) count++;
	}
	return count;
}
