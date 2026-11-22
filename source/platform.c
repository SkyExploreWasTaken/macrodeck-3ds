#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef __3DS__

void plat_mutex_init(plat_mutex *m) { LightLock_Init(m); }
void plat_mutex_lock(plat_mutex *m) { LightLock_Lock(m); }
void plat_mutex_unlock(plat_mutex *m) { LightLock_Unlock(m); }

uint64_t plat_ms(void) { return osGetTime(); }

void plat_sleep_ms(int ms) { svcSleepThread((s64)ms * 1000000LL); }

static uint64_t rng_state;

void plat_random(void *out, size_t len)
{
	// ps:ps gives real entropy; fall back to a tick-seeded xorshift if it is unavailable.
	if (R_SUCCEEDED(psInit())) {
		Result rc = PS_GenerateRandomBytes(out, len);
		psExit();
		if (R_SUCCEEDED(rc)) return;
	}
	if (!rng_state) rng_state = svcGetSystemTick() ^ 0x9E3779B97F4A7C15ULL;
	uint8_t *p = out;
	for (size_t i = 0; i < len; i++) {
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 7;
		rng_state ^= rng_state << 17;
		p[i] = (uint8_t)rng_state;
	}
}

const char *plat_data_dir(void) { return "sdmc:/3ds/MacroDeck3DS"; }

struct plat_thread {
	Thread t;
};

plat_thread *plat_thread_start(void (*fn)(void *), void *arg)
{
	plat_thread *pt = malloc(sizeof *pt);
	if (!pt) return NULL;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// Lower priority than the UI thread, same core: the UI yields every vblank, so the worker gets time.
	pt->t = threadCreate(fn, arg, 256 * 1024, prio + 1, -2, false);
	if (!pt->t) {
		free(pt);
		return NULL;
	}
	return pt;
}

void plat_thread_join(plat_thread *pt)
{
	if (!pt) return;
	threadJoin(pt->t, U64_MAX);
	threadFree(pt->t);
	free(pt);
}

#else

#include <unistd.h>

void plat_mutex_init(plat_mutex *m) { pthread_mutex_init(m, NULL); }
void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(m); }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }

uint64_t plat_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void plat_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

void plat_random(void *out, size_t len)
{
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		size_t n = fread(out, 1, len, f);
		fclose(f);
		if (n == len) return;
	}
	uint8_t *p = out;
	for (size_t i = 0; i < len; i++) p[i] = (uint8_t)rand();
}

struct plat_thread {
	pthread_t t;
	void (*fn)(void *);
	void *arg;
};

static void *thread_tramp(void *p)
{
	struct plat_thread *pt = p;
	pt->fn(pt->arg);
	return NULL;
}

plat_thread *plat_thread_start(void (*fn)(void *), void *arg)
{
	plat_thread *pt = malloc(sizeof *pt);
	if (!pt) return NULL;
	pt->fn = fn;
	pt->arg = arg;
	if (pthread_create(&pt->t, NULL, thread_tramp, pt) != 0) {
		free(pt);
		return NULL;
	}
	return pt;
}

void plat_thread_join(plat_thread *pt)
{
	if (!pt) return;
	pthread_join(pt->t, NULL);
	free(pt);
}

const char *plat_data_dir(void)
{
	const char *d = getenv("MD3DS_DATA");
	return d ? d : "./md3ds-data";
}

#endif

void plat_mkdirs(const char *path)
{
	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s", path);
	size_t len = strlen(tmp);
	if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
	// Skip a "sdmc:" style device prefix so we never try to mkdir it.
	char *start = strchr(tmp, ':');
	start = start ? start + 2 : tmp + 1;
	for (char *p = start; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
}
