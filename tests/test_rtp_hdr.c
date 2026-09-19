/* Unit test for the vendored rtp_hdr.h.
 *
 * VENDORED. Byte-identical copies live at
 *   rtc-ma/tests/test_rtp_hdr.c
 *   rtc-mv/tests/test_rtp_hdr.c
 * This file is the enforcement for the two copies of rtp_hdr.h: it
 * depends on nothing else in either library, so a behavioural drift in
 * one repo's header fails the other repo's suite too.
 *
 * Every packet is parsed out of an exactly-sized heap allocation, so a
 * read one byte past the end is an ASan report rather than a silent
 * pass on whatever followed in a static buffer.
 */

#include "rtp_hdr.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse `len` bytes of `src` from a heap block sized exactly `len`. */
static int parse_exact(const uint8_t *src, size_t len, RtpHdr *out)
{
    uint8_t *p = malloc(len ? len : 1);
    assert(p);
    memcpy(p, src, len);
    const int rc = rtp_hdr_parse(p, len, out);
    if (rc == 0) {
        assert(out->payload_len > 0);
        assert(out->payload_off <= len);
        assert(out->payload_len <= len - out->payload_off);
    }
    free(p);
    return rc;
}

/* Fixed header into buf. Returns the byte count written (12 + CSRCs). */
static size_t hdr12(uint8_t *buf, int pt, int marker, int cc, int ext, int pad)
{
    buf[0] = (uint8_t)(0x80 | (pad ? 0x20 : 0) | (ext ? 0x10 : 0) | (cc & 0x0F));
    buf[1] = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7F));
    buf[2] = 0x12; buf[3] = 0x34;                       /* seq  0x1234 */
    buf[4] = 0x11; buf[5] = 0x22; buf[6] = 0x33; buf[7] = 0x44;
    buf[8] = 0xDE; buf[9] = 0xAD; buf[10] = 0xBE; buf[11] = 0xEF;
    for (int i = 0; i < cc; i++) memset(buf + 12 + 4 * i, 0xC5, 4);
    return (size_t)(12 + 4 * cc);
}

static void t_fields(void)
{
    uint8_t buf[64];
    size_t  off = hdr12(buf, 111, /*marker*/ 1, 0, 0, 0);
    memcpy(buf + off, "PAYLOAD", 7);

    RtpHdr h;
    assert(parse_exact(buf, off + 7, &h) == 0);
    assert(h.seq == 0x1234);
    assert(h.timestamp == 0x11223344u);
    assert(h.ssrc == 0xDEADBEEFu);
    assert(h.pt == 111);
    assert(h.marker == 1);
    assert(h.payload_off == 12);
    assert(h.payload_len == 7);

    /* M bit must not leak into the payload type. */
    off = hdr12(buf, 96, /*marker*/ 0, 0, 0, 0);
    buf[off] = 0xAA;
    assert(parse_exact(buf, off + 1, &h) == 0);
    assert(h.pt == 96 && h.marker == 0);
}

static void t_version_and_truncation(void)
{
    uint8_t buf[64];
    RtpHdr  h;

    const size_t off = hdr12(buf, 111, 0, 0, 0, 0);
    buf[off] = 0xAA;

    for (size_t n = 0; n < 12; n++)                 /* short of a header */
        assert(parse_exact(buf, n, &h) == -1);
    assert(parse_exact(buf, 12, &h) == -1);         /* header, no payload */
    assert(parse_exact(buf, 13, &h) == 0);

    for (int v = 0; v < 4; v++) {
        if (v == 2) continue;
        buf[0] = (uint8_t)((v << 6) | (buf[0] & 0x3F));
        assert(parse_exact(buf, 13, &h) == -1);
    }
}

static void t_csrc(void)
{
    uint8_t buf[128];
    RtpHdr  h;

    for (int cc = 0; cc <= 15; cc++) {
        const size_t off = hdr12(buf, 111, 0, cc, 0, 0);
        buf[off] = 0xAA;
        assert(parse_exact(buf, off + 1, &h) == 0);
        assert(h.payload_off == off);
        assert(h.payload_len == 1);
        /* One byte short of the CSRC list the header claims. */
        assert(parse_exact(buf, off - 1, &h) == -1);
    }
}

static void t_extension(void)
{
    uint8_t buf[128];
    RtpHdr  h;

    size_t off = hdr12(buf, 111, 0, 0, /*ext*/ 1, 0);
    buf[off + 0] = 0xBE; buf[off + 1] = 0xDE;       /* one-byte profile */
    buf[off + 2] = 0x00; buf[off + 3] = 0x02;       /* 2 words          */
    memset(buf + off + 4, 0x5A, 8);
    buf[off + 12] = 0xAA;
    assert(parse_exact(buf, off + 13, &h) == 0);
    assert(h.payload_off == off + 12);
    assert(h.payload_len == 1);

    /* Truncated before and inside the 4-byte extension preamble. */
    for (size_t n = 0; n < 4; n++)
        assert(parse_exact(buf, off + n, &h) == -1);

    /* Declared word count runs past the packet. */
    buf[off + 3] = 0x05;                            /* claims 20 bytes  */
    assert(parse_exact(buf, off + 4 + 8, &h) == -1);

    /* A word count that would overflow a naive hdr + 4 + words * 4. */
    buf[off + 2] = 0xFF; buf[off + 3] = 0xFF;
    assert(parse_exact(buf, off + 13, &h) == -1);

    /* Extension exactly fills the packet: no payload left. */
    buf[off + 2] = 0x00; buf[off + 3] = 0x02;
    assert(parse_exact(buf, off + 12, &h) == -1);

    /* Two-byte (0x1000) profile parses the same way. */
    off = hdr12(buf, 111, 0, 0, /*ext*/ 1, 0);
    buf[off + 0] = 0x10; buf[off + 1] = 0x00;
    buf[off + 2] = 0x00; buf[off + 3] = 0x01;
    memset(buf + off + 4, 0, 4);
    buf[off + 8] = 0xAA;
    assert(parse_exact(buf, off + 9, &h) == 0);
    assert(h.payload_len == 1);
}

static void t_padding(void)
{
    uint8_t buf[64];
    RtpHdr  h;

    /* 7 payload bytes + 3 padding, last octet is the count. */
    size_t off = hdr12(buf, 111, 0, 0, 0, /*pad*/ 1);
    memcpy(buf + off, "PAYLOAD", 7);
    buf[off + 7] = 0; buf[off + 8] = 0; buf[off + 9] = 3;
    assert(parse_exact(buf, off + 10, &h) == 0);
    assert(h.payload_off == off);
    assert(h.payload_len == 7);

    /* Count larger than the payload region. */
    off = hdr12(buf, 111, 0, 0, 0, /*pad*/ 1);
    buf[off] = 0xAA; buf[off + 1] = 99;
    assert(parse_exact(buf, off + 2, &h) == -1);

    /* A count of 0 is malformed: the octet counts itself, so the
     * minimum legal value is 1. Accepting it folds the count byte into
     * the payload. */
    off = hdr12(buf, 111, 0, 0, 0, /*pad*/ 1);
    buf[off] = 0xAA; buf[off + 1] = 0;
    assert(parse_exact(buf, off + 2, &h) == -1);

    /* Padding that consumes the whole payload leaves nothing. */
    off = hdr12(buf, 111, 0, 0, 0, /*pad*/ 1);
    buf[off] = 2; buf[off + 1] = 2;
    assert(parse_exact(buf, off + 2, &h) == -1);

    /* P set on a header-only packet: the pad count must not be read
     * out of the fixed header. */
    off = hdr12(buf, 111, 0, 0, 0, /*pad*/ 1);
    assert(parse_exact(buf, off, &h) == -1);

    /* Same, with the count byte sitting inside a CSRC list. */
    off = hdr12(buf, 111, 0, /*cc*/ 2, 0, /*pad*/ 1);
    assert(parse_exact(buf, off, &h) == -1);
    assert(parse_exact(buf, 12, &h) == -1);
}

static void t_exhaustive_short(void)
{
    /* Every 4-bit pattern of flags against every length up to 24: the
     * parser must either reject or report a payload fully inside the
     * packet, and must never read past `len` (ASan enforces that via
     * the exact-sized allocation). */
    uint8_t buf[32];
    RtpHdr  h;
    int accepted = 0, rejected = 0;

    for (unsigned b0 = 0; b0 < 256; b0++) {
        for (unsigned b1 = 0; b1 < 256; b1 += 17) {
            for (size_t len = 0; len <= 24; len++) {
                for (size_t i = 0; i < sizeof(buf); i++)
                    buf[i] = (uint8_t)(i * 31 + b1);
                buf[0] = (uint8_t)b0;
                buf[1] = (uint8_t)b1;
                if (parse_exact(buf, len, &h) == 0) accepted++;
                else rejected++;
            }
        }
    }
    printf("test_rtp_hdr: %d accepted / %d rejected over short-packet sweep\n",
           accepted, rejected);
    assert(accepted > 0 && rejected > 0);
}

int main(void)
{
    t_fields();
    t_version_and_truncation();
    t_csrc();
    t_extension();
    t_padding();
    t_exhaustive_short();
    printf("test_rtp_hdr: OK\n");
    return 0;
}
