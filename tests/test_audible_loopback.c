/*
 * Audible loopback test, end-to-end through the public rtc-ma Player API.
 *
 * Two libdatachannel peer connections in the same process, signaling
 * forwarded directly. Peer A pushes a synthesised 440 Hz tone through a
 * send track (we use the internal send-track API directly so we don't
 * need real mic hardware). Peer B receives RTP on its inbound track
 * and pipes it to the speakers via an RtcmaPlayer.
 *
 *   Peer A: tone gen -> opus encode -> libdatachannel -> DTLS/SRTP/UDP loopback
 *   Peer B: <- libdatachannel -> rtcma_recv_track jitter -> opus decode -> ring
 *           -> miniaudio -> speakers
 *
 * Tagged AUDIBLE - needs a real audio backend.
 */

#define _GNU_SOURCE
#include "rtcma.h"
#include "rtcma_internal.h"  /* for the tone-generator's send-track */

#include <rtc/rtc.h>

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VOLUME      0.05
#define DURATION_S  6
#define TONE_HZ     440.0
#define CHANNELS    2

typedef struct {
    int             pc_a;
    int             pc_b;
    _Atomic int     track_b;
    _Atomic int     connected_a;
    _Atomic int     connected_b;
    RtcmaSendTrack  send_a;
    RtcmaPlayer    *player;
    pthread_t       tone_thread;
    _Atomic int     stop;
} Ctx;

static void a_on_desc(int pc, const char *sdp, const char *type, void *u)
{ (void)pc; rtcSetRemoteDescription(((Ctx *)u)->pc_b, sdp, type); }
static void a_on_cand(int pc, const char *cand, const char *mid, void *u)
{ (void)pc; rtcAddRemoteCandidate(((Ctx *)u)->pc_b, cand, mid); }
static void b_on_desc(int pc, const char *sdp, const char *type, void *u)
{ (void)pc; rtcSetRemoteDescription(((Ctx *)u)->pc_a, sdp, type); }
static void b_on_cand(int pc, const char *cand, const char *mid, void *u)
{ (void)pc; rtcAddRemoteCandidate(((Ctx *)u)->pc_a, cand, mid); }

static void a_on_state(int pc, rtcState s, void *u)
{
    (void)pc;
    if (s == RTC_CONNECTED) atomic_store(&((Ctx *)u)->connected_a, 1);
    fprintf(stderr, "[peer A] state=%d\n", (int)s);
}
static void b_on_state(int pc, rtcState s, void *u)
{
    (void)pc;
    if (s == RTC_CONNECTED) atomic_store(&((Ctx *)u)->connected_b, 1);
    fprintf(stderr, "[peer B] state=%d\n", (int)s);
}

static void b_on_track(int pc, int tr, void *u)
{
    (void)pc;
    Ctx *ctx = u;
    atomic_store(&ctx->track_b, tr);

    /* Attach inside the track callback - by spec libdatachannel has
     * already populated the track's description by this point, so the
     * auto-detect in rtcma_player_attach can see PT + sprop-stereo. */
    if (rtcma_player_attach(ctx->player, tr) < 0) {
        fprintf(stderr, "[peer B] rtcma_player_attach failed\n");
        return;
    }
    fprintf(stderr, "[peer B] inbound audio track wired to Player\n");
}

static void *tone_thread_fn(void *arg)
{
    Ctx *ctx = arg;
    int16_t pcm[RTCMA_FRAME_SAMPLES * CHANNELS];
    double phase = 0.0;
    const double phase_inc = 2.0 * M_PI * TONE_HZ / (double)RTCMA_SAMPLE_RATE;
    const int16_t amp = (int16_t)(VOLUME * 32767.0);

    /* Wait briefly for A to come up before pumping audio. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    for (int i = 0; i < 100 && !atomic_load(&ctx->connected_a); ++i)
        nanosleep(&ts, NULL);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!atomic_load(&ctx->stop)) {
        for (int i = 0; i < RTCMA_FRAME_SAMPLES; ++i) {
            int16_t s = (int16_t)(amp * sin(phase));
            phase += phase_inc;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            for (int c = 0; c < CHANNELS; ++c)
                pcm[i * CHANNELS + c] = s;
        }
        rtcma_send_track_push_pcm(&ctx->send_a, pcm, RTCMA_FRAME_SAMPLES);

        next.tv_nsec += 20 * 1000 * 1000;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec  += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

int main(void)
{
    Ctx ctx = {0};
    atomic_init(&ctx.connected_a, 0);
    atomic_init(&ctx.connected_b, 0);
    atomic_init(&ctx.track_b,     0);
    atomic_init(&ctx.stop,        0);
    ctx.send_a.rtc_track_id = -1;

    RtcmaPlayerConfig pcfg = {
        .channels = CHANNELS,
        /* leave payload_type at 0 -> auto-detect from SDP */
    };
    ctx.player = rtcma_player_new(&pcfg);
    if (!ctx.player) {
        fprintf(stderr, "skip: rtcma_player_new failed (no audio backend?)\n");
        return 0;
    }

    rtcConfiguration rcfg = {0};
    ctx.pc_a = rtcCreatePeerConnection(&rcfg);
    ctx.pc_b = rtcCreatePeerConnection(&rcfg);
    assert(ctx.pc_a >= 0 && ctx.pc_b >= 0);

    rtcSetUserPointer(ctx.pc_a, &ctx);
    rtcSetUserPointer(ctx.pc_b, &ctx);
    rtcSetLocalDescriptionCallback(ctx.pc_a, a_on_desc);
    rtcSetLocalCandidateCallback  (ctx.pc_a, a_on_cand);
    rtcSetStateChangeCallback     (ctx.pc_a, a_on_state);
    rtcSetLocalDescriptionCallback(ctx.pc_b, b_on_desc);
    rtcSetLocalCandidateCallback  (ctx.pc_b, b_on_cand);
    rtcSetStateChangeCallback     (ctx.pc_b, b_on_state);
    rtcSetTrackCallback           (ctx.pc_b, b_on_track);

    rtcTrackInit tinit = {
        .direction   = RTC_DIRECTION_SENDRECV,
        .codec       = RTC_CODEC_OPUS,
        .payloadType = 111,
        .mid         = "audio0",
        .profile     = "stereo=1;sprop-stereo=1;useinbandfec=1",
    };
    int track_a = rtcAddTrackEx(ctx.pc_a, &tinit);
    assert(track_a >= 0);

    /* rtc-ma builds its own RTP header in rtcma_send_track_push_pcm
     * and ships through rtcSendMessage with no media handler installed
     * on the track. Do NOT call rtcSetOpusPacketizer here - it would
     * double-wrap every outgoing packet. */
    assert(rtcma_send_track_attach(&ctx.send_a, track_a, CHANNELS, 111) == 0);

    int rc = rtcSetLocalDescription(ctx.pc_a, NULL);
    assert(rc == 0);

    struct timespec wait = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
    nanosleep(&wait, NULL);

    rc = rtcSetLocalDescription(ctx.pc_b, NULL);
    assert(rc == 0);

    for (int i = 0; i < 50 && !atomic_load(&ctx.track_b); ++i)
        nanosleep(&wait, NULL);
    if (!atomic_load(&ctx.track_b)) {
        fprintf(stderr, "FAIL: peer B never received a track\n");
        return 1;
    }

    assert(rtcma_player_start(ctx.player) == 0);
    pthread_create(&ctx.tone_thread, NULL, tone_thread_fn, &ctx);

    printf("Listening for %d seconds - you should hear a steady "
           "%.0f Hz tone at volume %.2f.\n",
           DURATION_S, TONE_HZ, VOLUME);
    fflush(stdout);

    struct timespec dur = { .tv_sec = DURATION_S, .tv_nsec = 0 };
    nanosleep(&dur, NULL);

    atomic_store(&ctx.stop, 1);
    pthread_join(ctx.tone_thread, NULL);

    int ca = atomic_load(&ctx.connected_a);
    int cb = atomic_load(&ctx.connected_b);
    fprintf(stderr, "[result] peer A connected: %d, peer B connected: %d\n",
            ca, cb);

    rtcma_player_destroy(ctx.player);
    rtcma_send_track_detach(&ctx.send_a);
    rtcDeletePeerConnection(ctx.pc_a);
    rtcDeletePeerConnection(ctx.pc_b);
    rtcCleanup();

    if (!ca || !cb) {
        fprintf(stderr, "FAIL: peers did not both reach CONNECTED\n");
        return 1;
    }
    printf("Audible loopback test finished.\n");
    return 0;
}
