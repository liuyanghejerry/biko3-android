/* Android audio transport for the original Biko3 mixer.
 *
 * The guest keeps its own mixer, decoder, libnx audio thread and four-buffer
 * accounting; only audout is hosted. Each guest block is 480 frames of 48 kHz
 * stereo signed 16-bit PCM (1920 bytes). The guest keeps about 20 ms in flight,
 * while Android's end-to-end latency is larger, so the host adds a block queue
 * and reports a guest buffer as released only after the AAudio callback has
 * consumed its last frame. If AAudio cannot open (no audio device), the
 * transport falls back to a silent sink that releases blocks immediately. */
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *next, *data;
    uint64_t capacity, length, offset;
} GuestAudioBuffer;
_Static_assert(sizeof(GuestAudioBuffer) == 40, "libnx AudioOutBuffer ABI");

#define BLOCK_BYTES 1920
#define QUEUE_BLOCKS 128
#define RELEASED_BLOCKS 32
#define ERROR_AUDIO 0x1f59u
#define BUILTIN_SPEAKER 2

static struct {
    pthread_mutex_t lock;
    pthread_mutex_t control;
    AAudioStream *stream;
    struct { GuestAudioBuffer *buffer; uint32_t offset; } queue[QUEUE_BLOCKS];
    unsigned head, count;
    GuestAudioBuffer *released[RELEASED_BLOCKS];
    unsigned released_head, released_count;
    uint64_t underruns, callbacks;
    int requested_start, started, suspended, silent;
    uint32_t error;
} audio = {.lock = PTHREAD_MUTEX_INITIALIZER, .control = PTHREAD_MUTEX_INITIALIZER};

static void release_locked(GuestAudioBuffer *buffer) {
    if (audio.released_count < RELEASED_BLOCKS) {
        audio.released[(audio.released_head + audio.released_count) % RELEASED_BLOCKS] = buffer;
        audio.released_count++;
    } else {
        audio.error = ERROR_AUDIO;
    }
}

static aaudio_data_callback_result_t on_audio(AAudioStream *stream, void *user,
                                             void *data, int32_t frames) {
    (void)user; (void)stream;
    int16_t *out = data;
    size_t want = (size_t)frames * 4, done = 0;
    pthread_mutex_lock(&audio.lock);
    audio.callbacks++;
    if (!audio.suspended) {
        while (done < want && audio.count) {
            GuestAudioBuffer *buffer = audio.queue[audio.head].buffer;
            uint32_t offset = audio.queue[audio.head].offset;
            size_t left = (size_t)buffer->length - offset;
            size_t take = want - done < left ? want - done : left;
            memcpy((uint8_t *)out + done, (uint8_t *)buffer->data + buffer->offset + offset, take);
            done += take;
            offset += (uint32_t)take;
            if (offset >= buffer->length) {
                release_locked(buffer);
                audio.head = (audio.head + 1) % QUEUE_BLOCKS;
                audio.count--;
            } else {
                audio.queue[audio.head].offset = offset;
            }
        }
    }
    if (done < want) {
        memset((uint8_t *)out + done, 0, want - done);
        if (!audio.suspended) audio.underruns++;
    }
    pthread_mutex_unlock(&audio.lock);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static uint32_t open_stream(void) {
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) return ERROR_AUDIO;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDeviceId(builder, BUILTIN_SPEAKER);
    AAudioStreamBuilder_setDataCallback(builder, on_audio, NULL);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, 240);
    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &audio.stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || !audio.stream) {
        fprintf(stderr, "[biko3] AAudio open failed %d (%s); using a silent sink\n",
                (int)result, AAudio_convertResultToText(result));
        audio.stream = NULL;
        return ERROR_AUDIO;
    }
    int32_t capacity = AAudioStream_getBufferCapacityInFrames(audio.stream);
    if (capacity > 4096) capacity = 4096;
    AAudioStream_setBufferSizeInFrames(audio.stream, capacity > 960 ? 960 : capacity);
    fprintf(stderr, "[biko3] native audio AAudio 48000 Hz stereo signed16, capacity %d frames\n", (int)capacity);
    return 0;
}

static uint32_t audio_initialize_impl(void) {
    if (audio.stream || audio.silent) return 0;
    audio.head = audio.count = audio.released_head = audio.released_count = 0;
    audio.underruns = audio.callbacks = 0;
    audio.requested_start = audio.started = 0;
    audio.error = 0;
    if (open_stream() == ERROR_AUDIO) {
        audio.silent = 1;
        fprintf(stderr, "[biko3] audio transport: silent sink\n");
    }
    return 0;
}

static uint32_t audio_start_impl(void) {
    audio.requested_start = 1;
    if (audio.stream && !audio.started) {
        if (AAudioStream_requestStart(audio.stream) != AAUDIO_OK) return ERROR_AUDIO;
        audio.started = 1;
        fprintf(stderr, "[biko3] audio stream started\n");
    }
    return 0;
}

static uint32_t audio_stop_impl(void) {
    audio.requested_start = 0;
    if (audio.stream && audio.started) {
        AAudioStream_requestStop(audio.stream);
        AAudioStream_requestFlush(audio.stream);
        audio.started = 0;
    }
    return 0;
}

static void audio_exit_impl(void) {
    if (audio.stream) {
        AAudioStream_requestStop(audio.stream);
        AAudioStream_close(audio.stream);
        audio.stream = NULL;
    }
    audio.silent = 0;
    audio.head = audio.count = audio.released_head = audio.released_count = 0;
}

static uint32_t audio_rate(void) { return 48000; }
static uint32_t audio_channels(void) { return 2; }
static uint32_t audio_format(void) { return 2; } /* PcmInt16 */

void biko3_audio_suspend(int suspended) {
    pthread_mutex_lock(&audio.control);
    pthread_mutex_lock(&audio.lock);
    audio.suspended = !!suspended;
    pthread_mutex_unlock(&audio.lock);
    pthread_mutex_unlock(&audio.control);
}

static uint32_t audio_append_impl(GuestAudioBuffer *guest) {
    if (!guest || !guest->data || guest->length != BLOCK_BYTES ||
        guest->offset > guest->capacity || guest->length > guest->capacity - guest->offset)
        return ERROR_AUDIO;
    if (audio.silent) {   /* no consumer: keep the guest's four-buffer accounting moving */
        pthread_mutex_lock(&audio.lock);
        release_locked(guest);
        pthread_mutex_unlock(&audio.lock);
        return 0;
    }
    if (!audio.stream) return ERROR_AUDIO;
    pthread_mutex_lock(&audio.lock);
    if (audio.count >= QUEUE_BLOCKS) {
        pthread_mutex_unlock(&audio.lock);
        return ERROR_AUDIO;
    }
    audio.queue[(audio.head + audio.count) % QUEUE_BLOCKS].buffer = guest;
    audio.queue[(audio.head + audio.count) % QUEUE_BLOCKS].offset = 0;
    audio.count++;
    pthread_mutex_unlock(&audio.lock);
    if (audio.requested_start && !audio.started) return audio_start_impl();
    return 0;
}

static uint32_t audio_released_impl(GuestAudioBuffer **out, uint32_t *count) {
    if (!out || !count) return ERROR_AUDIO;
    pthread_mutex_lock(&audio.lock);
    *out = NULL; *count = 0;
    if (audio.released_count) {
        *out = audio.released[audio.released_head];
        audio.released_head = (audio.released_head + 1) % RELEASED_BLOCKS;
        audio.released_count--;
        *count = 1;
    }
    uint32_t result = audio.error ? ERROR_AUDIO : 0;
    pthread_mutex_unlock(&audio.lock);
    return result;
}

static uint32_t audio_initialize(void) {
    pthread_mutex_lock(&audio.control);
    uint32_t result = audio_initialize_impl();
    pthread_mutex_unlock(&audio.control);
    return result;
}
static uint32_t audio_start(void) {
    pthread_mutex_lock(&audio.control);
    uint32_t result = audio_start_impl();
    pthread_mutex_unlock(&audio.control);
    return result;
}
static uint32_t audio_stop(void) {
    pthread_mutex_lock(&audio.control);
    uint32_t result = audio_stop_impl();
    pthread_mutex_unlock(&audio.control);
    return result;
}
static void audio_exit(void) {
    pthread_mutex_lock(&audio.control);
    audio_exit_impl();
    pthread_mutex_unlock(&audio.control);
}
static uint32_t audio_append(GuestAudioBuffer *guest) {
    pthread_mutex_lock(&audio.control);
    uint32_t result = audio_append_impl(guest);
    pthread_mutex_unlock(&audio.control);
    return result;
}
static uint32_t audio_released(GuestAudioBuffer **out, uint32_t *count) {
    return audio_released_impl(out, count);
}

void biko3_audio_stats(uint64_t *callbacks, uint64_t *underruns, int *silent) {
    pthread_mutex_lock(&audio.lock);
    if (callbacks) *callbacks = audio.callbacks;
    if (underruns) *underruns = audio.underruns;
    if (silent) *silent = audio.silent;
    pthread_mutex_unlock(&audio.lock);
}

void *biko3_audio_resolve(const char *name) {
#define AUDIO(n, f) if (!strcmp(name, n)) return (void *)f
    AUDIO("biko3_audio_initialize", audio_initialize);
    AUDIO("biko3_audio_exit", audio_exit);
    AUDIO("biko3_audio_rate", audio_rate);
    AUDIO("biko3_audio_channels", audio_channels);
    AUDIO("biko3_audio_format", audio_format);
    AUDIO("biko3_audio_start", audio_start);
    AUDIO("biko3_audio_stop", audio_stop);
    AUDIO("biko3_audio_append", audio_append);
    AUDIO("biko3_audio_released", audio_released);
#undef AUDIO
    return NULL;
}
