#include "rtcma_internal.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#include "miniaudio.h"

#include "rtcma_device_id.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull-mode audio. Each Player and Capturer owns its own ma_device
 * (playback-only and capture-only respectively). miniaudio's data
 * callback fires on a per-device thread and is the clock for that
 * direction - no separate decode or encode threads, no timers.
 *
 *   playback: pull a 20 ms frame from the bound recv track, copy to
 *             output, hold spill in pb_leftover. The audio device is
 *             the playout clock, which is the only thing that
 *             prevents audible cracking.
 *
 *   capture:  accumulate input samples until a full 20 ms frame is
 *             ready, then push_pcm to the bound send track.
 *
 * Binding model: each handle has at most one bound track, guarded by
 * a per-handle mutex held briefly around the audio callback. Attach
 * and detach acquire the mutex to bind / unbind; once detach returns,
 * the data callback is guaranteed not to touch the track's memory
 * again. The mutex is uncontended in steady state. */

/* pb_leftover holds spill from a decoded opus frame that overflowed the
 * audio-device callback's request. Sized for the worst case: peer sends
 * a 120 ms opus frame (RTCMA_DECODE_MAX_SAMPLES) and the device asks for
 * close to zero this tick - we hold the rest until the next tick(s).
 * RTCMA_DECODE_MAX_SAMPLES x 2 channels = ~23 kB in the Player struct. */
#define CAPTURE_ACCUM_FRAMES   2  /* up to 2 frames of capture accumulation */

/* -- Channel mixing ---------------------------------------------------
 *
 * The audio device's channel count (configured at _new from cfg.channels
 * or a default) is independent of the codec's channel count (resolved
 * from SDP at attach). When they differ we need to bridge.
 *
 * We only ever bridge mono<->stereo - Opus over RTP only uses 1 or 2
 * channels in practice, and we cap configured channels to {1,2} on the
 * device side too. So:
 *   mono -> stereo: duplicate each sample (L -> L, L)
 *   stereo -> mono: average ((L + R) / 2 with rounding-down via >>1)
 *
 * No-op when src_ch == dst_ch - caller copies directly. Both buffers
 * hold interleaved int16; dst must have room for samples_per_channel
 * x dst_ch. */
static void mix_channels(const int16_t *src, int src_ch,
                         int16_t *dst, int dst_ch,
                         int samples_per_channel)
{
    if (src_ch == dst_ch) {
        memcpy(dst, src, (size_t)samples_per_channel * src_ch * sizeof(int16_t));
        return;
    }
    if (src_ch == 1 && dst_ch == 2) {
        for (int i = 0; i < samples_per_channel; i++) {
            dst[2*i + 0] = src[i];
            dst[2*i + 1] = src[i];
        }
        return;
    }
    if (src_ch == 2 && dst_ch == 1) {
        for (int i = 0; i < samples_per_channel; i++) {
            int32_t s = (int32_t)src[2*i + 0] + (int32_t)src[2*i + 1];
            dst[i] = (int16_t)(s >> 1);
        }
        return;
    }
    /* unreachable for the values rtc-ma allows */
}

struct RtcmaPlayer {
    int               channels;             /* audio device output channels */
    int               payload_type_override;

    pthread_mutex_t   bind_lock;
    RtcmaRecvTrack    recv;
    bool              attached;

    /* Playback leftover buffer (audio thread only - no sync needed).
     * Sized for one max-duration opus frame (120 ms x 2ch). */
    int16_t           pb_leftover[RTCMA_DECODE_MAX_SAMPLES * 2];
    int               pb_leftover_samples;

    /* Heap-allocated so reopen() can test-then-swap: init the new
     * device first, only tear down the old one on success. */
    ma_device        *device;
    int               started;

    /* Mirror of ma_device.masterVolumeFactor so reopen() can re-apply
     * the user's gain to a freshly-init'd ma_device (which defaults to
     * 1.0). Public API is single-threaded per handle, so plain float. */
    float             volume;

    /* stats */
    _Atomic uint64_t  pb_callbacks;
    _Atomic uint64_t  pb_underrun_samples;
    _Atomic uint64_t  pb_underrun_callbacks;
    _Atomic uint64_t  pb_max_underrun;
};

struct RtcmaCapturer {
    int               channels;       /* audio device capture channels */
    int               payload_type;

    pthread_mutex_t   bind_lock;
    RtcmaSendTrack    send;
    bool              attached;

    /* Capture accumulator (audio thread only). */
    int16_t           cap_accum[RTCMA_FRAME_SAMPLES * 2 * CAPTURE_ACCUM_FRAMES];
    int               cap_accum_samples;

    ma_device        *device;
    int               started;

    /* See RtcmaPlayer.volume. */
    float             volume;

    _Atomic uint64_t  cap_frames_pushed;
    _Atomic uint64_t  cap_drops;
};

/* -- Playback callback ----------------------------------------------- */

static void on_playback(ma_device *dev, void *output, const void *input,
                        ma_uint32 frame_count)
{
    (void)input;
    RtcmaPlayer *p = (RtcmaPlayer *)dev->pUserData;
    int16_t     *out = (int16_t *)output;

    pthread_mutex_lock(&p->bind_lock);

    atomic_fetch_add(&p->pb_callbacks, 1);

    size_t want_samples = (size_t)frame_count * (size_t)p->channels;
    size_t filled       = 0;

    /* 1. Drain leftover from previous callback. */
    if (p->pb_leftover_samples > 0) {
        size_t take = (size_t)p->pb_leftover_samples;
        if (take > want_samples) take = want_samples;
        memcpy(out + filled, p->pb_leftover, take * sizeof(int16_t));
        if ((size_t)p->pb_leftover_samples > take) {
            memmove(p->pb_leftover, p->pb_leftover + take,
                    ((size_t)p->pb_leftover_samples - take) * sizeof(int16_t));
        }
        p->pb_leftover_samples -= (int)take;
        filled                 += take;
    }

    /* 2. Pull whole 20 ms frames until enough. The decoder produces
     * recv.channels-interleaved samples; mix to p->channels to match
     * the device, since the negotiated codec channels (per SDP
     * sprop-stereo) are not necessarily the same as the audio
     * hardware output channels. */
    /* Decode and device-mix buffers must hold a worst-case 120 ms x 2ch
     * opus frame (RTCMA_DECODE_MAX_SAMPLES x 2 = 11520 int16 = ~23 kB).
     * Real-world peers send 20 ms most of the time; this just covers the
     * libwebrtc 40/60 ms case without the decoder returning -2. */
    while (filled < want_samples && p->attached) {
        int16_t opus_pcm[RTCMA_DECODE_MAX_SAMPLES * 2];
        int decoded = rtcma_recv_track_pull_pcm(&p->recv, opus_pcm,
                                                sizeof(opus_pcm) / sizeof(opus_pcm[0]));
        if (decoded <= 0) {
            size_t under = want_samples - filled;
            atomic_fetch_add(&p->pb_underrun_samples, under);
            atomic_fetch_add(&p->pb_underrun_callbacks, 1);
            uint64_t prev_max = atomic_load(&p->pb_max_underrun);
            while (under > prev_max &&
                   !atomic_compare_exchange_weak(&p->pb_max_underrun,
                                                 &prev_max, under)) {}
            memset(out + filled, 0, under * sizeof(int16_t));
            filled = want_samples;
            break;
        }

        int16_t dev_pcm[RTCMA_DECODE_MAX_SAMPLES * 2];
        mix_channels(opus_pcm, p->recv.channels, dev_pcm, p->channels,
                     decoded);

        size_t produced = (size_t)decoded * (size_t)p->channels;
        size_t needed   = want_samples - filled;
        if (produced <= needed) {
            memcpy(out + filled, dev_pcm, produced * sizeof(int16_t));
            filled += produced;
        } else {
            memcpy(out + filled, dev_pcm, needed * sizeof(int16_t));
            size_t spill = produced - needed;
            memcpy(p->pb_leftover, dev_pcm + needed, spill * sizeof(int16_t));
            p->pb_leftover_samples = (int)spill;
            filled = want_samples;
        }
    }

    if (filled < want_samples) {
        memset(out + filled, 0, (want_samples - filled) * sizeof(int16_t));
    }

    pthread_mutex_unlock(&p->bind_lock);
}

/* -- Capture callback ------------------------------------------------ */

static void on_capture(ma_device *dev, void *output, const void *input,
                       ma_uint32 frame_count)
{
    (void)output;
    RtcmaCapturer  *c  = (RtcmaCapturer *)dev->pUserData;
    const int16_t  *in = (const int16_t *)input;

    pthread_mutex_lock(&c->bind_lock);

    size_t in_samples = (size_t)frame_count * (size_t)c->channels;

    if (!c->attached) {
        atomic_fetch_add(&c->cap_drops, in_samples);
        c->cap_accum_samples = 0;
        pthread_mutex_unlock(&c->bind_lock);
        return;
    }

    while (in_samples > 0) {
        size_t cap_max = (size_t)RTCMA_FRAME_SAMPLES * (size_t)c->channels
                         * CAPTURE_ACCUM_FRAMES;
        size_t space   = cap_max - (size_t)c->cap_accum_samples;
        if (space == 0) {
            c->cap_accum_samples = 0;
            space = cap_max;
        }
        size_t take = (in_samples < space) ? in_samples : space;
        memcpy(c->cap_accum + c->cap_accum_samples, in,
               take * sizeof(int16_t));
        c->cap_accum_samples += (int)take;
        in                   += take;
        in_samples           -= take;

        size_t one_frame = (size_t)RTCMA_FRAME_SAMPLES * (size_t)c->channels;
        while ((size_t)c->cap_accum_samples >= one_frame) {
            /* Mix device-channel PCM to encoder-channel PCM before
             * push. Negotiated encoder channels (sprop-stereo) may
             * differ from the mic's hardware channel count. */
            int16_t enc_pcm[RTCMA_FRAME_SAMPLES * 2];
            mix_channels(c->cap_accum, c->channels,
                         enc_pcm, c->send.channels,
                         RTCMA_FRAME_SAMPLES);
            rtcma_send_track_push_pcm(&c->send, enc_pcm,
                                      RTCMA_FRAME_SAMPLES);
            atomic_fetch_add(&c->cap_frames_pushed, 1);
            if ((size_t)c->cap_accum_samples > one_frame) {
                memmove(c->cap_accum, c->cap_accum + one_frame,
                        ((size_t)c->cap_accum_samples - one_frame)
                            * sizeof(int16_t));
            }
            c->cap_accum_samples -= (int)one_frame;
        }
    }

    pthread_mutex_unlock(&c->bind_lock);
}

/* -- ma_device init helpers ------------------------------------------
 *
 * `device_id` is "<backend>:<payload>" or NULL/empty for the system
 * default. We parse to a local ma_device_id and hand miniaudio a
 * pointer to it; ma_device_init copies the data internally during
 * init, so the local goes out of scope safely on return. */

static ma_device *playback_device_init(RtcmaPlayer *p, const char *device_id)
{
    ma_device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;

    ma_device_id  id_buf;
    ma_device_id *id_ptr = NULL;
    if (device_id && *device_id) {
        ma_backend backend;
        if (rtcma_devid_resolve_active_backend(&backend) != 0 ||
            rtcma_devid_from_string(device_id, backend, &id_buf) != 0) {
            free(dev);
            return NULL;
        }
        id_ptr = &id_buf;
    }

    ma_device_config mcfg = ma_device_config_init(ma_device_type_playback);
    mcfg.playback.format    = ma_format_s16;
    mcfg.playback.channels  = (ma_uint32)p->channels;
    mcfg.playback.pDeviceID = id_ptr;
    mcfg.sampleRate         = RTCMA_SAMPLE_RATE;
    mcfg.dataCallback       = on_playback;
    mcfg.pUserData          = p;

    if (ma_device_init(NULL, &mcfg, dev) != MA_SUCCESS) {
        free(dev);
        return NULL;
    }
    return dev;
}

static ma_device *capture_device_init(RtcmaCapturer *c, const char *device_id)
{
    ma_device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;

    ma_device_id  id_buf;
    ma_device_id *id_ptr = NULL;
    if (device_id && *device_id) {
        ma_backend backend;
        if (rtcma_devid_resolve_active_backend(&backend) != 0 ||
            rtcma_devid_from_string(device_id, backend, &id_buf) != 0) {
            free(dev);
            return NULL;
        }
        id_ptr = &id_buf;
    }

    ma_device_config mcfg = ma_device_config_init(ma_device_type_capture);
    mcfg.capture.format    = ma_format_s16;
    mcfg.capture.channels  = (ma_uint32)c->channels;
    mcfg.capture.pDeviceID = id_ptr;
    mcfg.sampleRate        = RTCMA_SAMPLE_RATE;
    mcfg.dataCallback      = on_capture;
    mcfg.pUserData         = c;
    /* Voice-comm capture path: platform AEC/AGC/NS and a call-normalized
     * level. Without it the mic is noticeably quiet to the peer. */
    mcfg.aaudio.inputPreset = ma_aaudio_input_preset_voice_communication;

    if (ma_device_init(NULL, &mcfg, dev) != MA_SUCCESS) {
        free(dev);
        return NULL;
    }
    return dev;
}

/* -- Device enumeration ----------------------------------------------
 *
 * miniaudio returns ma_device_info* arrays whose memory it owns; the
 * IDs become invalid once the context is torn down. Serialise each
 * ma_device_id to a heap-owned ASCII string so callers can keep the
 * list around (and persist the strings) without holding a context
 * open. */
static int copy_devices(ma_backend backend,
                        const ma_device_info *src, ma_uint32 n,
                        RtcmaAudioDevice **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;
    if (n == 0) return 0;

    RtcmaAudioDevice *arr = calloc((size_t)n, sizeof(*arr));
    if (!arr) return -1;

    for (ma_uint32 i = 0; i < n; ++i) {
        char *id_str = rtcma_devid_to_string(backend, &src[i].id);
        if (!id_str) {
            for (ma_uint32 j = 0; j < i; ++j) free((void *)arr[j].id);
            free(arr);
            return -1;
        }
        arr[i].id         = id_str;
        arr[i].is_default = src[i].isDefault ? true : false;
        size_t name_max = sizeof(arr[i].name) - 1;
        strncpy(arr[i].name, src[i].name, name_max);
        arr[i].name[name_max] = '\0';
    }
    *out = arr;
    *out_count = (int)n;
    return 0;
}

int rtcma_enumerate_devices(RtcmaDeviceList *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) {
        rtcma_log(RTCMA_LOG_ERROR, "enumerate ma_context_init failed");
        return -1;
    }

    /* Memoize the backend miniaudio just resolved - rtcma_devid_from_string
     * in later Player/Capturer init calls needs to know the active backend
     * and the answer is fixed for the process lifetime. */
    rtcma_devid_set_active_backend(ctx.backend);

    ma_device_info *pb_infos  = NULL;
    ma_device_info *cap_infos = NULL;
    ma_uint32       pb_count  = 0;
    ma_uint32       cap_count = 0;
    if (ma_context_get_devices(&ctx, &pb_infos, &pb_count,
                               &cap_infos, &cap_count) != MA_SUCCESS) {
        rtcma_log(RTCMA_LOG_ERROR, "ma_context_get_devices failed");
        ma_context_uninit(&ctx);
        return -1;
    }

    int rc = copy_devices(ctx.backend, pb_infos, pb_count, &out->playback,
                          &out->playback_count);
    if (rc == 0) {
        rc = copy_devices(ctx.backend, cap_infos, cap_count, &out->capture,
                          &out->capture_count);
    }

    ma_context_uninit(&ctx);

    if (rc != 0) {
        rtcma_device_list_free(out);
        return -1;
    }
    return 0;
}

void rtcma_device_list_free(RtcmaDeviceList *list)
{
    if (!list) return;
    for (int i = 0; i < list->playback_count; ++i) {
        free((void *)list->playback[i].id);
    }
    free(list->playback);
    for (int i = 0; i < list->capture_count; ++i) {
        free((void *)list->capture[i].id);
    }
    free(list->capture);
    memset(list, 0, sizeof(*list));
}

/* -- Player ------------------------------------------------------------ */

RtcmaPlayer *rtcma_player_new(const RtcmaPlayerConfig *cfg)
{
    if (!cfg) return NULL;
    int channels = cfg->channels ? cfg->channels : 2;
    if (channels < 1 || channels > 2) return NULL;

    RtcmaPlayer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->channels              = channels;
    p->payload_type_override = cfg->payload_type;
    p->recv.rtc_track_id     = -1;
    p->volume                = 1.0f;
    pthread_mutex_init(&p->bind_lock, NULL);

    p->device = playback_device_init(p, cfg->device_id);
    if (!p->device) {
        rtcma_log(RTCMA_LOG_ERROR, "player ma_device_init failed");
        pthread_mutex_destroy(&p->bind_lock);
        free(p);
        return NULL;
    }

    rtcma_log(RTCMA_LOG_INFO,
              "player backend=%s rate=%u channels=%d%s",
              ma_get_backend_name(p->device->pContext->backend),
              (unsigned)p->device->sampleRate,
              channels,
              (cfg->device_id && *cfg->device_id) ? " pinned" : "");

    return p;
}

void rtcma_player_destroy(RtcmaPlayer *p)
{
    if (!p) return;
    rtcma_player_detach(p);
    if (p->device) {
        if (p->started) ma_device_stop(p->device);
        ma_device_uninit(p->device);
        free(p->device);
    }
    pthread_mutex_destroy(&p->bind_lock);
    rtcma_log(RTCMA_LOG_INFO,
              "player stats: pb_cb=%llu pb_underrun_samples=%llu "
              "pb_underrun_cb=%llu pb_max_underrun=%llu",
              (unsigned long long)atomic_load(&p->pb_callbacks),
              (unsigned long long)atomic_load(&p->pb_underrun_samples),
              (unsigned long long)atomic_load(&p->pb_underrun_callbacks),
              (unsigned long long)atomic_load(&p->pb_max_underrun));
    free(p);
}

int rtcma_player_start(RtcmaPlayer *p)
{
    if (!p || !p->device) return -1;
    if (p->started) return 0;
    if (ma_device_start(p->device) != MA_SUCCESS) {
        rtcma_log(RTCMA_LOG_ERROR, "player ma_device_start failed");
        return -1;
    }
    p->started = 1;
    return 0;
}

int rtcma_player_reopen(RtcmaPlayer *p, const char *device_id)
{
    if (!p || !p->device) return -1;

    ma_device *new_dev = playback_device_init(p, device_id);
    if (!new_dev) {
        rtcma_log(RTCMA_LOG_WARNING,
                  "player reopen ma_device_init failed (player unchanged)");
        return -1;
    }

    bool was_started = p->started;

    if (p->started) {
        ma_device_stop(p->device);
        p->started = 0;
    }
    ma_device_uninit(p->device);
    free(p->device);

    /* Audio-thread-only state - safe to reset, no callback is running
     * and the new device hasn't started yet. */
    p->pb_leftover_samples = 0;

    p->device = new_dev;

    /* Fresh ma_device defaults to volume=1.0; carry the user's setting
     * forward so a muted reopen doesn't silently unmute. */
    ma_device_set_master_volume(p->device, p->volume);

    rtcma_log(RTCMA_LOG_INFO, "player reopened%s",
              (device_id && *device_id) ? " pinned" : " default");

    if (was_started) {
        if (ma_device_start(p->device) != MA_SUCCESS) {
            rtcma_log(RTCMA_LOG_WARNING,
                      "player reopen ma_device_start failed "
                      "(installed but stopped)");
            return -1;
        }
        p->started = 1;
    }
    return 0;
}

int rtcma_player_attach(RtcmaPlayer *p, int rtc_track)
{
    if (!p) return -1;

    pthread_mutex_lock(&p->bind_lock);
    if (p->attached) {
        pthread_mutex_unlock(&p->bind_lock);
        rtcma_log(RTCMA_LOG_WARNING, "player already attached");
        return -1;
    }
    pthread_mutex_unlock(&p->bind_lock);

    if (rtcma_recv_track_attach(&p->recv, rtc_track,
                                /*channels_override*/ 0,
                                p->payload_type_override) < 0) {
        return -1;
    }

    pthread_mutex_lock(&p->bind_lock);
    p->attached = true;
    pthread_mutex_unlock(&p->bind_lock);
    return 0;
}

int rtcma_player_detach(RtcmaPlayer *p)
{
    if (!p) return -1;

    /* Flip the flag under the lock; once we release it, no future
     * audio callback can drain &p->recv, and any callback already in
     * flight has returned. Safe to tear down the recv track. */
    pthread_mutex_lock(&p->bind_lock);
    bool was = p->attached;
    p->attached = false;
    pthread_mutex_unlock(&p->bind_lock);

    if (was) rtcma_recv_track_detach(&p->recv);
    return 0;
}

int rtcma_player_set_volume(RtcmaPlayer *p, float volume)
{
    if (!p || !p->device) return -1;
    /* `volume >= 0 && volume <= 1` is false for NaN - intentional. */
    if (!(volume >= 0.0f && volume <= 1.0f)) return -1;
    p->volume = volume;
    return ma_device_set_master_volume(p->device, volume) == MA_SUCCESS
        ? 0 : -1;
}

/* -- Capturer ---------------------------------------------------------- */

RtcmaCapturer *rtcma_capturer_new(const RtcmaCapturerConfig *cfg)
{
    if (!cfg) return NULL;
    /* channels=0 -> mono mic by default (matches most laptop / headset
     * hardware and gajim's mono-opus offer). The encoder's channel
     * count is decided independently from SDP at attach time and the
     * capture callback mixes to bridge any mismatch. */
    int channels = cfg->channels ? cfg->channels : 1;
    if (channels < 1 || channels > 2) return NULL;

    RtcmaCapturer *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->channels          = channels;
    c->payload_type      = cfg->payload_type;
    c->send.rtc_track_id = -1;
    c->volume            = 1.0f;
    pthread_mutex_init(&c->bind_lock, NULL);

    c->device = capture_device_init(c, cfg->device_id);
    if (!c->device) {
        rtcma_log(RTCMA_LOG_ERROR, "capturer ma_device_init failed");
        pthread_mutex_destroy(&c->bind_lock);
        free(c);
        return NULL;
    }

    rtcma_log(RTCMA_LOG_INFO,
              "capturer backend=%s rate=%u channels=%d%s",
              ma_get_backend_name(c->device->pContext->backend),
              (unsigned)c->device->sampleRate,
              channels,
              (cfg->device_id && *cfg->device_id) ? " pinned" : "");

    return c;
}

void rtcma_capturer_destroy(RtcmaCapturer *c)
{
    if (!c) return;
    rtcma_capturer_detach(c);
    if (c->device) {
        if (c->started) ma_device_stop(c->device);
        ma_device_uninit(c->device);
        free(c->device);
    }
    pthread_mutex_destroy(&c->bind_lock);
    rtcma_log(RTCMA_LOG_INFO,
              "capturer stats: cap_frames=%llu cap_drops=%llu "
              "send_ok=%llu send_fail=%llu last_rc=%d",
              (unsigned long long)atomic_load(&c->cap_frames_pushed),
              (unsigned long long)atomic_load(&c->cap_drops),
              (unsigned long long)atomic_load(&c->send.diag_send_ok),
              (unsigned long long)atomic_load(&c->send.diag_send_fail),
              atomic_load(&c->send.diag_last_rc));
    free(c);
}

int rtcma_capturer_start(RtcmaCapturer *c)
{
    if (!c || !c->device) return -1;
    if (c->started) return 0;
    if (ma_device_start(c->device) != MA_SUCCESS) {
        rtcma_log(RTCMA_LOG_ERROR, "capturer ma_device_start failed");
        return -1;
    }
    c->started = 1;
    return 0;
}

int rtcma_capturer_reopen(RtcmaCapturer *c, const char *device_id)
{
    if (!c || !c->device) return -1;

    ma_device *new_dev = capture_device_init(c, device_id);
    if (!new_dev) {
        rtcma_log(RTCMA_LOG_WARNING,
                  "capturer reopen ma_device_init failed "
                  "(capturer unchanged)");
        return -1;
    }

    bool was_started = c->started;

    if (c->started) {
        ma_device_stop(c->device);
        c->started = 0;
    }
    ma_device_uninit(c->device);
    free(c->device);

    c->cap_accum_samples = 0;

    c->device = new_dev;

    /* See rtcma_player_reopen. */
    ma_device_set_master_volume(c->device, c->volume);

    rtcma_log(RTCMA_LOG_INFO, "capturer reopened%s",
              (device_id && *device_id) ? " pinned" : " default");

    if (was_started) {
        if (ma_device_start(c->device) != MA_SUCCESS) {
            rtcma_log(RTCMA_LOG_WARNING,
                      "capturer reopen ma_device_start failed "
                      "(installed but stopped)");
            return -1;
        }
        c->started = 1;
    }
    return 0;
}

int rtcma_capturer_attach(RtcmaCapturer *c, int rtc_track)
{
    if (!c) return -1;

    pthread_mutex_lock(&c->bind_lock);
    if (c->attached) {
        pthread_mutex_unlock(&c->bind_lock);
        rtcma_log(RTCMA_LOG_WARNING, "capturer already attached");
        return -1;
    }
    pthread_mutex_unlock(&c->bind_lock);

    /* Pass 0 for channels -> encoder picks SDP sprop-stereo. The
     * audio device channel count (c->channels) is independent: the
     * capture callback bridges via mix_channels. */
    if (rtcma_send_track_attach(&c->send, rtc_track,
                                /*channels_override*/ 0,
                                c->payload_type) < 0) {
        return -1;
    }

    pthread_mutex_lock(&c->bind_lock);
    c->attached = true;
    pthread_mutex_unlock(&c->bind_lock);
    return 0;
}

int rtcma_capturer_detach(RtcmaCapturer *c)
{
    if (!c) return -1;

    pthread_mutex_lock(&c->bind_lock);
    bool was = c->attached;
    c->attached = false;
    pthread_mutex_unlock(&c->bind_lock);

    if (was) rtcma_send_track_detach(&c->send);
    return 0;
}

int rtcma_capturer_set_volume(RtcmaCapturer *c, float volume)
{
    if (!c || !c->device) return -1;
    if (!(volume >= 0.0f && volume <= 1.0f)) return -1;
    c->volume = volume;
    return ma_device_set_master_volume(c->device, volume) == MA_SUCCESS
        ? 0 : -1;
}
