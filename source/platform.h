// Small portability layer so the network/protocol core also builds on a desktop host for testing.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __3DS__
#include <3ds.h>
typedef LightLock plat_mutex;
#else
#include <pthread.h>
typedef pthread_mutex_t plat_mutex;
#endif

void plat_mutex_init(plat_mutex *m);
void plat_mutex_lock(plat_mutex *m);
void plat_mutex_unlock(plat_mutex *m);

uint64_t plat_ms(void);
void plat_sleep_ms(int ms);
void plat_random(void *out, size_t len);

typedef struct plat_thread plat_thread;
plat_thread *plat_thread_start(void (*fn)(void *), void *arg);
void plat_thread_join(plat_thread *t);

// Directory where config.json lives (created on demand).
const char *plat_data_dir(void);
void plat_mkdirs(const char *path);
