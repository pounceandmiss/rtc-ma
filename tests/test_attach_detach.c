/*
 * Lifecycle contract test for rtcma_recv_track + rtcma_send_track.
 * No audio hardware required - exercises the libdatachannel-side wiring
 * (rtcSetMessageCallback / rtcSetUserPointer / rtcSendMessage) and the
 * attach/detach handshake against two real PCs looped in-process.
 *
 * Verifies:
 *   1. Auto-detect of PT + channels from track SDP works against
 *      libdatachannel's actual rtcGetTrackPayloadTypesForCodec /
 *      rtcGetTrackDescription output.
 *   2. Many attach -> push some RTP -> detach cycles leave no objects
 *      behind in libdatachannel's internal maps (eraseAll log capture).
 *   3. Detach is callable when not attached and never crashes.
 *
 * Uses the *internal* track API directly so the test does not depend on
 * miniaudio (which would fail on a headless CI box). The public
 * RtcmaPlayer / RtcmaCapturer happy path is covered by the AUDIBLE
 * loopback test.
 */

#define _GNU_SOURCE
#include "rtcma_internal.h"

#include <rtc/rtc.h>

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ITERATIONS 5
#define CHANNELS   2

typedef struct {
    int             pc_a;
    int             pc_b;
    int             track_a;            /* outgoing on pc_a */
    _Atomic int     track_b;            /* set from on_track on pc_b */
    _Atomic int     connected_a;
    _Atomic int     connected_b;
} Ctx;

static _Atomic int g_leak_lines = 0;
static char        g_last_leak_message[512];

static void log_cb(rtcLogLevel level, const char *message)
{
    (void)level;
    if (!message) return;
    if (strstr(message, "not properly destroyed")) {
        atomic_fetch_add(&g_leak_lines, 1);
        strncpy(g_last_leak_message, message,
                sizeof(g_last_leak_message) - 1);
    }
}

/* -- signaling forwarders (A <-> B in-process) ------------------------- */

static void a_on_desc(int pc, const char *sdp, const char *type, void *u)
{ (void)pc; rtcSetRemoteDescription(((Ctx *)u)->pc_b, sdp, type); }
static void a_on_cand(int pc, const char *cand, const char *mid, void *u)
{ (void)pc; rtcAddRemoteCandidate(((Ctx *)u)->pc_b, cand, mid); }
static void b_on_desc(int pc, const char *sdp, const char *type, void *u)
{ (void)pc; rtcSetRemoteDescription(((Ctx *)u)->pc_a, sdp, type); }
static void b_on_cand(int pc, const char *cand, const char *mid, void *u)
{ (void)pc; rtcAddRemoteCandidate(((Ctx *)u)->pc_a, cand, mid); }

static void a_on_state(int pc, rtcState s, void *u)
{ (void)pc; if (s == RTC_CONNECTED) atomic_store(&((Ctx *)u)->connected_a, 1); }
static void b_on_state(int pc, rtcState s, void *u)
{ (void)pc; if (s == RTC_CONNECTED) atomic_store(&((Ctx *)u)->connected_b, 1); }

static void b_on_track(int pc, int tr, void *u)
{ (void)pc; atomic_store(&((Ctx *)u)->track_b, tr); }

/* Create one PC pair and walk it through to CONNECTED with a single
 * sendrecv Opus audio track originating from A. Returns the inbound
 * track id on B (or -1). */
static int setup_pair(Ctx *ctx, int *track_a_out)
{
    rtcConfiguration cfg = {0};
    ctx->pc_a = rtcCreatePeerConnection(&cfg);
    ctx->pc_b = rtcCreatePeerConnection(&cfg);
    assert(ctx->pc_a >= 0 && ctx->pc_b >= 0);

    rtcSetUserPointer(ctx->pc_a, ctx);
    rtcSetUserPointer(ctx->pc_b, ctx);

    rtcSetLocalDescriptionCallback(ctx->pc_a, a_on_desc);
    rtcSetLocalCandidateCallback  (ctx->pc_a, a_on_cand);
    rtcSetStateChangeCallback     (ctx->pc_a, a_on_state);

    rtcSetLocalDescriptionCallback(ctx->pc_b, b_on_desc);
    rtcSetLocalCandidateCallback  (ctx->pc_b, b_on_cand);
    rtcSetStateChangeCallback     (ctx->pc_b, b_on_state);
    rtcSetTrackCallback           (ctx->pc_b, b_on_track);

    rtcTrackInit tinit = {
        .direction   = RTC_DIRECTION_SENDRECV,
        .codec       = RTC_CODEC_OPUS,
        .payloadType = 111,
        .mid         = "audio0",
        .profile     = "stereo=1;sprop-stereo=1;useinbandfec=1",
    };
    int track_a = rtcAddTrackEx(ctx->pc_a, &tinit);
    assert(track_a >= 0);
    ctx->track_a = track_a;
    *track_a_out = track_a;

    /* rtcma_send_track_attach installs OpusRtpPacketizer itself; the
     * test no longer needs to pre-install one. (Doing so here would be
     * harmless - setMediaHandler would just get overwritten by the
     * adapter - but the omission documents the new contract.) */

    int rc = rtcSetLocalDescription(ctx->pc_a, NULL);
    assert(rc == 0);

    /* Let B's track callback fire before B answers. */
    struct timespec wait = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
    nanosleep(&wait, NULL);

    rc = rtcSetLocalDescription(ctx->pc_b, NULL);
    assert(rc == 0);

    int track_b = -1;
    for (int i = 0; i < 50; ++i) {
        track_b = atomic_load(&ctx->track_b);
        if (track_b > 0) break;
        nanosleep(&wait, NULL);
    }
    return track_b;
}

static void teardown_pair(Ctx *ctx)
{
    /* rtcDelete{PeerConnection,Track} are the only paths that erase
     * from libdatachannel's C-API maps. Closing the PC does NOT cascade
     * to its tracks' map entries, so each side's track must be deleted
     * explicitly or shared_ptr<Track> + RtpConfig + RtcpSrReporter leak.
     * This is the documented contract for any rtc-ma consumer: rtc-ma
     * never deletes tracks on its own. */
    int track_b = atomic_load(&ctx->track_b);
    if (track_b > 0) rtcDeleteTrack(track_b);
    if (ctx->track_a > 0) rtcDeleteTrack(ctx->track_a);
    rtcDeletePeerConnection(ctx->pc_a);
    rtcDeletePeerConnection(ctx->pc_b);
    ctx->pc_a = ctx->pc_b = -1;
    ctx->track_a = 0;
    atomic_store(&ctx->track_b, 0);
    atomic_store(&ctx->connected_a, 0);
    atomic_store(&ctx->connected_b, 0);
}

int main(void)
{
    rtcInitLogger(RTC_LOG_INFO, log_cb);

    for (int iter = 0; iter < ITERATIONS; ++iter) {
        Ctx ctx = {0};
        atomic_init(&ctx.track_b, 0);
        atomic_init(&ctx.connected_a, 0);
        atomic_init(&ctx.connected_b, 0);

        int track_a = -1;
        int track_b = setup_pair(&ctx, &track_a);
        if (track_b <= 0) {
            fprintf(stderr,
                    "FAIL (iter %d): peer B never received a track\n", iter);
            teardown_pair(&ctx);
            return 1;
        }

        /* (1) Auto-detect both PT and channels via SDP probe. */
        RtcmaRecvTrack recv = { .rtc_track_id = -1 };
        int rc = rtcma_recv_track_attach(&recv, track_b, /*ch*/0, /*pt*/0);
        if (rc != 0) {
            fprintf(stderr,
                    "FAIL (iter %d): rtcma_recv_track_attach (auto) failed\n",
                    iter);
            teardown_pair(&ctx);
            return 1;
        }
        /* sprop-stereo=1 was in our offer SDP -> channels==2. */
        assert(recv.channels == 2);
        assert(recv.payload_type == 111);

        RtcmaSendTrack send = { .rtc_track_id = -1 };
        rc = rtcma_send_track_attach(&send, track_a, CHANNELS, 111);
        assert(rc == 0);

        /* Push a few silent 20 ms frames so the send->recv path runs at
         * least once; libdatachannel may or may not be CONNECTED yet
         * but rtcSendMessage on a not-yet-connected DTLS-SRTP track is
         * a documented no-op rather than a fatal error. */
        int16_t silence[RTCMA_FRAME_SAMPLES * CHANNELS] = {0};
        for (int i = 0; i < 5; ++i) {
            rtcma_send_track_push_pcm(&send, silence, RTCMA_FRAME_SAMPLES);
        }
        struct timespec wait = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&wait, NULL);

        rtcma_recv_track_detach(&recv);
        rtcma_send_track_detach(&send);

        /* (3) Double-detach is a no-op. */
        rtcma_recv_track_detach(&recv);
        rtcma_send_track_detach(&send);

        teardown_pair(&ctx);
    }

    /* (2) eraseAll runs in rtcCleanup. If any iteration leaked, log_cb
     * captures the count message. */
    rtcCleanup();

    int leaks = atomic_load(&g_leak_lines);
    if (leaks != 0) {
        fprintf(stderr,
                "FAIL: libdatachannel reported leak(s) at cleanup:\n  %s\n",
                g_last_leak_message);
        return 1;
    }

    printf("test_attach_detach: %d iterations, zero leaked objects.\n",
           ITERATIONS);
    return 0;
}
