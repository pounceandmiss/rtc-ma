/* TSan regression test: rtcma_capturer/player_destroy used to detach
 * (tearing down the track) before ma_device_stop, racing on_capture/
 * on_playback. Meaningful only under ThreadSanitizer - a plain run
 * can't observe it either way. See rtc-mv's test_bitrate_race.c.
 *
 * Tagged AUDIBLE - needs a real audio backend (silent; volume muted). */

#define _GNU_SOURCE
#include "rtcma.h"

#include <rtc/rtc.h>

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define CHANNELS    2
#define ITERATIONS  25

typedef struct {
    int         pc_a;
    int         pc_b;
    int         track_a;
    _Atomic int track_b;
} Ctx;

static void a_on_desc(int pc, const char *sdp, const char *type, void *u)
{ (void)pc; rtcSetRemoteDescription(((Ctx *)u)->pc_b, sdp, type); }
static void a_on_cand(int pc, const char *cand, const char *mid, void *u)
{ (void)pc; rtcAddRemoteCandidate(((Ctx *)u)->pc_b, cand, mid); }
static void b_on_desc(int pc, const char *sdp, const char *type, void *u)
{ (void)pc; rtcSetRemoteDescription(((Ctx *)u)->pc_a, sdp, type); }
static void b_on_cand(int pc, const char *cand, const char *mid, void *u)
{ (void)pc; rtcAddRemoteCandidate(((Ctx *)u)->pc_a, cand, mid); }
static void b_on_track(int pc, int tr, void *u)
{ (void)pc; atomic_store(&((Ctx *)u)->track_b, tr); }

static int setup_pair(Ctx *ctx)
{
    rtcConfiguration cfg = {0};
    ctx->pc_a = rtcCreatePeerConnection(&cfg);
    ctx->pc_b = rtcCreatePeerConnection(&cfg);
    assert(ctx->pc_a >= 0 && ctx->pc_b >= 0);

    rtcSetUserPointer(ctx->pc_a, ctx);
    rtcSetUserPointer(ctx->pc_b, ctx);
    rtcSetLocalDescriptionCallback(ctx->pc_a, a_on_desc);
    rtcSetLocalCandidateCallback  (ctx->pc_a, a_on_cand);
    rtcSetLocalDescriptionCallback(ctx->pc_b, b_on_desc);
    rtcSetLocalCandidateCallback  (ctx->pc_b, b_on_cand);
    rtcSetTrackCallback           (ctx->pc_b, b_on_track);

    rtcTrackInit tinit = {
        .direction   = RTC_DIRECTION_SENDRECV,
        .codec       = RTC_CODEC_OPUS,
        .payloadType = 111,
        .mid         = "audio0",
        .profile     = "stereo=1;sprop-stereo=1;useinbandfec=1",
    };
    ctx->track_a = rtcAddTrackEx(ctx->pc_a, &tinit);
    assert(ctx->track_a >= 0);

    assert(rtcSetLocalDescription(ctx->pc_a, NULL) == 0);

    struct timespec wait = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
    nanosleep(&wait, NULL);

    assert(rtcSetLocalDescription(ctx->pc_b, NULL) == 0);

    for (int i = 0; i < 50 && !atomic_load(&ctx->track_b); ++i)
        nanosleep(&wait, NULL);
    return atomic_load(&ctx->track_b) > 0;
}

/* Attach + start a fresh Capturer against track_a, then destroy it after
 * a short (possibly zero) delay - racing destroy() against on_capture,
 * which the audio thread may be running right now or about to start. */
static void hammer_capturer(int track_a, int delay_us)
{
    RtcmaCapturerConfig cfg = { .channels = CHANNELS };
    RtcmaCapturer *c = rtcma_capturer_new(&cfg);
    if (!c) { fprintf(stderr, "skip: no capture device\n"); return; }
    rtcma_capturer_set_volume(c, 0.0f);  /* mute: silence is fine, no mic needed for the race */
    if (rtcma_capturer_attach(c, track_a) < 0) { rtcma_capturer_destroy(c); return; }
    if (rtcma_capturer_start(c) < 0) { rtcma_capturer_destroy(c); return; }
    if (delay_us > 0) usleep((useconds_t)delay_us);
    rtcma_capturer_destroy(c);
}

static void hammer_player(int track_b, int delay_us)
{
    RtcmaPlayerConfig cfg = { .channels = CHANNELS };
    RtcmaPlayer *p = rtcma_player_new(&cfg);
    if (!p) { fprintf(stderr, "skip: no playback device\n"); return; }
    rtcma_player_set_volume(p, 0.0f);
    if (rtcma_player_attach(p, track_b) < 0) { rtcma_player_destroy(p); return; }
    if (rtcma_player_start(p) < 0) { rtcma_player_destroy(p); return; }
    if (delay_us > 0) usleep((useconds_t)delay_us);
    rtcma_player_destroy(p);
}

int main(void)
{
    rtcInitLogger(RTC_LOG_WARNING, NULL);

    Ctx ctx = {0};
    atomic_init(&ctx.track_b, 0);
    if (!setup_pair(&ctx)) {
        fprintf(stderr, "FAIL: peer B never received a track\n");
        return 1;
    }
    int track_b = atomic_load(&ctx.track_b);

    /* Vary the start-to-destroy delay across iterations (including 0)
     * to hit different phases of the audio device's callback period -
     * maximizes the chance of destroy() landing while on_capture /
     * on_playback is actually in flight. */
    for (int i = 0; i < ITERATIONS; ++i) {
        hammer_capturer(ctx.track_a, (i % 7) * 500);
        hammer_player(track_b, (i % 5) * 700);
    }

    if (track_b > 0) rtcDeleteTrack(track_b);
    if (ctx.track_a > 0) rtcDeleteTrack(ctx.track_a);
    rtcDeletePeerConnection(ctx.pc_a);
    rtcDeletePeerConnection(ctx.pc_b);
    rtcCleanup();

    printf("test_destroy_device_race: OK (no crash; meaningful only "
           "under ThreadSanitizer)\n");
    return 0;
}
