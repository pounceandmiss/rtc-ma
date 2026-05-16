/*
 * Pure unit test for the opus SDP probe - no libdatachannel.
 *
 * The probe must default to mono when sprop-stereo is absent (RFC 7587
 * default; what Dino-style senders emit) and only switch to stereo when
 * the sender explicitly declares sprop-stereo=1. Several cases also
 * guard against substring traps (PT 11 vs 111) and the stereo=1 /
 * sprop-stereo=1 distinction.
 *
 * The expanded probe also reads useinbandfec / usedtx / cbr /
 * maxaveragebitrate / maxplaybackrate / minptime; we exercise those
 * via CHECK_PARAMS below.
 */

#include "rtcma_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK_CH(sdp, pt, want)                                                 \
    do {                                                                        \
        int _g = rtcma_opus_channels_from_sdp((sdp), (pt));                     \
        if (_g != (want)) {                                                     \
            fprintf(stderr, "FAIL: pt=%d expected %d got %d\nSDP:\n%s\n",       \
                    (pt), (want), _g, (sdp));                                   \
            assert(_g == (want));                                               \
        }                                                                       \
    } while (0)

#define CHECK_PARAM(field, sdp, pt, want)                                       \
    do {                                                                        \
        RtcmaOpusParams _p;                                                     \
        rtcma_opus_params_from_sdp((sdp), (pt), &_p);                           \
        if (_p.field != (want)) {                                               \
            fprintf(stderr, "FAIL: pt=%d field=%s expected %d got %d\nSDP:\n%s\n", \
                    (pt), #field, (want), _p.field, (sdp));                     \
            assert(_p.field == (want));                                         \
        }                                                                       \
    } while (0)

int main(void)
{
    /* No fmtp -> mono. */
    CHECK_CH("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
             "a=rtpmap:111 opus/48000/2\r\n", 111, 1);

    /* Dino-style fmtp without sprop-stereo -> mono. */
    CHECK_CH("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
             "a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 useinbandfec=1\r\n", 111, 1);

    /* Canonical stereo offer -> 2. */
    CHECK_CH("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
             "a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 stereo=1;sprop-stereo=1;useinbandfec=1\r\n",
             111, 2);

    /* stereo=1 alone is a request to receive stereo, not a declaration
     * to send stereo. Without sprop-stereo -> mono. */
    CHECK_CH("a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 stereo=1;useinbandfec=1\r\n", 111, 1);

    /* sprop-stereo=0 (note: this parser is a substring scan; it matches
     * sprop-stereo=1 only, so =0 means it never finds the tag -> mono). */
    CHECK_CH("a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 stereo=1;sprop-stereo=0;useinbandfec=1\r\n",
             111, 1);

    /* PT mismatch. */
    CHECK_CH("a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 sprop-stereo=1\r\n", 96, 1);

    /* Substring trap: looking for PT 11 must not match a=fmtp:111. */
    CHECK_CH("a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 sprop-stereo=1\r\n", 11, 1);

    /* ...and the other direction: looking for PT 111 must not pick up
     * a=fmtp:11 on a different codec. */
    CHECK_CH("a=rtpmap:11 L16/44100/2\r\n"
             "a=fmtp:11 sprop-stereo=1\r\n"
             "a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 useinbandfec=1\r\n", 111, 1);

    /* LF-only line endings. */
    CHECK_CH("a=rtpmap:111 opus/48000/2\n"
             "a=fmtp:111 sprop-stereo=1;useinbandfec=1\n", 111, 2);

    /* CRLF, sprop-stereo trailing. */
    CHECK_CH("a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 useinbandfec=1;sprop-stereo=1\r\n", 111, 2);

    /* sprop-stereo=1 only counts when it lives on the fmtp:<pt> line. */
    CHECK_CH("a=rtpmap:111 opus/48000/2\r\n"
             "a=fmtp:111 useinbandfec=1\r\n"
             "a=x-note: peer also sent sprop-stereo=1 in a previous offer\r\n",
             111, 1);

    /* Null / pathological inputs. */
    assert(rtcma_opus_channels_from_sdp(NULL, 111) == 1);
    assert(rtcma_opus_channels_from_sdp("", 111) == 1);
    assert(rtcma_opus_channels_from_sdp("a=fmtp:111 sprop-stereo=1\r\n", -1)
           == 1);

    /* PT 0 and PT 127 boundary cases. */
    CHECK_CH("a=rtpmap:0 opus/48000/2\r\n"
             "a=fmtp:0 sprop-stereo=1\r\n", 0, 2);
    CHECK_CH("a=rtpmap:127 opus/48000/2\r\n"
             "a=fmtp:127 sprop-stereo=1\r\n", 127, 2);

    /* -- Expanded params: useinbandfec / usedtx / cbr / maxavgbitrate /
     * maxplaybackrate / minptime. These drive opus_encoder_ctl in
     * rtcma_send_track_attach, so getting them right is what makes
     * tacky inter-op gracefully with non-tacky XMPP clients. */

    /* Defaults when no fmtp line is present. */
    {
        RtcmaOpusParams p;
        rtcma_opus_params_from_sdp("a=rtpmap:111 opus/48000/2\r\n",
                                   111, &p);
        assert(p.channels          == 1);
        assert(p.useinbandfec      == 0);
        assert(p.usedtx            == 0);
        assert(p.cbr               == 0);
        assert(p.maxaveragebitrate == 0);
        assert(p.maxplaybackrate   == 0);
        assert(p.minptime          == 0);
    }

    /* Full gajim-style line. */
    const char *gajim =
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\r\n";
    CHECK_PARAM(useinbandfec,      gajim, 111, 1);
    CHECK_PARAM(usedtx,            gajim, 111, 0);
    CHECK_PARAM(minptime,          gajim, 111, 10);
    CHECK_PARAM(channels,          gajim, 111, 1);

    /* Conversations-style with explicit ratecaps. */
    const char *conv =
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 minptime=20;useinbandfec=1;usedtx=1;"
        "maxplaybackrate=16000;maxaveragebitrate=20000;cbr=1\r\n";
    CHECK_PARAM(useinbandfec,      conv, 111, 1);
    CHECK_PARAM(usedtx,            conv, 111, 1);
    CHECK_PARAM(cbr,               conv, 111, 1);
    CHECK_PARAM(minptime,          conv, 111, 20);
    CHECK_PARAM(maxplaybackrate,   conv, 111, 16000);
    CHECK_PARAM(maxaveragebitrate, conv, 111, 20000);

    /* `stereo=1` MUST NOT light up `sprop-stereo` and vice versa. */
    const char *stereo_only =
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 stereo=1;useinbandfec=1\r\n";
    CHECK_PARAM(channels,          stereo_only, 111, 1);
    CHECK_PARAM(useinbandfec,      stereo_only, 111, 1);

    const char *sprop_only =
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 sprop-stereo=1\r\n";
    CHECK_PARAM(channels,          sprop_only, 111, 2);
    CHECK_PARAM(useinbandfec,      sprop_only, 111, 0);

    /* PT mismatch returns defaults, not whatever happens to be in the
     * SDP. Guards against rtcGetTrackPayloadTypesForCodec yielding one
     * PT while the fmtp line is for another. */
    CHECK_PARAM(useinbandfec,
                "a=fmtp:111 useinbandfec=1\r\n", 96, 0);

    printf("test_sdp_parse: all cases passed\n");
    return 0;
}
