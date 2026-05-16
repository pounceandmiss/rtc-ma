/*
 * Pure unit test for RtcmaJitter - no libdatachannel, no audio, no opus.
 * Exercises seq/timestamp ordering, gap handling, and reset behaviour
 * so any regression in the jitter buffer surfaces here.
 */

#include "rtcma_internal.h"

#include <assert.h>
#include <stdio.h>

static void tag(uint8_t *buf, uint16_t seq)
{
    buf[0] = (uint8_t)(seq >> 8);
    buf[1] = (uint8_t)(seq & 0xFF);
}

static bool tag_matches(const uint8_t *buf, int len, uint16_t seq)
{
    return len >= 2
        && buf[0] == (uint8_t)(seq >> 8)
        && buf[1] == (uint8_t)(seq & 0xFF);
}

static void test_in_order(void)
{
    RtcmaJitter j;
    rtcma_jitter_init(&j);

    uint8_t payload[16];
    for (uint16_t i = 0; i < 6; ++i) {
        tag(payload, 1000 + i);
        assert(rtcma_jitter_put(&j, 1000 + i, payload, sizeof(payload)));
    }

    for (uint16_t i = 0; i < 6; ++i) {
        uint8_t buf[64]; int len = 0;
        bool present = rtcma_jitter_get(&j, buf, sizeof(buf), &len);
        assert(present);
        assert(tag_matches(buf, len, 1000 + i));
    }
    assert(j.stat_late_drop == 0);
    assert(j.stat_dup_drop == 0);
    rtcma_jitter_destroy(&j);
    printf("  in_order: ok\n");
}

static void test_reorder(void)
{
    RtcmaJitter j;
    rtcma_jitter_init(&j);

    uint8_t payload[16];
    tag(payload, 2000); rtcma_jitter_put(&j, 2000, payload, sizeof(payload));
    uint16_t shuffled[] = { 2003, 2001, 2005, 2002, 2004 };
    for (int i = 0; i < 5; ++i) {
        tag(payload, shuffled[i]);
        assert(rtcma_jitter_put(&j, shuffled[i], payload, sizeof(payload)));
    }

    for (uint16_t expected = 2000; expected <= 2005; ++expected) {
        uint8_t buf[64]; int len = 0;
        bool present = rtcma_jitter_get(&j, buf, sizeof(buf), &len);
        assert(present);
        assert(tag_matches(buf, len, expected));
    }
    rtcma_jitter_destroy(&j);
    printf("  reorder: ok\n");
}

static void test_late_drop(void)
{
    RtcmaJitter j;
    rtcma_jitter_init(&j);

    uint8_t payload[16];
    for (uint16_t i = 0; i < 4; ++i) {
        tag(payload, 5000 + i);
        rtcma_jitter_put(&j, 5000 + i, payload, sizeof(payload));
    }
    for (int i = 0; i < 4; ++i) {
        uint8_t buf[64]; int len = 0;
        rtcma_jitter_get(&j, buf, sizeof(buf), &len);
    }

    tag(payload, 4990);
    assert(!rtcma_jitter_put(&j, 4990, payload, sizeof(payload)));
    assert(j.stat_late_drop == 1);

    tag(payload, 5004 - 3);
    assert(rtcma_jitter_put(&j, 5004 - 3, payload, sizeof(payload)));

    rtcma_jitter_destroy(&j);
    printf("  late_drop: ok\n");
}

static void test_miss_advances(void)
{
    RtcmaJitter j;
    rtcma_jitter_init(&j);

    uint8_t payload[16];
    tag(payload, 100); rtcma_jitter_put(&j, 100, payload, sizeof(payload));
    tag(payload, 101); rtcma_jitter_put(&j, 101, payload, sizeof(payload));
    tag(payload, 103); rtcma_jitter_put(&j, 103, payload, sizeof(payload));
    tag(payload, 104); rtcma_jitter_put(&j, 104, payload, sizeof(payload));

    uint8_t buf[64]; int len = 0;
    assert(rtcma_jitter_get(&j, buf, sizeof(buf), &len));
    assert(tag_matches(buf, len, 100));
    assert(rtcma_jitter_get(&j, buf, sizeof(buf), &len));
    assert(tag_matches(buf, len, 101));
    /* seq 102 - MISS, head must advance */
    assert(!rtcma_jitter_get(&j, buf, sizeof(buf), &len));
    assert(len == 0);
    assert(rtcma_jitter_get(&j, buf, sizeof(buf), &len));
    assert(tag_matches(buf, len, 103));
    assert(rtcma_jitter_get(&j, buf, sizeof(buf), &len));
    assert(tag_matches(buf, len, 104));
    assert(j.stat_get_miss == 1);

    rtcma_jitter_destroy(&j);
    printf("  miss_advances: ok\n");
}

static void test_wraparound(void)
{
    RtcmaJitter j;
    rtcma_jitter_init(&j);

    uint8_t payload[16];
    uint16_t seqs[] = { 65533, 65534, 65535, 0, 1, 2 };
    for (int i = 0; i < 6; ++i) {
        tag(payload, seqs[i]);
        assert(rtcma_jitter_put(&j, seqs[i], payload, sizeof(payload)));
    }
    for (int i = 0; i < 6; ++i) {
        uint8_t buf[64]; int len = 0;
        bool present = rtcma_jitter_get(&j, buf, sizeof(buf), &len);
        assert(present);
        assert(tag_matches(buf, len, seqs[i]));
    }
    rtcma_jitter_destroy(&j);
    printf("  wraparound: ok\n");
}

static void test_duplicate(void)
{
    RtcmaJitter j;
    rtcma_jitter_init(&j);

    uint8_t payload[16];
    tag(payload, 700); rtcma_jitter_put(&j, 700, payload, sizeof(payload));
    tag(payload, 701); rtcma_jitter_put(&j, 701, payload, sizeof(payload));
    tag(payload, 700);
    assert(!rtcma_jitter_put(&j, 700, payload, sizeof(payload)));
    assert(j.stat_dup_drop == 1);

    rtcma_jitter_destroy(&j);
    printf("  duplicate: ok\n");
}

static void test_big_jump_skips_stale(void)
{
    RtcmaJitter j;
    rtcma_jitter_init(&j);

    uint8_t payload[16];
    for (uint16_t i = 0; i < 5; ++i) {
        tag(payload, 9000 + i);
        rtcma_jitter_put(&j, 9000 + i, payload, sizeof(payload));
    }
    uint8_t buf[64]; int len = 0;
    assert(rtcma_jitter_get(&j, buf, sizeof(buf), &len));
    assert(tag_matches(buf, len, 9000));
    assert(rtcma_jitter_get(&j, buf, sizeof(buf), &len));
    assert(tag_matches(buf, len, 9001));
    assert(j.playing);
    assert(j.fill_count == 3);

    tag(payload, 9050);
    assert(rtcma_jitter_put(&j, 9050, payload, sizeof(payload)));
    assert(j.fill_count == 1);

    int misses = 0;
    while (1) {
        bool present = rtcma_jitter_get(&j, buf, sizeof(buf), &len);
        if (present) {
            assert(tag_matches(buf, len, 9050));
            break;
        }
        misses++;
        if (misses > 100) { fprintf(stderr, "FAIL: never reached 9050\n"); assert(0); }
    }
    assert(misses > 0);

    rtcma_jitter_destroy(&j);
    printf("  big_jump_skips_stale: ok\n");
}

int main(void)
{
    printf("test_jitter:\n");
    test_in_order();
    test_reorder();
    test_late_drop();
    test_miss_advances();
    test_wraparound();
    test_duplicate();
    test_big_jump_skips_stale();
    printf("all jitter tests ok.\n");
    return 0;
}
