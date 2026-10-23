/* Android front end for the nxnative host: window, lifecycle, input, logging,
 * guest asset installation and the JNI surface the Activity talks to.
 *
 * The guest runs on one host thread with a large stack (as on iOS). All the
 * platform code the guest reaches goes through the existing hooks
 * (Biko3Libnx.c / Biko3Vulkan.c / Biko3AudioAndroid.c); this file only owns the
 * Android side: ANativeWindow hand-over, pause/resume, touch and log files. */
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dm.h"
#include "front.h"
#include "hle.h"
#include "Biko3Vulkan.h"
#include "Biko3Libnx.h"
#include "Biko3Audio.h"
#include "dm_tsd.h"
#include "dm_input.h"

#define TAG "Biko3"
#define STATUS_MAX 8192

int dm_main(int argc, char **argv);
int biko3_selftest_run(const char *guest_dir, const char *data_dir, void (*report)(const char *line));
void biko3_audio_stats(uint64_t *callbacks, uint64_t *underruns, int *silent);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_thread_started;
static int g_paused;
static ANativeWindow *g_window;
static char g_guest_dir[1024];
static char g_data_dir[1024];
static char g_status[STATUS_MAX];
static char g_last_line[1024];
static char g_status_path[1200];

static void status_add(const char *line) {
    pthread_mutex_lock(&g_lock);
    size_t used = strlen(g_status);
    if (used + strlen(line) + 2 < STATUS_MAX) {
        memcpy(g_status + used, line, strlen(line));
        g_status[used + strlen(line)] = '\n';
        g_status[used + strlen(line) + 1] = 0;
    }
    snprintf(g_last_line, sizeof g_last_line, "%s", line);
    pthread_mutex_unlock(&g_lock);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", line);
    fprintf(stderr, "[biko3] %s\n", line);
    if (g_status_path[0]) {
        FILE *status = fopen(g_status_path, "a");
        if (status) { fprintf(status, "%s\n", line); fclose(status); }
    }
}

static void status_set(const char *line) {
    pthread_mutex_lock(&g_lock);
    g_status[0] = 0;
    pthread_mutex_unlock(&g_lock);
    status_add(line);
}

static void report_line(const char *line) { status_add(line); }

static void front_error(const char *message) { status_add(message ? message : "unknown game error"); }

/* ---------- front.h ---------- */
void *front_metal_layer(void) { return g_window; }
int front_screen_max_fps(void) { return 60; }
void front_run(void (*guest)(void)) { guest(); }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial,
                     int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    return -1;
}
void front_save_failed(int err) {
    char line[128];
    snprintf(line, sizeof line, "save write failed (errno %d); progress was not saved", err);
    status_add(line);
}

/* ---------- assets ---------- */
static int copy_asset(AAssetManager *manager, const char *name, const char *target) {
    char path[1200];
    snprintf(path, sizeof path, "guest/%s", name);
    AAsset *asset = AAssetManager_open(manager, path, AASSET_MODE_STREAMING);
    if (!asset) return -1;
    off_t length = AAsset_getLength(asset);
    /* Always rewrite: the asset content can change between APK builds while
     * the size stays the same (the guest image is patched, not resized). */
    FILE *file = fopen(target, "wb");
    if (!file) {
        AAsset_close(asset);
        return -1;
    }
    char buffer[65536];
    int read;
    long total = 0;
    while ((read = AAsset_read(asset, buffer, sizeof buffer)) > 0) {
        if (fwrite(buffer, 1, (size_t)read, file) != (size_t)read) break;
        total += read;
    }
    AAsset_close(asset);
    fclose(file);
    return total == length ? 0 : -1;
}

static int install_guest(AAssetManager *manager) {
    static const char *files[] = {
        "main.patched.bin", "main.fixups.tsv", "main.gates.tsv",
        "main.sites.tsv", "main.cuts.tsv", "main.obs.tsv",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char target[1200];
        snprintf(target, sizeof target, "%s/%s", g_guest_dir, files[i]);
        int result = copy_asset(manager, files[i], target);
        if (result) {
            char line[1300];
            snprintf(line, sizeof line, "guest asset %s: %s", files[i], result < 0 ? "missing" : "copy failed");
            status_add(line);
            if (!strcmp(files[i], "main.patched.bin") || !strcmp(files[i], "main.fixups.tsv") ||
                !strcmp(files[i], "main.gates.tsv"))
                return 0;
        }
    }
    status_add("guest image installed");
    return 1;
}

/* ---------- boot thread ---------- */
static void *boot_thread(void *argument) {
    AAssetManager *manager = (AAssetManager *)argument;
    char log_path[1200];
    snprintf(log_path, sizeof log_path, "%s/log.txt", g_data_dir);
    if (freopen(log_path, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(fileno(stderr), STDOUT_FILENO);
    status_set("Biko3 Android starting");

    if (g_window) {
        biko3_vk_set_metal_layer(g_window);
        status_add("Vulkan window handed to the bridge");
    } else {
        status_add("no present surface");
    }

    int guest_ready = manager ? install_guest(manager) : 0;
    char line[1300];
    snprintf(line, sizeof line, "self-test: guest %s", guest_ready ? "present" : "missing");
    status_add(line);
    biko3_selftest_run(g_guest_dir, g_data_dir, report_line);

    uint64_t callbacks = 0, underruns = 0;
    int silent = 0;
    biko3_audio_stats(&callbacks, &underruns, &silent);
    snprintf(line, sizeof line, "audio: callbacks %llu, underruns %llu, %s",
             (unsigned long long)callbacks, (unsigned long long)underruns, silent ? "silent sink" : "aaudio");
    status_add(line);

    if (!guest_ready) {
        status_add("guest not booted: image missing");
        return NULL;
    }
    {
        /* Development switch: a marker file in the app's external directory
         * keeps the app in self-test mode so the Android layer can be inspected
         * on a host whose ABI cannot execute the guest. */
        char marker[1200];
        snprintf(marker, sizeof marker, "%s/selftest-only", g_data_dir);
        if (access(marker, F_OK) == 0) {
            status_add("self-test only: guest boot skipped (selftest-only marker)");
            for (;;) sleep(60);
        }
    }
#if defined(__aarch64__)
    char *argv[] = { (char *)"dmhost", g_guest_dir, NULL };
    status_add("booting the original guest");
    int result = dm_main(2, argv);
    snprintf(line, sizeof line, "dm_main returned %d", result);
    status_add(line);
#else
    status_add("guest not booted: this build has no AArch64 guest execution");
#endif
    return NULL;
}

/* ---------- JNI ---------- */
JNIEXPORT jboolean JNICALL
Java_io_github_hyc1743_biko3_Biko3Native_start(JNIEnv *env, jclass clazz, jobject assets,
                                               jstring files_dir, jstring data_dir,
                                               jobject surface, jint width, jint height) {
    (void)clazz;
    if (g_thread_started) return JNI_TRUE;
    const char *files = (*env)->GetStringUTFChars(env, files_dir, NULL);
    const char *data = (*env)->GetStringUTFChars(env, data_dir, NULL);
    snprintf(g_guest_dir, sizeof g_guest_dir, "%s/guest", files);
    snprintf(g_data_dir, sizeof g_data_dir, "%s", data);
    snprintf(g_status_path, sizeof g_status_path, "%s/status.txt", files);
    { FILE *status = fopen(g_status_path, "w"); if (status) fclose(status); }
    AAssetManager *manager = assets ? AAssetManager_fromJava(env, assets) : NULL;
    if (surface) g_window = ANativeWindow_fromSurface(env, surface);
    if (g_window) {
        /* The game renders its portrait picture into the Switch's 1280x720
         * panel, rotated for a landscape display. Present it in portrait by
         * asking the compositor to rotate the buffer; the 9:16 view then scales
         * both axes by the same amount. */
        /* The game's panel is 1280x720; the 16:9 SurfaceView scales it evenly. */
        ANativeWindow_setBuffersGeometry(g_window, 1280, 720, WINDOW_FORMAT_RGBX_8888);
        /* A SurfaceView on a landscape activity gets the display rotation as a
         * buffer transform; the guest already renders upright in the activity's
         * coordinate space, so force the identity transform. */
        ANativeWindow_setBuffersTransform(g_window, ANATIVEWINDOW_TRANSFORM_IDENTITY);
    }

    char path[1200];
    snprintf(path, sizeof path, "%s/game", g_data_dir);
    setenv("BIKO3_GAME_ROOT", path, 1);
    snprintf(path, sizeof path, "%s/saves", g_data_dir);
    setenv("BIKO3_SAVE_ROOT", path, 1);
    snprintf(path, sizeof path, "%s/captures", g_data_dir);
    setenv("BIKO3_CAPTURE_ROOT", path, 1);
    setenv("DM_ROMFS", g_guest_dir, 1);
    setenv("DM_SAVE", path, 1);
    setenv("DM_TEMP", g_data_dir, 1);
    (*env)->ReleaseStringUTFChars(env, files_dir, files);
    (*env)->ReleaseStringUTFChars(env, data_dir, data);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    g_thread_started = pthread_create(&g_thread, &attr, boot_thread, manager) == 0;
    pthread_attr_destroy(&attr);
    return g_thread_started ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_io_github_hyc1743_biko3_Biko3Native_stop(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    dm_host_shutdown("Activity stop");
}

JNIEXPORT void JNICALL
Java_io_github_hyc1743_biko3_Biko3Native_pause(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    g_paused = 1;
    biko3_audio_suspend(1);
    biko3_vk_suspend(1);
    guest_clock_freeze_for(1, 1);
    dm_focus_set(2);
    dm_focus_set(3);
    status_add("paused");
}

JNIEXPORT void JNICALL
Java_io_github_hyc1743_biko3_Biko3Native_resume(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    g_paused = 0;
    dm_focus_set(2);
    guest_clock_freeze_for(1, 0);
    biko3_vk_suspend(0);
    biko3_audio_suspend(0);
    dm_focus_set(1);
    status_add("resumed");
}

JNIEXPORT void JNICALL
Java_io_github_hyc1743_biko3_Biko3Native_touch(JNIEnv *env, jclass clazz, jint action,
                                               jint id, jfloat x, jfloat y) {
    (void)env; (void)clazz;
    /* MotionEvent.ACTION_UP = 1, ACTION_CANCEL = 3, ACTION_POINTER_UP = 6. The
     * guest only sees a click when the touch is released, so lift the point on
     * those actions instead of reporting a permanent touch. */
    if (action == 1 || action == 3 || action == 6) {
        dm_touch_set(NULL, 0);
        return;
    }
    dm_touch point = { id, x, y };
    dm_touch_set(&point, 1);
}

JNIEXPORT void JNICALL
Java_io_github_hyc1743_biko3_Biko3Native_buttons(JNIEnv *env, jclass clazz, jint mask) {
    (void)env; (void)clazz;
    dm_input_set((uint64_t)(uint32_t)mask, 0, 0, 0, 0);
}

JNIEXPORT jstring JNICALL
Java_io_github_hyc1743_biko3_Biko3Native_status(JNIEnv *env, jclass clazz) {
    (void)clazz;
    pthread_mutex_lock(&g_lock);
    jstring result = (*env)->NewStringUTF(env, g_status);
    pthread_mutex_unlock(&g_lock);
    return result;
}
