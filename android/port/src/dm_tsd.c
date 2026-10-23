/* Per-thread guest slots for Android.
 *
 * The rewritten guest reads its four thread slots (x18, tpidr_el0, tpidrro_el0
 * and the gate entry) as [tpidrro_el0 + offset], where the offset comes from
 * the key page the loader fills in. iOS uses tpidrro_el0 because Darwin's TSD
 * lives at TP + 8*key. Android has neither that layout nor, on translated x86
 * hosts, a writable tpidrro_el0, so the port rewrites the guest's
 * "mrs xN, tpidrro_el0" to "mrs xN, tpidr_el0" (same instruction width;
 * tools/patch_android_guest_image.py).
 *
 * The slot memory then has to be reachable at a *fixed* TP-relative offset on
 * every thread. A plain TLS variable cannot do that: bionic arranges dynamic
 * TLS blocks per thread, and the x86 emulator's ARM translation hands each
 * thread a different distance between TP and its TLS blocks.
 *
 * So each guest thread gets a private copy of its ARM TLS block (TCB, the
 * executable's TLS segment and the words just above it), the slots are placed
 * a fixed distance below that copy, and the thread pointer is moved into the
 * copy (msr tpidr_el0). Host code keeps working because the copied TCB holds
 * the same DTV, bionic_tls and pthread pointers; the guest reads
 * [TP - K + slot*8] and lands in the slot area. K is a compile-time constant,
 * so the key page offsets are the same on every thread and platform. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dm_tsd.h"

/* port_prefix.h force-includes this file's users and defines these macros; the
 * implementation below must use the real libc accessors. */
#undef pthread_setspecific
#undef pthread_getspecific

#define DM_TSD_DISTANCE (256 * 1024)   /* K: slots this far below the copied TP */
#define DM_TSD_SLOT_BYTES 64           /* room for dm_guest_slots */
#define DM_TSD_LOWER (64 * 1024)       /* bytes of TLS below TP to copy */
#define DM_TSD_UPPER (128 * 1024)      /* TCB, executable segment, bionic_tls */

__thread uint64_t dm_guest_slots[8] __attribute__((used, visibility("hidden")));

static pthread_key_t g_keys[DM_TSD_N];
static pthread_key_t g_clone_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static int g_slot_ready;
static int g_clone_failed;

static void key_init(void) { pthread_key_create(&g_clone_key, NULL); }

static uint64_t thread_pointer(void) {
#if defined(__aarch64__)
    uint64_t value = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

/* Bounds of the mapping that contains address; 0 when it cannot be found. */
static int mapping_bounds(uintptr_t address, uintptr_t *start, uintptr_t *end) {
    char line[512];
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    int found = 0;
    while (fgets(line, sizeof line, maps)) {
        unsigned long long low = 0, high = 0;
        if (sscanf(line, "%llx-%llx", &low, &high) != 2) continue;
        if (low <= address && address < high) {
            *start = (uintptr_t)low;
            *end = (uintptr_t)high;
            found = 1;
            break;
        }
    }
    fclose(maps);
    return found;
}

static uint8_t *clone_for_thread(void) {
    uint8_t *clone = pthread_getspecific(g_clone_key);
    if (clone) return clone;
    if (g_clone_failed) return NULL;
    uintptr_t tp = (uintptr_t)thread_pointer();
    uintptr_t start = 0, end = 0;
    if (!tp || !mapping_bounds(tp, &start, &end) || start >= tp) {
        g_clone_failed = 1;
        fprintf(stderr, "[biko3] guest TLS: cannot locate the TLS mapping (tp=%#llx)\n",
                (unsigned long long)tp);
        return NULL;
    }
    /* Bionic places the TCB and static TLS at the top of the thread's mapping
     * (above the stack), so only a small window around TP belongs to the TLS;
     * on the x86 emulator's translator the same window is inside a small
     * mapping. Copy a bounded window on both sides instead of everything below
     * TP (which is the whole stack on a device). */
    size_t below = tp - start;
    if (below > DM_TSD_LOWER) below = DM_TSD_LOWER;
    if (below + DM_TSD_SLOT_BYTES >= DM_TSD_DISTANCE) {
        g_clone_failed = 1;
        fprintf(stderr, "[biko3] guest TLS: %zu bytes below TP do not fit the slot window\n", below);
        return NULL;
    }
    size_t upper = end - tp;
    if (upper > DM_TSD_UPPER) upper = DM_TSD_UPPER;
    uint8_t *buffer = mmap(NULL, DM_TSD_DISTANCE + DM_TSD_SLOT_BYTES + DM_TSD_UPPER + 4096,
                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buffer == MAP_FAILED) {
        g_clone_failed = 1;
        fprintf(stderr, "[biko3] guest TLS: clone allocation failed\n");
        return NULL;
    }
    uint8_t *copy = buffer + DM_TSD_DISTANCE - below;
    memcpy(copy, (const void *)(tp - below), below + upper);
    clone = buffer;   /* slots at the buffer base; TP is buffer + DM_TSD_DISTANCE */
    pthread_setspecific(g_clone_key, clone);
#if defined(__aarch64__)
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(buffer + DM_TSD_DISTANCE));
#endif
    fprintf(stderr, "[biko3] guest TLS: cloned %zu+%zu bytes around TP, slots at TP-%d\n",
            below, upper, (int)DM_TSD_DISTANCE);
    return clone;
}

void dm_tsd_attach(void) {
    pthread_once(&g_key_once, key_init);
    (void)clone_for_thread();
}

void dm_tsd_register(int slot, pthread_key_t key) {
    if (slot < 0 || slot >= DM_TSD_N) return;
    g_keys[slot] = key;
    g_slot_ready = 1;
}

uint64_t dm_tsd_offset(int slot) {
    if (!thread_pointer()) return (uint64_t)(uintptr_t)&dm_guest_slots[slot + 1];
    return (uint64_t)(-(int64_t)DM_TSD_DISTANCE) + (uint64_t)slot * 8;
}

static int16_t slot_for_key(pthread_key_t key) {
    /* bionic encodes keys as 32-bit values with the high bit set, so compare
     * instead of indexing by key. */
    if (!g_slot_ready) return -1;
    for (int slot = 0; slot < DM_TSD_N; slot++)
        if (g_keys[slot] == key) return (int16_t)slot;
    return -1;
}

int dm_tsd_setspecific(pthread_key_t key, const void *value) {
    int slot = slot_for_key(key);
    if (slot < 0) return pthread_setspecific(key, value);
    uint8_t *clone = clone_for_thread();
    if (!clone) return pthread_setspecific(key, value);
    memcpy(clone + slot * 8, &value, sizeof value);
    return 0;
}

void *dm_tsd_getspecific(pthread_key_t key) {
    int slot = slot_for_key(key);
    if (slot < 0) return pthread_getspecific(key);
    uint8_t *clone = clone_for_thread();
    if (!clone) return pthread_getspecific(key);
    void *value = NULL;
    memcpy(&value, clone + slot * 8, sizeof value);
    return value;
}

void *dm_tsd_slots_base(void) {
    uint8_t *clone = clone_for_thread();
    return clone ? clone : (void *)&dm_guest_slots[0];
}

/* Read slot i the way the guest does: [tpidr_el0 + key page offset]. */
uint64_t dm_tsd_read_guest(int slot) {
    uint64_t value = 0;
    uint64_t at = thread_pointer() + dm_tsd_offset(slot);
    memcpy(&value, (const void *)(uintptr_t)at, sizeof value);
    return value;
}

int64_t dm_tsd_thread_offset(void) {
    return (int64_t)((uintptr_t)dm_tsd_slots_base() - (uintptr_t)thread_pointer());
}
