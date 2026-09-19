/* Coverage-guided fuzz target for the inbound RTP path:
 *   rtp_hdr_parse -> rtcma_parse_rtp
 *
 * Built two ways from this one file:
 *
 *   clang, -DRTCMA_FUZZ=ON   -> `fuzz_rtp`, a real libFuzzer binary
 *                               (-fsanitize=fuzzer,address,undefined).
 *   anything else            -> `test_rtp_fuzz`, an ordinary ctest that
 *                               drives the same entry point with a
 *                               deterministic generator, or replays the
 *                               files named on its command line.
 *
 * Seed a libFuzzer corpus from the second mode with
 * `./test_rtp_fuzz --emit-corpus <dir>`.
 *
 * The sibling of rtc-mv/tests/test_rtp_fuzz.c: same shape, same input
 * format, but over the PT-filtering wrapper rather than the VP8
 * descriptor. Both bottom out in the vendored rtp_hdr.h.
 *
 * Input format: a sequence of packets, each prefixed with a two-byte
 * big-endian length. Trailing bytes past the last complete record are
 * fed as a final packet.
 */

#include "rtcma_internal.h"
#include "rtp_hdr.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Outcome tallies, so the standalone driver can fail loudly if the
 * generator ever stops reaching one of the three paths. */
static unsigned long g_accept, g_skip, g_malformed;

static void one_packet(const uint8_t *data, size_t size)
{
    /* Exact-sized allocation: a read one byte past the packet is an
     * ASan report, not a silent pass into adjacent bytes. */
    uint8_t *pkt = malloc(size ? size : 1);
    if (!pkt) return;
    memcpy(pkt, data, size);

    RtpHdr h;
    const int bare = rtp_hdr_parse(pkt, size, &h);
    if (bare == 0) {
        assert(h.payload_len > 0);
        assert(h.payload_off <= size);
        assert(h.payload_len <= size - h.payload_off);
    }

    /* Three expectations per packet: accept-everything, the PT the
     * packet actually carries (forces ACCEPT), and a fixed PT the
     * packet usually does not carry (forces SKIP). */
    for (int mode = 0; mode < 3; mode++) {
        int want;
        switch (mode) {
        case 0:  want = -1; break;
        case 1:  want = (bare == 0) ? (int)h.pt : 111; break;
        default: want = 111; break;
        }

        uint16_t       seq     = 0xAAAA;
        uint32_t       ts      = 0xAAAAAAAAu;
        const uint8_t *payload = (const uint8_t *)(uintptr_t)0xAAAA;
        int            plen    = -12345;

        const RtcmaRtpParse rc =
            rtcma_parse_rtp(pkt, (int)size, want, &seq, &ts, &payload, &plen);

        if (rc == RTCMA_RTP_MALFORMED) {
            g_malformed++;
            assert(bare != 0);
            /* Outputs untouched, as rtcma_internal.h promises. */
            assert(seq == 0xAAAA && ts == 0xAAAAAAAAu);
            assert(payload == (const uint8_t *)(uintptr_t)0xAAAA);
            assert(plen == -12345);
            continue;
        }

        assert(bare == 0);
        if (rc == RTCMA_RTP_SKIP) {
            g_skip++;
            assert(want >= 0 && (int)h.pt != want);
            assert(seq == 0xAAAA && ts == 0xAAAAAAAAu);
            assert(payload == (const uint8_t *)(uintptr_t)0xAAAA);
            assert(plen == -12345);
            continue;
        }

        g_accept++;
        assert(rc == RTCMA_RTP_ACCEPT);
        assert(want < 0 || (int)h.pt == want);
        assert(seq == h.seq && ts == h.timestamp);
        assert(plen > 0);
        assert(payload >= pkt && payload <= pkt + size);
        assert((size_t)plen <= (size_t)((pkt + size) - payload));
        assert(payload == pkt + h.payload_off && (size_t)plen == h.payload_len);

        volatile uint8_t sink = 0;
        for (int k = 0; k < plen; k++) sink ^= payload[k];
        (void)sink;
    }
    free(pkt);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t off = 0;
    while (size - off >= 2) {
        const size_t n = ((size_t)data[off] << 8) | data[off + 1];
        off += 2;
        if (n > size - off) break;
        one_packet(data + off, n);
        off += n;
    }
    if (off < size) one_packet(data + off, size - off);
    return 0;
}

#ifndef RTCMA_LIBFUZZER

/* -- standalone driver ------------------------------------------------ */

static uint64_t g_rng = 0x123456789abcdef0ULL;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}
static uint32_t rnd_max(uint32_t n) { return n ? rnd() % n : 0; }

/* One plausible-ish RTP packet, or pure noise. Returns bytes written. */
static size_t gen_packet(uint8_t *pkt, size_t cap, int noise)
{
    if (noise) {
        const size_t len = rnd_max(64);
        for (size_t j = 0; j < len; j++) pkt[j] = (uint8_t)rnd();
        return len;
    }

    const int cc = (int)rnd_max(16);
    const int x  = (int)rnd_max(2);
    const int p  = (int)rnd_max(2);
    const size_t hdr = 12 + (size_t)cc * 4;

    size_t len = hdr + (x ? 4 + (size_t)rnd_max(4) * 4 : 0) + rnd_max(300);
    if (len > cap) len = cap;

    for (size_t j = 0; j < len; j++) pkt[j] = (uint8_t)rnd();
    pkt[0] = (uint8_t)(0x80 | (p << 5) | (x << 4) | cc);
    /* Bias toward the negotiated PT so the ACCEPT path gets exercised. */
    pkt[1] = rnd_max(2) ? (uint8_t)(rnd_max(2) ? 111 : 0xEF) : (uint8_t)rnd();
    return len;
}

static size_t gen_input(uint8_t *buf, size_t cap, int i)
{
    const int packets = 1 + (int)rnd_max(4);
    size_t off = 0;
    for (int k = 0; k < packets && cap - off > 1050; k++) {
        const size_t n = gen_packet(buf + off + 2, 1024, (i + k) % 3 == 0);
        buf[off]     = (uint8_t)(n >> 8);
        buf[off + 1] = (uint8_t)(n & 0xFF);
        off += 2 + n;
    }
    return off;
}

static int replay_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    static uint8_t buf[1 << 16];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    LLVMFuzzerTestOneInput(buf, n);
    return 0;
}

static int emit_corpus(const char *dir)
{
    enum { SEEDS = 64 };
    static uint8_t buf[1 << 16];
    char path[1024];

    for (int i = 0; i < SEEDS; i++) {
        const size_t n = gen_input(buf, sizeof(buf), i);
        if (snprintf(path, sizeof(path), "%s/seed%03d.bin", dir, i)
                >= (int)sizeof(path))
            return -1;
        FILE *f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", path); return -1; }
        fwrite(buf, 1, n, f);
        fclose(f);
    }
    printf("wrote %d seeds to %s\n", SEEDS, dir);
    return 0;
}

int main(int argc, char **argv)
{
    rtcmaInitLogger(RTCMA_LOG_NONE, NULL);

    if (argc == 3 && strcmp(argv[1], "--emit-corpus") == 0)
        return emit_corpus(argv[2]) == 0 ? 0 : 1;

    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            if (replay_file(argv[i]) != 0) return 1;
        printf("test_rtp_fuzz: replayed %d input(s), OK\n", argc - 1);
        return 0;
    }

    enum { N = 120000 };
    static uint8_t buf[1 << 16];
    for (int i = 0; i < N; i++) {
        const size_t n = gen_input(buf, sizeof(buf), i);
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("test_rtp_fuzz: %d inputs, accept=%lu skip=%lu malformed=%lu, "
           "no escaping payloads\n", N, g_accept, g_skip, g_malformed);
    assert(g_accept > 0 && g_skip > 0 && g_malformed > 0);
    printf("test_rtp_fuzz: OK\n");
    return 0;
}

#endif /* !RTCMA_LIBFUZZER */
