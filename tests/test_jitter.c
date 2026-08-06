/*
 * Unit test for the speexdsp jitter adapter - no libdatachannel, no audio,
 * no opus. Payloads are opaque to the buffer, so the tests tag them with
 * their timestamp and check ordering, gap handling, and the depth the
 * buffer settles at.
 *
 * The depth tests drive a tick model: one put and one get per playback
 * period, which is what the audio device callback does. The ring this
 * replaced only ever grew its play-out depth, so those two guard against
 * a buffer that buys latency on the first bad patch of network and never
 * gives it back.
 */

#include "rtcma_internal.h"

#include <assert.h>
#include <stdio.h>

#define FRAME 960                 /* 20 ms at 48 kHz */

static void tag(uint8_t *buf, uint32_t ts)
{
    buf[0] = (uint8_t)(ts >> 24);
    buf[1] = (uint8_t)(ts >> 16);
    buf[2] = (uint8_t)(ts >> 8);
    buf[3] = (uint8_t)ts;
}

static bool tag_matches(const uint8_t *buf, int len, uint32_t ts)
{
    return len >= 4
        && buf[0] == (uint8_t)(ts >> 24)
        && buf[1] == (uint8_t)(ts >> 16)
        && buf[2] == (uint8_t)(ts >> 8)
        && buf[3] == (uint8_t)ts;
}

static bool put_at(RtcmaJitter *j, uint32_t ts)
{
    uint8_t payload[16];
    tag(payload, ts);
    return rtcma_jitter_put(j, ts, (uint16_t)(ts / FRAME), payload,
                            sizeof(payload), FRAME);
}

/* Distance from the point the buffer is playing from to the newest thing
 * it has been handed, which is the latency it is holding. */
static int depth(RtcmaJitter *j, uint32_t newest_ts)
{
    return (int)(newest_ts + FRAME
                 - (uint32_t)jitter_buffer_get_pointer_timestamp(j->jb));
}

/* Left by the last pull, so the calls that care about them can look
 * without every other call site declaring out-params it ignores. */
static int last_span, last_skip;

static RtcmaJitterResult pull(RtcmaJitter *j, uint8_t *buf, int cap, int *len)
{
    return rtcma_jitter_get(j, buf, cap, len, &last_span, &last_skip);
}

static void test_silent_until_first_packet(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    uint8_t buf[64]; int len = 0;
    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_SILENT);
    assert(len == 0);

    rtcma_jitter_destroy(&j);
    printf("  silent_until_first_packet: ok\n");
}

static void test_in_order(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    for (uint32_t i = 0; i < 6; ++i) assert(put_at(&j, 48000 + i * FRAME));

    for (uint32_t i = 0; i < 6; ++i) {
        uint8_t buf[64]; int len = 0;
        assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
        assert(tag_matches(buf, len, 48000 + i * FRAME));
    }
    assert(j.stat_conceal == 0);

    rtcma_jitter_destroy(&j);
    printf("  in_order: ok\n");
}

static void test_reorder(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    const uint32_t base = 90000;
    const int order[] = { 0, 3, 1, 2, 5, 4 };
    for (int i = 0; i < 6; ++i) assert(put_at(&j, base + (uint32_t)order[i] * FRAME));

    for (uint32_t i = 0; i < 6; ++i) {
        uint8_t buf[64]; int len = 0;
        assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
        assert(tag_matches(buf, len, base + i * FRAME));
    }

    rtcma_jitter_destroy(&j);
    printf("  reorder: ok\n");
}

/* A lost frame comes back as one conceal, carrying the packet that follows
 * the gap so the caller can recover it from that packet's FEC data. The
 * packet must still play in its own right on the pull after that. */
static void test_gap_conceals(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    /* Everything but the third frame arrives. */
    const uint32_t base = 7000;
    for (uint32_t i = 0; i < 6; ++i) {
        if (i == 2) continue;
        assert(put_at(&j, base + i * FRAME));
    }

    uint8_t buf[64]; int len = 0;

    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
    assert(tag_matches(buf, len, base));
    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
    assert(tag_matches(buf, len, base + FRAME));

    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_CONCEAL);
    assert(len > 0);
    assert(tag_matches(buf, len, base + 3 * FRAME));

    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
    assert(tag_matches(buf, len, base + 3 * FRAME));
    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
    assert(tag_matches(buf, len, base + 4 * FRAME));

    assert(j.stat_conceal == 1);
    assert(j.stat_conceal_fec == 1);

    rtcma_jitter_destroy(&j);
    printf("  gap_conceals: ok\n");
}

/* No packet after the gap means nothing to recover from, and the caller
 * must be told so rather than handed a stale payload. */
static void test_gap_without_successor(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    const uint32_t base = 60000;
    assert(put_at(&j, base));
    assert(put_at(&j, base + FRAME));

    uint8_t buf[64]; int len = 0;
    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);

    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_CONCEAL);
    assert(len == 0);
    assert(j.stat_conceal_fec == 0);

    rtcma_jitter_destroy(&j);
    printf("  gap_without_successor: ok\n");
}

/* Recovering a gap needs the packet after it to be in hand at the moment
 * the gap is concealed. test_gap_conceals loads every packet up front, so
 * it cannot see whether that holds once packets arrive one at a time -
 * and it does not hold at all if the buffer plays at the live edge. Drive
 * arrivals a packet per period, the way a call does. */
static void test_gap_recoverable_when_streaming(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    uint32_t ts = 200000;
    for (int tick = 0; tick < 2000; ++tick) {
        if (tick % 97 != 0) assert(put_at(&j, ts));
        ts += FRAME;
        uint8_t buf[64]; int len = 0;
        pull(&j, buf, sizeof(buf), &len);
    }

    assert(j.stat_conceal > 10);
    /* Allow a couple of stragglers: the first gap lands before the buffer
     * has settled, and a gap can coincide with one the estimator makes. */
    assert(j.stat_conceal_fec + 3 >= j.stat_conceal);

    rtcma_jitter_destroy(&j);
    printf("  gap_recoverable_when_streaming: %llu/%llu gaps recoverable: ok\n",
           (unsigned long long)j.stat_conceal_fec,
           (unsigned long long)j.stat_conceal);
}

/* The play point does not have to land on a packet boundary once a peer
 * changes frame duration mid-stream. A packet that starts behind the play
 * point overlaps audio already emitted, and the caller is told how much of
 * the decoded frame to drop. */
static void test_late_packet_overlaps_play_point(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    const uint32_t base = 30000;
    uint8_t buf[64]; int len = 0;

    assert(put_at(&j, base));
    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
    assert(last_skip == 0);

    /* Nothing for the next period, so it conceals and moves on. */
    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_CONCEAL);
    assert(last_span == FRAME);

    /* The frame it gave up on now turns up as a 40 ms packet, straddling
     * the point playback has already reached. */
    uint8_t payload[16];
    tag(payload, base + FRAME);
    assert(rtcma_jitter_put(&j, base + FRAME, 1, payload, sizeof(payload),
                            2 * FRAME));

    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
    assert(tag_matches(buf, len, base + FRAME));
    assert(last_skip == FRAME);   /* its first 20 ms was already concealed */

    rtcma_jitter_destroy(&j);
    printf("  late_packet_overlaps_play_point: ok\n");
}

/* The mirror case: the next packet starts after the play point, leaving a
 * gap in front of it. The gap plays first and the packet must still be
 * delivered whole on the pull after that, not dropped. */
static void test_packet_after_play_point_is_held(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    const uint32_t base = 40000;
    uint8_t buf[64]; int len = 0;

    assert(put_at(&j, base));
    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);

    /* Half a frame further on than the play point expects. */
    uint8_t payload[16];
    const uint32_t off_grid = base + FRAME + FRAME / 2;
    tag(payload, off_grid);
    assert(rtcma_jitter_put(&j, off_grid, 1, payload, sizeof(payload), FRAME));

    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_CONCEAL);
    assert(last_span == FRAME / 2);

    assert(pull(&j, buf, sizeof(buf), &len) == RTCMA_JITTER_FRAME);
    assert(tag_matches(buf, len, off_grid));

    rtcma_jitter_destroy(&j);
    printf("  packet_after_play_point_is_held: ok\n");
}

/* A link that stalls and then delivers a burst. Each stall used to cost a
 * frame of permanent depth; the run must instead settle at a bounded
 * depth, and give it back once the stalls stop. */
static void test_stall_does_not_ratchet(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    uint32_t next_send = 300000;
    uint32_t newest    = next_send;
    uint32_t held      = 0;

    /* Long enough to outrun the floor the buffer keeps, so the stalls
     * still cost concealment and the depth still has to grow. */
    for (int tick = 0; tick < 1500; ++tick) {
        int stalled = (tick % 50) < 6;
        if (stalled) {
            held++;
        } else {
            for (uint32_t k = 0; k <= held; ++k) {
                newest = next_send;
                assert(put_at(&j, next_send));
                next_send += FRAME;
            }
            held = 0;
        }
        uint8_t buf[64]; int len = 0;
        pull(&j, buf, sizeof(buf), &len);
    }

    int stalled_depth = depth(&j, newest);
    assert(j.stat_conceal > 0);      /* the stalls did cost concealment */
    assert(stalled_depth <= 8 * FRAME);

    /* Link goes clean. The depth bought to ride out the stalls should come
     * back rather than staying for the rest of the call. */
    for (int tick = 0; tick < 3000; ++tick) {
        newest = next_send;
        assert(put_at(&j, next_send));
        next_send += FRAME;
        uint8_t buf[64]; int len = 0;
        pull(&j, buf, sizeof(buf), &len);
    }

    int clean_depth = depth(&j, newest);
    assert(clean_depth <= stalled_depth);
    assert(clean_depth <= 4 * FRAME);

    rtcma_jitter_destroy(&j);
    printf("  stall_does_not_ratchet: stalled=%d clean=%d samples: ok\n",
           stalled_depth, clean_depth);
}

/* Sender clock running fast: a surplus frame every so often, nothing ever
 * lost. The buffer has to shed the surplus, or the depth walks up until
 * packets fall off the end.
 *
 * One frame per 500 is 2000 ppm, an order of magnitude worse than any real
 * pair of crystals. The depth this settles at scales with the drift rate,
 * because the estimator works off a window of the last thousand or so
 * packets and monotonic drift always leaves it that far behind. */
static void test_fast_sender_sheds(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    uint32_t next_send = 5000;
    uint32_t newest    = next_send;

    for (int tick = 0; tick < 6000; ++tick) {
        int extra = (tick % 500 == 0) ? 1 : 0;
        for (int k = 0; k <= extra; ++k) {
            newest = next_send;
            assert(put_at(&j, next_send));
            next_send += FRAME;
        }
        uint8_t buf[64]; int len = 0;
        pull(&j, buf, sizeof(buf), &len);
    }

    /* The bound is the floor the buffer keeps plus room for the estimator
     * to sit above it; the point is that drift does not walk it upwards. */
    int settled = depth(&j, newest);
    assert(settled <= 5 * FRAME);
    assert(j.stat_conceal == 0);     /* shedding must not manufacture gaps */

    rtcma_jitter_destroy(&j);
    printf("  fast_sender_sheds: depth=%d samples: ok\n", settled);
}

static void test_oversized_payload_rejected(void)
{
    RtcmaJitter j;
    assert(rtcma_jitter_init(&j, FRAME) == 0);

    static uint8_t big[RTCMA_JITTER_MAX_PAYLOAD + 1];
    assert(!rtcma_jitter_put(&j, 1000, 1, big, sizeof(big), FRAME));
    assert(j.stat_put_reject == 1);
    assert(j.stat_put == 0);

    rtcma_jitter_destroy(&j);
    printf("  oversized_payload_rejected: ok\n");
}

int main(void)
{
    printf("test_jitter:\n");
    test_silent_until_first_packet();
    test_in_order();
    test_reorder();
    test_gap_conceals();
    test_gap_without_successor();
    test_gap_recoverable_when_streaming();
    test_late_packet_overlaps_play_point();
    test_packet_after_play_point_is_held();
    test_stall_does_not_ratchet();
    test_fast_sender_sheds();
    test_oversized_payload_rejected();
    printf("test_jitter: all ok\n");
    return 0;
}
