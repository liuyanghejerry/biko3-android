/* No-op implementations for the retail-SDK and Apple-only host modules that the
 * Android build does not compile. Each one is unreachable for this guest (a
 * libnx homebrew image) but keeps the shared host sources linkable. */
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "dm.h"

/* Defined by hle_nvn.c upstream (the NVN layer is not built on Android). */
__thread _Atomic int dm_host_work;

/* Diag / tooling modules (probe, Unreal allocator, Oodle, counting stubs). */
void probe_install(void) {}
void dm_ue_malloc_install(void) {}
void dm_oodle_serve_maybe(int phase) { (void)phase; }
void dm_oodle_job_maybe(void) {}
void hlecount_flush(void) {}
void hlecount_seal(void) {}
void hlecount_svc(int num) { (void)num; }
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    (void)kind; (void)name; return fn;
}

/* hle_audio.c / hle_nvn.c are not part of the Android build: the Biko3 audio
 * transport in Biko3AudioAndroid.c and the Vulkan bridge own these lifecycles. */
void dm_audio_suspend(int on) { (void)on; }
void dm_audio_reset(void) {}
uint64_t dm_audio_render_calls(void) { return 0; }
void dm_gfx_suspend(int on) { (void)on; }
void dm_gfx_enter(void) {}
void dm_gfx_leave(void) {}
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }
void dm_shot_written(const char *path, int ok) { (void)path; (void)ok; }
uint64_t dm_nvn_frame_count(void) { return 0; }
uint64_t dm_nvn_sub_cpu_ns(void) { return 0; }

/* hle_lifecycle.c uses GCD; this replacement keeps the same contract. */
void dm_host_shutdown(const char *why) {
    LOG("received %s: graceful exit", why ? why : "shutdown");
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    _exit(0);
}
static void on_term(int sig) { (void)sig; dm_host_shutdown("SIGTERM"); }
void dm_term_install(void) {
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
}
