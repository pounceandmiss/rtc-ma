/*
 * Player + Capturer reopen contract test.
 *
 *  1. Pointer-based lifecycle: many reopens to the system default, each
 *     allocating a new ma_device and freeing the previous one. Catches
 *     leaks / double-frees / pointer-state bugs. Player and Capturer
 *     run the same drill independently.
 *
 *  2. Failure-recovery: when ma_device_init fails inside reopen, the
 *     handle must be left alive on the previous device and a subsequent
 *     destroy must not crash.
 *
 *     Triggering miniaudio init failure portably is hard - PulseAudio
 *     silently falls back to the system default when handed a
 *     non-existent device name. We try with a guaranteed-bogus ID; if
 *     the backend is strict we exercise the failure path, otherwise we
 *     just note it and the lifecycle coverage still validates.
 *
 * Tagged AUDIBLE - needs a real audio backend, no sound is played.
 */

#include "rtcma.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REOPEN_ITERATIONS 10

static int run_player(void)
{
    RtcmaPlayerConfig cfg = { .channels = 2 };
    RtcmaPlayer *p = rtcma_player_new(&cfg);
    if (!p) {
        fprintf(stderr, "skip: rtcma_player_new failed (no audio backend?)\n");
        return 0;
    }
    assert(rtcma_player_start(p) == 0);

    for (int i = 0; i < REOPEN_ITERATIONS; ++i) {
        int rc = rtcma_player_reopen(p, NULL);
        if (rc != 0) {
            fprintf(stderr,
                    "FAIL: player reopen(NULL) #%d returned %d\n", i, rc);
            rtcma_player_destroy(p);
            return 1;
        }
    }

    RtcmaDeviceList list = {0};
    if (rtcma_enumerate_devices(&list) == 0 && list.playback_count > 0) {
        int rc = rtcma_player_reopen(p, list.playback[0].id);
        if (rc != 0) {
            fprintf(stderr,
                    "FAIL: player reopen(pinned) returned %d\n", rc);
            rtcma_device_list_free(&list);
            rtcma_player_destroy(p);
            return 1;
        }
    }
    rtcma_device_list_free(&list);

    /* Cross-backend id_from_string mismatch *and* an obviously bogus
     * payload - strict backends reject; permissive ones fall back to
     * default. The "rtcma:" tag will fail tag-match for every real
     * backend, so id_from_string returns -1 deterministically. */
    static const char bogus[] = "rtcma:__test_invalid_device_NoMatchPossible__";

    fprintf(stderr,
            "[note] the next player reopen is *expected* to fail on strict "
            "backends; permissive backends (e.g. PulseAudio) may silently "
            "fall back to default and report success.\n");
    int rc = rtcma_player_reopen(p, bogus);
    if (rc != 0) {
        fprintf(stderr, "[ok] strict backend rejected bogus ID; "
                        "verifying player is still alive...\n");
    } else {
        fprintf(stderr, "[note] backend silently accepted bogus ID - "
                        "failure-path coverage skipped on this backend.\n");
    }

    rtcma_player_destroy(p);
    return 0;
}

static int run_capturer(void)
{
    RtcmaCapturerConfig cfg = { .channels = 2 };
    RtcmaCapturer *c = rtcma_capturer_new(&cfg);
    if (!c) {
        fprintf(stderr, "skip: rtcma_capturer_new failed (no audio backend?)\n");
        return 0;
    }
    assert(rtcma_capturer_start(c) == 0);

    for (int i = 0; i < REOPEN_ITERATIONS; ++i) {
        int rc = rtcma_capturer_reopen(c, NULL);
        if (rc != 0) {
            fprintf(stderr,
                    "FAIL: capturer reopen(NULL) #%d returned %d\n", i, rc);
            rtcma_capturer_destroy(c);
            return 1;
        }
    }

    RtcmaDeviceList list = {0};
    if (rtcma_enumerate_devices(&list) == 0 && list.capture_count > 0) {
        int rc = rtcma_capturer_reopen(c, list.capture[0].id);
        if (rc != 0) {
            fprintf(stderr,
                    "FAIL: capturer reopen(pinned) returned %d\n", rc);
            rtcma_device_list_free(&list);
            rtcma_capturer_destroy(c);
            return 1;
        }
    }
    rtcma_device_list_free(&list);

    /* Cross-backend id_from_string mismatch *and* an obviously bogus
     * payload - strict backends reject; permissive ones fall back to
     * default. The "rtcma:" tag will fail tag-match for every real
     * backend, so id_from_string returns -1 deterministically. */
    static const char bogus[] = "rtcma:__test_invalid_device_NoMatchPossible__";

    fprintf(stderr,
            "[note] the next capturer reopen is *expected* to fail on strict "
            "backends; permissive backends (e.g. PulseAudio) may silently "
            "fall back to default and report success.\n");
    int rc = rtcma_capturer_reopen(c, bogus);
    if (rc != 0) {
        fprintf(stderr, "[ok] strict backend rejected bogus ID; "
                        "verifying capturer is still alive...\n");
    } else {
        fprintf(stderr, "[note] backend silently accepted bogus ID - "
                        "failure-path coverage skipped on this backend.\n");
    }

    rtcma_capturer_destroy(c);
    return 0;
}

int main(void)
{
    int rc = run_player();
    if (rc != 0) return rc;
    rc = run_capturer();
    if (rc != 0) return rc;
    printf("Reopen test ok.\n");
    return 0;
}
