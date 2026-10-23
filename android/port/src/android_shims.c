/* Android implementations of the small Darwin API surface the host uses.
 * Everything here is either a clock/TSD/permission adapter or a diagnostic
 * that Android cannot provide (cross-thread register dumps); the diagnostics
 * fail closed and the guest is unaffected. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/getsect.h>
#include <libkern/OSCacheControl.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>

/* ---------- clocks ---------- */
uint64_t mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
int mach_timebase_info(mach_timebase_info_data_t *info) {
    if (info) { info->numer = 1; info->denom = 1; }
    return 0;
}
int mach_wait_until(uint64_t deadline) {
    struct timespec ts = { (time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    return 0;
}
uint64_t clock_gettime_nsec_np(int clock_id) {
    struct timespec ts;
    if (clock_gettime((clockid_t)clock_id, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------- Mach stubs ---------- */
mach_port_t mach_task_self(void) { return (mach_port_t)getpid(); }
mach_port_t mach_thread_self(void) { return (mach_port_t)(uintptr_t)pthread_self(); }
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) { (void)task; (void)name; return KERN_SUCCESS; }
mach_port_t pthread_mach_thread_np(pthread_t thread) { (void)thread; return 0; }
kern_return_t thread_info(thread_act_t thread, thread_state_flavor_t flavor, thread_info_t info, mach_msg_type_number_t *count) {
    (void)thread; (void)flavor; (void)info; (void)count; return KERN_FAILURE;
}
kern_return_t thread_get_state(thread_act_t thread, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t *count) {
    (void)thread; (void)flavor; (void)state; (void)count; return KERN_FAILURE;
}
kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *count) {
    (void)task;
    if (list) *list = NULL;
    if (count) *count = 0;
    return KERN_FAILURE;
}
kern_return_t thread_suspend(thread_act_t thread) { (void)thread; return KERN_FAILURE; }
kern_return_t thread_resume(thread_act_t thread) { (void)thread; return KERN_FAILURE; }
kern_return_t vm_allocate(mach_port_t task, vm_address_t *address, vm_size_t size, int flags) {
    (void)task; (void)address; (void)size; (void)flags; return KERN_FAILURE;
}
kern_return_t vm_deallocate(mach_port_t task, vm_address_t address, vm_size_t size) {
    (void)task; (void)address; (void)size; return KERN_SUCCESS;
}
kern_return_t vm_remap(mach_port_t target, vm_address_t *address, vm_size_t size, vm_size_t mask, int flags,
                       mach_port_t src, vm_address_t src_addr, boolean_t copy, vm_prot_t *cur, vm_prot_t *max,
                       vm_inherit_t inherit) {
    (void)target; (void)address; (void)size; (void)mask; (void)flags; (void)src; (void)src_addr;
    (void)copy; (void)cur; (void)max; (void)inherit; return KERN_FAILURE;
}
kern_return_t vm_region(vm_address_t *address, vm_size_t *size, vm_region_flavor_t flavor,
                        vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *object) {
    (void)address; (void)size; (void)flavor; (void)info; (void)count; (void)object; return KERN_FAILURE;
}
/* In-process read of a possibly invalid address: /proc/self/mem returns EIO
 * instead of raising SIGSEGV, which is exactly the fail-closed behaviour the
 * backtrace walker needs. */
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t address, vm_size_t size,
                                vm_address_t data, vm_size_t *out_size) {
    static int fd = -1;
    (void)task;
    if (out_size) *out_size = 0;
    if (fd < 0) fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return KERN_FAILURE;
    ssize_t n = pread(fd, (void *)data, size, (off_t)address);
    if (n < 0 || (vm_size_t)n != size) return KERN_FAILURE;
    if (out_size) *out_size = size;
    return KERN_SUCCESS;
}

/* ---------- pthread extensions ---------- */
void *pthread_get_stackaddr_np(pthread_t thread) {
    pthread_attr_t attr;
    void *base = NULL;
    size_t size = 0;
    if (pthread_getattr_np(thread, &attr)) return NULL;
    if (pthread_attr_getstack(&attr, &base, &size)) base = NULL;
    pthread_attr_destroy(&attr);
    return base ? (void *)((char *)base + size) : NULL;
}
size_t pthread_get_stacksize_np(pthread_t thread) {
    pthread_attr_t attr;
    void *base = NULL;
    size_t size = 0;
    if (pthread_getattr_np(thread, &attr)) return 0;
    if (pthread_attr_getstack(&attr, &base, &size)) size = 0;
    pthread_attr_destroy(&attr);
    return size;
}
int pthread_set_qos_class_self_np(qos_class_t cls, int relative_priority) {
    (void)cls; (void)relative_priority; return 0;
}
pthread_override_t pthread_override_qos_class_start_np(pthread_t thread, qos_class_t cls, int relative_priority) {
    (void)thread; (void)cls; (void)relative_priority; return NULL;
}
int pthread_override_qos_class_end_np(pthread_override_t override) { (void)override; return 0; }
int pthread_get_qos_class_np(pthread_t thread, qos_class_t *cls, int *relative_priority) {
    (void)thread;
    if (cls) *cls = QOS_CLASS_UNSPECIFIED;
    if (relative_priority) *relative_priority = 0;
    return 0;
}

/* ---------- cache maintenance ---------- */
void sys_icache_invalidate(void *start, size_t len) { __builtin___clear_cache((char *)start, (char *)start + len); }

/* ---------- ELF section lookup (replaces Mach-O getsectiondata) ---------- */
struct mach_header { unsigned int magic; };
const struct mach_header dm_fake_mh_execute_header = { 0 };

extern const unsigned char __start_swhle[] __attribute__((weak));
extern const unsigned char __stop_swhle[] __attribute__((weak));
extern const unsigned char __start_swforce[] __attribute__((weak));
extern const unsigned char __stop_swforce[] __attribute__((weak));

static unsigned long section_len(const unsigned char *start, const unsigned char *stop) {
    return (start && stop && stop > start) ? (unsigned long)(stop - start) : 0;
}
unsigned char *getsectiondata(const struct mach_header *mh, const char *segname,
                              const char *sectname, unsigned long *size) {
    (void)mh; (void)segname;
    if (!sectname || !size) return NULL;
    if (!strcmp(sectname, "swhle")) {
        *size = section_len(__start_swhle, __stop_swhle);
        return *size ? (unsigned char *)__start_swhle : NULL;
    }
    if (!strcmp(sectname, "swforce")) {
        *size = section_len(__start_swforce, __stop_swforce);
        return *size ? (unsigned char *)__start_swforce : NULL;
    }
    return NULL;
}

/* ---------- sysctl: process start time ---------- */
static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return 0;
}
int sysctl(int *name, unsigned int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!name || namelen < 4 || name[0] != CTL_KERN || name[1] != KERN_PROC ||
        name[2] != KERN_PROC_PID || !oldp || !oldlenp) {
        errno = EINVAL;
        return -1;
    }
    char stat[1024], sys[1024];
    if (read_file("/proc/self/stat", stat, sizeof stat)) return -1;
    /* /proc/<pid>/stat: "<pid> (<comm>) <state> ..."; starttime is field 22
     * (1-based), i.e. the 20th token after the command name, in clock ticks
     * since boot. /proc/stat's btime gives the boot wall-clock time. */
    unsigned long long starttime = 0;
    char *comm_end = strrchr(stat, ')');
    if (comm_end) {
        int field = 2;
        for (char *tok = strtok(comm_end + 1, " "); tok; tok = strtok(NULL, " ")) {
            if (++field == 22) { starttime = strtoull(tok, NULL, 10); break; }
        }
    }
    unsigned long long btime = 0;
    if (!read_file("/proc/stat", sys, sizeof sys)) {
        char *b = strstr(sys, "btime ");
        if (b) btime = strtoull(b + 6, NULL, 10);
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    struct kinfo_proc *kp = (struct kinfo_proc *)oldp;
    memset(kp, 0, *oldlenp < sizeof *kp ? *oldlenp : sizeof *kp);
    kp->kp_proc.p_starttime.tv_sec = (time_t)(btime + starttime / (unsigned long long)hz);
    kp->kp_proc.p_starttime.tv_usec = (suseconds_t)((starttime % (unsigned long long)hz) * 1000000ull / (unsigned long long)hz);
    *oldlenp = sizeof *kp;
    return 0;
}

/* ---------- CommonCrypto SHA-256 ---------- */
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + BSIG1(e) + CH(e, f, g) + k256[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
int CC_SHA256_Init(CC_SHA256_CTX *ctx) {
    static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    if (!ctx) return 0;
    memcpy(ctx->h, init, sizeof init);
    ctx->bytes = 0; ctx->used = 0;
    return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *ctx, const void *data, unsigned int len) {
    if (!ctx) return 0;
    const uint8_t *p = data;
    ctx->bytes += len;
    while (len) {
        size_t take = 64 - ctx->used;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->used, p, take);
        ctx->used += (uint32_t)take; p += take; len -= (unsigned int)take;
        if (ctx->used == 64) { sha256_block(ctx->h, ctx->buf); ctx->used = 0; }
    }
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *ctx) {
    if (!ctx || !md) return 0;
    uint64_t bits = ctx->bytes * 8;
    uint8_t pad = 0x80;
    CC_SHA256_Update(ctx, &pad, 1);
    uint8_t zero = 0;
    while (ctx->used != 56) CC_SHA256_Update(ctx, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(ctx, len, 8);
    for (int i = 0; i < 8; i++) {
        md[i * 4] = (uint8_t)(ctx->h[i] >> 24); md[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        md[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8); md[i * 4 + 3] = (uint8_t)ctx->h[i];
    }
    return 1;
}
unsigned char *CC_SHA256(const void *data, unsigned int len, unsigned char *md) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    CC_SHA256_Update(&ctx, data, len);
    CC_SHA256_Final(md, &ctx);
    return md;
}
