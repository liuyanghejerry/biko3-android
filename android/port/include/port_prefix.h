/* Force-included in every upstream host translation unit: declares the Darwin
 * functions the host calls without including their headers, and routes the two
 * pthread TSD accessors through the Android guest-TLS registry. */
#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Darwin pthread extensions (implemented in android_shims.c). */
void *pthread_get_stackaddr_np(pthread_t thread);
size_t pthread_get_stacksize_np(pthread_t thread);
uint64_t clock_gettime_nsec_np(int clock_id);

/* Guest thread slots: on Darwin the host stores them in the pthread TSD, which
 * happens to be tpidrro_el0 + 8*key. Android's bionic stores TSD elsewhere, so
 * the host keeps the four slots in its own per-thread block and points
 * tpidrro_el0 at it (see dm_tsd.c). */
#include <dm_tsd.h>
#define pthread_setspecific(key, value) dm_tsd_setspecific((key), (value))
#define pthread_getspecific(key) dm_tsd_getspecific((key))
