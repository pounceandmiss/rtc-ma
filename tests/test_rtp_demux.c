/*
 * Pure unit test for rtcma_parse_rtp - no libdatachannel.
 *
 * libdatachannel hands every RTP packet on the negotiated SSRC to the
 * track message callback regardless of payload type. Real-world peers
 * (Conversations on Android, libwebrtc-based browsers) multiplex
 * opus(111) with CN(13), telephone-event(110), RED(63), and the legacy
 * PCMU/PCMA/G722 fallbacks on the same m-line. Without PT filtering
 * those non-opus RTP packets get queued as if they were opus and fail
 * opus_decode with -4 (OPUS_INVALID_PACKET). This test exercises the
 * RFC 3550 sec.5.1 + RFC 8285 sec.4 header walker that the live receive path
 * relies on to make that filtering decision.
 */

#include "rtcma_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Build a minimal RTP header into buf. Returns header length. Caller
 * appends payload bytes after that. */
static int make_rtp_header(uint8_t *buf, int pt, uint16_t seq,
                           int csrc_count, int has_ext, int has_padding)
{
    buf[0] = (uint8_t)((2 << 6)
                       | (has_padding ? (1 << 5) : 0)
                       | (has_ext     ? (1 << 4) : 0)
                       | (csrc_count & 0x0F));
    buf[1]  = (uint8_t)(pt & 0x7F);
    buf[2]  = (uint8_t)(seq >> 8);
    buf[3]  = (uint8_t)(seq & 0xFF);
    /* A fixed timestamp so the parser's extraction of it is checked; the
     * ssrc after it is not read. */
    buf[4]  = 0x11;
    buf[5]  = 0x22;
    buf[6]  = 0x33;
    buf[7]  = 0x44;
    memset(buf + 8, 0, 4);
    int off = 12;
    for (int i = 0; i < csrc_count; ++i) {
        memset(buf + off, 0xCC, 4);
        off += 4;
    }
    return off;
}

/* Append a 4-byte ext-preamble + ext_words*4 zeroed bytes. profile is
 * 0xBEDE (RFC 8285 one-byte form) or 0x1000 (two-byte form); the parser
 * doesn't care which - it only reads the length in 32-bit words. */
static int append_extension(uint8_t *buf, int off, uint16_t profile,
                            int ext_words)
{
    buf[off + 0] = (uint8_t)(profile >> 8);
    buf[off + 1] = (uint8_t)(profile & 0xFF);
    buf[off + 2] = (uint8_t)(ext_words >> 8);
    buf[off + 3] = (uint8_t)(ext_words & 0xFF);
    memset(buf + off + 4, 0, (size_t)ext_words * 4);
    return off + 4 + ext_words * 4;
}

int main(void)
{
    uint8_t buf[256];

    uint16_t       seq;
    uint32_t       ts;
    const uint8_t *payload;
    int            payload_len;

    /* 1. Bare opus packet: V=2, no CSRC, no ext, no padding, PT=111. */
    {
        int off = make_rtp_header(buf, 111, 0x1234, 0, 0, 0);
        const char *opus = "OPUS-PAYLOAD";
        size_t      olen = strlen(opus);
        memcpy(buf + off, opus, olen);
        int total = off + (int)olen;

        RtcmaRtpParse r = rtcma_parse_rtp(buf, total, 111,
                                          &seq, &ts, &payload, &payload_len);
        assert(r == RTCMA_RTP_ACCEPT);
        assert(seq == 0x1234);
        assert(ts == 0x11223344);
        assert(payload_len == (int)olen);
        assert(memcmp(payload, opus, olen) == 0);
    }

    /* 2. PT mismatch - Conversations-style CN(13), DTMF(110), RED(63) all
     *    skip cleanly. This is the load-bearing case for the live fix. */
    for (int wrong_pt = 0; wrong_pt < 128; ++wrong_pt) {
        if (wrong_pt == 111) continue;
        int off = make_rtp_header(buf, wrong_pt, 99, 0, 0, 0);
        buf[off++] = 0xAA;  /* some payload */
        RtcmaRtpParse r = rtcma_parse_rtp(buf, off, 111,
                                          &seq, &ts, &payload, &payload_len);
        assert(r == RTCMA_RTP_SKIP);
    }
    {
        int off = make_rtp_header(buf, 13, 1, 0, 0, 0);
        buf[off++] = 0;
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_SKIP);
    }
    {
        int off = make_rtp_header(buf, 110, 1, 0, 0, 0);
        buf[off++] = 0;
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_SKIP);
    }
    {
        int off = make_rtp_header(buf, 63, 1, 0, 0, 0);
        buf[off++] = 0;
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_SKIP);
    }

    /* 3. expected_pt = -1 disables filtering - for callers that want
     *    to parse without committing to a PT yet. */
    {
        int off = make_rtp_header(buf, 13, 5, 0, 0, 0);
        buf[off++] = 0xAA;
        assert(rtcma_parse_rtp(buf, off, -1, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_ACCEPT);
        assert(seq == 5);
    }

    /* 4. Short packets - anything < 12 bytes is malformed. */
    for (int sz = 0; sz < 12; ++sz) {
        memset(buf, 0x80, (size_t)sz);
        assert(rtcma_parse_rtp(buf, sz, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_MALFORMED);
    }

    /* 5. Wrong RTP version (V=1, V=3) -> malformed. */
    {
        int off = make_rtp_header(buf, 111, 1, 0, 0, 0);
        buf[off++] = 0xAA;
        buf[0] = (1 << 6);
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_MALFORMED);
        buf[0] = (3 << 6);
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_MALFORMED);
    }

    /* 6. CSRC count: header grows by 4 x CC. */
    {
        int off = make_rtp_header(buf, 111, 0xBEEF, /*cc*/ 3, 0, 0);
        const char *opus = "PAYLOAD";
        size_t      olen = strlen(opus);
        memcpy(buf + off, opus, olen);
        int total = off + (int)olen;
        assert(rtcma_parse_rtp(buf, total, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_ACCEPT);
        assert(seq == 0xBEEF);
        assert(payload_len == (int)olen);
        assert(memcmp(payload, opus, olen) == 0);
    }

    /* 7. One-byte (0xBEDE) extension header - common in WebRTC. Skips
     *    over the extension and returns the opus payload after it. */
    {
        int off = make_rtp_header(buf, 111, 1, 0, /*ext*/ 1, 0);
        off = append_extension(buf, off, 0xBEDE, /*words*/ 2);
        const char *opus = "EXT-PL";
        size_t      olen = strlen(opus);
        memcpy(buf + off, opus, olen);
        int total = off + (int)olen;
        assert(rtcma_parse_rtp(buf, total, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_ACCEPT);
        assert(payload_len == (int)olen);
        assert(memcmp(payload, opus, olen) == 0);
    }

    /* 8. Two-byte (0x1000) extension header - what extmap-allow-mixed
     *    senders emit when an extension id exceeds 14. The parser must
     *    treat the length-in-words field identically. */
    {
        int off = make_rtp_header(buf, 111, 1, 0, /*ext*/ 1, 0);
        off = append_extension(buf, off, 0x1000, /*words*/ 3);
        const char *opus = "TWO-BYTE";
        size_t      olen = strlen(opus);
        memcpy(buf + off, opus, olen);
        int total = off + (int)olen;
        assert(rtcma_parse_rtp(buf, total, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_ACCEPT);
        assert(payload_len == (int)olen);
        assert(memcmp(payload, opus, olen) == 0);
    }

    /* 9. Extension flag set but packet truncated before the 4-byte
     *    extension preamble -> malformed (must not read off the end). */
    {
        int off = make_rtp_header(buf, 111, 1, 0, /*ext*/ 1, 0);
        assert(off == 12);
        /* No extension preamble bytes. */
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_MALFORMED);
        /* Even 1-3 bytes of preamble is still short. */
        buf[off + 0] = 0xBE;
        buf[off + 1] = 0xDE;
        buf[off + 2] = 0x00;
        assert(rtcma_parse_rtp(buf, off + 3, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_MALFORMED);
    }

    /* 10. Extension length says N words but packet doesn't contain them. */
    {
        int off = make_rtp_header(buf, 111, 1, 0, /*ext*/ 1, 0);
        buf[off + 0] = 0xBE;
        buf[off + 1] = 0xDE;
        buf[off + 2] = 0x00;
        buf[off + 3] = 0x05;     /* claims 5 words = 20 bytes */
        /* Only provide 8 bytes of extension data. */
        memset(buf + off + 4, 0, 8);
        int total = off + 4 + 8;
        assert(rtcma_parse_rtp(buf, total, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_MALFORMED);
    }

    /* 11. Padding: P-bit set, last byte = pad count, payload shrinks. */
    {
        int off = make_rtp_header(buf, 111, 1, 0, 0, /*padding*/ 1);
        const char *opus = "PAYLOAD";
        size_t      olen = strlen(opus);
        memcpy(buf + off, opus, olen);
        int payload_off = off;
        off += (int)olen;
        /* 3 bytes of padding, the last is the count. */
        buf[off + 0] = 0;
        buf[off + 1] = 0;
        buf[off + 2] = 3;
        int total = off + 3;
        assert(rtcma_parse_rtp(buf, total, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_ACCEPT);
        assert(payload     == buf + payload_off);
        assert(payload_len == (int)olen);
    }

    /* 12. Padding count > payload region -> malformed. */
    {
        int off = make_rtp_header(buf, 111, 1, 0, 0, /*padding*/ 1);
        /* One byte of "payload" plus padding count of 99 - bogus. */
        buf[off++] = 0xAA;
        buf[off++] = 99;
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_MALFORMED);
    }

    /* 13. Empty payload (header + ext consume everything) -> malformed. */
    {
        int off = make_rtp_header(buf, 111, 1, 0, 0, 0);
        /* No payload. */
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_MALFORMED);
    }

    /* 14. NULL msg / negative size -> malformed (defensive). */
    assert(rtcma_parse_rtp(NULL, 100, 111, &seq, &ts, &payload, &payload_len)
           == RTCMA_RTP_MALFORMED);
    assert(rtcma_parse_rtp(buf, -1, 111, &seq, &ts, &payload, &payload_len)
           == RTCMA_RTP_MALFORMED);

    /* 15. ACCEPT path leaves out_* untouched on SKIP / MALFORMED. */
    {
        int off = make_rtp_header(buf, 13, 0xDEAD, 0, 0, 0);
        buf[off++] = 0xAA;
        seq         = 0xFEFE;
        payload     = (const uint8_t *)0xDEADBEEF;
        payload_len = -1;
        assert(rtcma_parse_rtp(buf, off, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_SKIP);
        assert(seq         == 0xFEFE);
        assert(payload     == (const uint8_t *)0xDEADBEEF);
        assert(payload_len == -1);
    }

    /* 16. CSRC + extension + padding all set: combined header walk. */
    {
        int off = make_rtp_header(buf, 111, 0xCAFE, /*cc*/ 2,
                                  /*ext*/ 1, /*padding*/ 1);
        off = append_extension(buf, off, 0xBEDE, /*words*/ 1);
        const char *opus = "MIXED!";
        size_t      olen = strlen(opus);
        memcpy(buf + off, opus, olen);
        int payload_off = off;
        off += (int)olen;
        buf[off + 0] = 0;
        buf[off + 1] = 2;       /* 2 bytes padding total, last is count */
        int total = off + 2;
        assert(rtcma_parse_rtp(buf, total, 111, &seq, &ts, &payload, &payload_len)
               == RTCMA_RTP_ACCEPT);
        assert(seq         == 0xCAFE);
        assert(payload     == buf + payload_off);
        assert(payload_len == (int)olen);
        assert(memcmp(payload, opus, olen) == 0);
    }

    printf("test_rtp_demux: all cases passed\n");
    return 0;
}
