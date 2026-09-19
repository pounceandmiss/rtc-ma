/* rtp_hdr.h - RFC 3550 fixed header parse.
 *
 * VENDORED. Byte-identical copies live at
 *   rtc-ma/src/rtp_hdr.h
 *   rtc-mv/src/rtp_hdr.h
 * along with the equally identical tests/test_rtp_hdr.c. Edit one, copy
 * to the other; the shared test is what catches a divergence, since the
 * two libraries have no build-time relationship.
 *
 * RFC 3550 only. No payload-format knowledge (no VP8 descriptor, no PT
 * filtering) and no size policy - callers layer that on top. Reports
 * offsets rather than pointers so a length mistake cannot escape as a
 * pointer past the end of the packet.
 */
#ifndef RTP_HDR_H
#define RTP_HDR_H

#include <stddef.h>
#include <stdint.h>

#define RTP_HDR_FIXED_LEN 12

typedef struct {
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
    uint8_t  pt;                /* payload type, M bit masked off      */
    int      marker;            /* 1 if the M bit is set               */
    size_t   payload_off;       /* first payload byte, past CSRC+ext   */
    size_t   payload_len;       /* padding already stripped, always >0 */
} RtpHdr;

/* Parse `len` bytes at `pkt`. Returns 0 and fills `out`, or -1 if the
 * packet is truncated, not version 2, or its declared lengths do not
 * fit. Never reads pkt[len] or beyond. On success:
 *   out->payload_len > 0
 *   out->payload_off + out->payload_len <= len
 * `out` is untouched when -1 is returned.
 *
 * Handles CSRC lists and the RFC 8285 header extension (both the
 * one-byte 0xBEDE and two-byte 0x1000 profiles - the length-in-32-bit-
 * words field sits in the same place either way). */
static inline int rtp_hdr_parse(const uint8_t *pkt, size_t len, RtpHdr *out)
{
    if (!pkt || !out || len < RTP_HDR_FIXED_LEN) return -1;
    if ((pkt[0] >> 6) != 2) return -1;

    const int    has_pad = pkt[0] & 0x20;
    const int    has_ext = pkt[0] & 0x10;
    const size_t cc      = pkt[0] & 0x0F;

    /* Every bound below is written as a subtraction from a length we
     * have already checked, so no intermediate can wrap back into
     * range on a hostile length field. */
    size_t hdr = RTP_HDR_FIXED_LEN + cc * 4;
    if (len < hdr) return -1;

    if (has_ext) {
        if (len - hdr < 4) return -1;
        const size_t words = ((size_t)pkt[hdr + 2] << 8) | pkt[hdr + 3];
        if ((len - hdr - 4) / 4 < words) return -1;
        hdr += 4 + words * 4;
    }

    size_t end = len;
    if (has_pad) {
        /* The pad count is the last octet of the packet, and it counts
         * itself. A count of 0 is malformed, and it must not reach back
         * into the header. */
        if (end <= hdr) return -1;
        const size_t pad = pkt[end - 1];
        if (pad == 0 || end - hdr < pad) return -1;
        end -= pad;
    }
    if (end <= hdr) return -1;          /* header-only, nothing to hand on */

    out->seq         = (uint16_t)(((uint16_t)pkt[2] << 8) | pkt[3]);
    out->timestamp   = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16)
                     | ((uint32_t)pkt[6] <<  8) |  (uint32_t)pkt[7];
    out->ssrc        = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16)
                     | ((uint32_t)pkt[10] << 8) |  (uint32_t)pkt[11];
    out->pt          = (uint8_t)(pkt[1] & 0x7F);
    out->marker      = (pkt[1] & 0x80) ? 1 : 0;
    out->payload_off = hdr;
    out->payload_len = end - hdr;
    return 0;
}

#endif /* RTP_HDR_H */
