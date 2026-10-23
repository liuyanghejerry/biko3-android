/* Guest thread-local slots for the Android host.
 *
 * The rewritten guest reads its four thread slots (x18, tpidr_el0, tpidrro_el0
 * and the gate entry) as [tpidrro_el0 + offset], where the offset comes from
 * the image's key page and is written by the loader. On iOS those offsets are
 * the host pthread key numbers * 8 because Darwin's TSD lives at TP + 8*key.
 * bionic's TSD does not, so on Android the host points tpidrro_el0 at its own
 * per-thread slot block and keeps the four values there. */
#pragma once
#include <pthread.h>
#include <stdint.h>

enum { DM_TSD_K18 = 0, DM_TSD_KTLS = 1, DM_TSD_KTLR = 2, DM_TSD_KGATE = 3, DM_TSD_N = 4 };

/* Called by the loader once the four keys exist; maps key -> slot. */
void dm_tsd_register(int slot, pthread_key_t key);

/* Key page value for slot i: the guest reads [tpidr_el0 + offset]. */
uint64_t dm_tsd_offset(int slot);

/* Accessors used by the host instead of pthread_setspecific/getspecific. */
int dm_tsd_setspecific(pthread_key_t key, const void *value);
void *dm_tsd_getspecific(pthread_key_t key);

/* Read slot i the way the guest does: [tpidr_el0 + key page offset]. */
uint64_t dm_tsd_read_guest(int slot);

/* Host-side slot array base (diagnostics). */
void *dm_tsd_slots_base(void);

/* This thread's TP-relative slot offset (diagnostics). */
int64_t dm_tsd_thread_offset(void);

/* Compute/validate this thread's slot offset; call before guest code. */
void dm_tsd_attach(void);
