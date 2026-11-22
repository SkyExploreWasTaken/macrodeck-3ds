#pragma once

typedef struct {
	char name[64]; // the computer's name from the TXT record, or the instance label
	char ip[20];
	int port;
	char version[24];
} MdnsHost;

// Browses _macrodeck._tcp.local for timeout_ms. Returns how many hosts were found (at most max).
int mdns_discover(MdnsHost *out, int max, int timeout_ms);
