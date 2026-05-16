/*
 * Audible mic-loopback test: mic -> RtcmaCapturer -> libdatachannel ->
 * RtcmaPlayer -> speakers, in one process via two looped PCs.
 *
 * This is the only audible test that exercises the Capturer's audio
 * device (mic open + on_capture callback + encode + rtcSendMessage).
 * test_audible_loopback injects via the internal send-track API and
 * bypasses miniaudio capture entirely.
 *
 *   Peer A: mic -> RtcmaCapturer -> opus encode -> libdatachannel
 *   Peer B: <- libdatachannel -> RtcmaPlayer -> speakers
 *
 * ! FEEDBACK WARNING -------------------------------------------------
 *  Looping your mic to your speakers without headphones produces a
 *  howling squeal. The test prints a banner and counts down before
 *  opening the mic so you have a moment to put headphones on (or
 *  drop the speaker volume to zero - you'll still see the connection
 *  succeed in the logs).
 *
 * Tagged AUDIBLE - needs a real audio backend with a working mic.
 */

#define _GNU_SOURCE
#include "rtcma.h"

#include <rtc/rtc.h>

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DURATION_S  4
#define CHANNELS    2

typedef struct {
    int             pc_a;
    int             pc_b;
    _Atomic int     track_b;
    _Atomic int     connected_a;
    _Atomic int     connected_b;
    RtcmaCapturer  *capturer;
    RtcmaPlayer    *player;
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

    if (rtcma_player_attach(ctx->player, tr) < 0) {
        fprintf(stderr, "[peer B] rtcma_player_attach failed\n");
        return;
    }
    if (rtcma_player_start(ctx->player) < 0) {
        fprintf(stderr, "[peer B] rtcma_player_start failed\n");
        return;
    }
    fprintf(stderr, "[peer B] inbound track wired and playing\n");
}

int main(void)
{
    Ctx ctx = {0};
    atomic_init(&ctx.connected_a, 0);
    atomic_init(&ctx.connected_b, 0);
    atomic_init(&ctx.track_b,     0);

    printf("\n");
    printf("  +----------------------------------------------------------+\n");
    printf("  |  !  PUT ON HEADPHONES (or mute your speakers).           |\n");
    printf("  |     This test loops your mic to your speakers and WILL   |\n");
    printf("  |     feed back into a howling squeal otherwise.           |\n");
    printf("  +----------------------------------------------------------+\n");
    printf("\n");
    for (int i = 3; i > 0; --i) {
        printf("  opening mic in %d...\n", i);
        fflush(stdout);
        struct timespec one_sec = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&one_sec, NULL);
    }
    printf("\n");

    /* Player and Capturer each own their own audio device. */
    RtcmaPlayerConfig pcfg = { .channels = CHANNELS };
    ctx.player = rtcma_player_new(&pcfg);
    if (!ctx.player) {
        fprintf(stderr, "skip: rtcma_player_new failed (no audio backend?)\n");
        return 0;
    }

    RtcmaCapturerConfig ccfg = { .channels = CHANNELS };
    ctx.capturer = rtcma_capturer_new(&ccfg);
    if (!ctx.capturer) {
        fprintf(stderr, "skip: rtcma_capturer_new failed (no mic?)\n");
        rtcma_player_destroy(ctx.player);
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
    assert(rtcma_capturer_attach(ctx.capturer, track_a) == 0);

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

    assert(rtcma_capturer_start(ctx.capturer) == 0);

    printf("Listening for %d seconds - speak into the mic; you should "
           "hear yourself in the headphones.\n", DURATION_S);
    fflush(stdout);

    struct timespec dur = { .tv_sec = DURATION_S, .tv_nsec = 0 };
    nanosleep(&dur, NULL);

    int ca = atomic_load(&ctx.connected_a);
    int cb = atomic_load(&ctx.connected_b);
    fprintf(stderr, "[result] peer A connected: %d, peer B connected: %d\n",
            ca, cb);

    rtcma_capturer_destroy(ctx.capturer);
    rtcma_player_destroy(ctx.player);
    rtcDeletePeerConnection(ctx.pc_a);
    rtcDeletePeerConnection(ctx.pc_b);
    rtcCleanup();

    if (!ca || !cb) {
        fprintf(stderr, "FAIL: peers did not both reach CONNECTED\n");
        return 1;
    }
    printf("Audible mic-loopback test finished.\n");
    return 0;
}
