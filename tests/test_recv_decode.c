/*
 * Decode-side buffer-size regression test.
 *
 * `opus_decode` writes whatever per-channel sample count the packet
 * encodes for and returns OPUS_BUFFER_TOO_SMALL (-2) if the caller's
 * frame_size argument can't hold it. WebRTC peers (Conversations,
 * libwebrtc browsers) ship 20 ms frames most of the time but switch to
 * 40 ms or 60 ms under bandwidth pressure - the live `-2` errors in
 * the field came from a hardcoded 20 ms decode budget. This test
 * synthesises real opus packets at 20, 40, and 60 ms and verifies
 * `rtcma_recv_track_pull_pcm` returns the right per-channel sample
 * count for each, end-to-end through the jitter ring.
 *
 * No libdatachannel: the test manually populates the recv track and
 * pushes payloads into the jitter ring directly. rtc_track_id is
 * faked to 0 so the pull_pcm "not attached" early return doesn't fire.
 */

#include "rtcma_internal.h"

#include <opus/opus.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Encode `samples_per_channel` int16 samples of `pcm` into `out` and
 * return the opus packet size. Asserts on encoder failure. */
static int encode_frame(OpusEncoder *enc, const int16_t *pcm,
                        int samples_per_channel,
                        unsigned char *out, int out_cap)
{
    int n = opus_encode(enc, pcm, samples_per_channel, out, out_cap);
    if (n <= 0) {
        fprintf(stderr, "opus_encode(%d samples) failed: %d\n",
                samples_per_channel, n);
        assert(n > 0);
    }
    return n;
}

/* Fill `pcm` with a low-amplitude sine. Pure silence works too but the
 * resulting packets are 2-3 bytes and easy to confuse for malformed
 * data when stepping through a failure. */
static void fill_sine(int16_t *pcm, int samples, int *phase)
{
    for (int i = 0; i < samples; ++i) {
        /* ~440 Hz at 48 kHz, low amplitude. */
        int p = (*phase + i) % 109;
        pcm[i] = (int16_t)((p - 54) * 100);
    }
    *phase = (*phase + samples) % 109;
}

int main(void)
{
    int err = 0;

    OpusEncoder *enc = opus_encoder_create(RTCMA_SAMPLE_RATE, 1,
                                           OPUS_APPLICATION_AUDIO, &err);
    assert(enc && err == OPUS_OK);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));

    RtcmaRecvTrack t = {0};
    t.rtc_track_id = 0;          /* fake attached state */
    t.payload_type = 111;
    t.channels     = 1;
    t.dec = opus_decoder_create(RTCMA_SAMPLE_RATE, 1, &err);
    assert(t.dec && err == OPUS_OK);
    rtcma_jitter_init(&t.jitter);

    /* Build one 20 ms, one 40 ms, and one 60 ms opus packet. */
    int16_t pcm_20[960];
    int16_t pcm_40[1920];
    int16_t pcm_60[2880];
    unsigned char buf_20[1500];
    unsigned char buf_40[1500];
    unsigned char buf_60[1500];
    int phase = 0;
    fill_sine(pcm_20, 960,  &phase);
    fill_sine(pcm_40, 1920, &phase);
    fill_sine(pcm_60, 2880, &phase);

    int len_20 = encode_frame(enc, pcm_20, 960,  buf_20, sizeof(buf_20));
    int len_40 = encode_frame(enc, pcm_40, 1920, buf_40, sizeof(buf_40));
    int len_60 = encode_frame(enc, pcm_60, 2880, buf_60, sizeof(buf_60));

    /* Queue all three into the jitter ring. Need >=2 frames before
     * pull_pcm starts draining (prime_threshold=2); enqueuing all three
     * up front primes it on the first pull and the rest play back-to-back. */
    assert(rtcma_jitter_put(&t.jitter, 0, buf_20, len_20));
    assert(rtcma_jitter_put(&t.jitter, 1, buf_40, len_40));
    assert(rtcma_jitter_put(&t.jitter, 2, buf_60, len_60));

    int16_t out[RTCMA_DECODE_MAX_SAMPLES];
    const size_t out_cap = sizeof(out) / sizeof(out[0]);

    /* 20 ms - the historical happy path. */
    int n = rtcma_recv_track_pull_pcm(&t, out, out_cap);
    assert(n == 960);

    /* 40 ms - pre-fix: opus_decode returned OPUS_BUFFER_TOO_SMALL (-2)
     * because the 960-sample frame_size couldn't hold the 1920-sample
     * decoded frame. */
    n = rtcma_recv_track_pull_pcm(&t, out, out_cap);
    assert(n == 1920);

    /* 60 ms - same family, larger size. */
    n = rtcma_recv_track_pull_pcm(&t, out, out_cap);
    assert(n == 2880);

    /* Buffer-too-small check: callers must size for the worst case. */
    int16_t small_out[RTCMA_FRAME_SAMPLES];   /* 960 - old contract */
    int rc = rtcma_recv_track_pull_pcm(&t, small_out, RTCMA_FRAME_SAMPLES);
    assert(rc == -1);

    /* Subsequent pull with the ring empty runs PLC (jitter is now in
     * `playing` state). PLC asks for one 20 ms frame regardless of the
     * previous packet's duration, so we expect 960. */
    n = rtcma_recv_track_pull_pcm(&t, out, out_cap);
    assert(n == 960);

    opus_decoder_destroy(t.dec);
    opus_encoder_destroy(enc);
    rtcma_jitter_destroy(&t.jitter);

    printf("test_recv_decode: 20/40/60 ms frames all decoded\n");
    return 0;
}
