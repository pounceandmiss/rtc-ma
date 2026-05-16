/*
 * Unit test for the ma_device_id <-> "backend:payload" string codec.
 *
 * We treat the union as an opaque byte buffer (size from
 * rtcma_internal_id_buffer_size). The test never inspects the union
 * layout, just round-trips through both directions of the codec and
 * checks that parse failures fire on the expected inputs.
 *
 * The "memset 0xAB" before id_from_string is deliberate: a bug where
 * the parser leaves union padding uninitialised would let the leftover
 * bytes leak into id_to_string's output. Pre-fill + round-trip equality
 * catches that.
 */

#include "rtcma_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void roundtrip_ok(const char *s)
{
    const char *colon = strchr(s, ':');
    if (!colon) { fprintf(stderr, "test bug: no colon in %s\n", s); abort(); }

    char tag[32];
    size_t tlen = (size_t)(colon - s);
    if (tlen >= sizeof(tag)) { fprintf(stderr, "test bug: tag too long\n"); abort(); }
    memcpy(tag, s, tlen);
    tag[tlen] = '\0';

    int b = rtcma_internal_backend_from_tag(tag);
    if (b < 0) {
        fprintf(stderr, "FAIL: unknown backend tag '%s' for input '%s'\n", tag, s);
        g_failures++; return;
    }

    size_t bufsz = rtcma_internal_id_buffer_size();
    void *buf = malloc(bufsz);
    assert(buf);
    memset(buf, 0xAB, bufsz);  /* poison: parser must zero union padding */

    if (rtcma_internal_id_from_string(s, b, buf) != 0) {
        fprintf(stderr, "FAIL: id_from_string('%s', %s) rejected valid input\n", s, tag);
        g_failures++; free(buf); return;
    }

    char *out = rtcma_internal_id_to_string(b, buf);
    if (!out) {
        fprintf(stderr, "FAIL: id_to_string returned NULL for '%s'\n", s);
        g_failures++; free(buf); return;
    }
    if (strcmp(out, s) != 0) {
        fprintf(stderr, "FAIL: round-trip mismatch\n  in:  '%s'\n  out: '%s'\n",
                s, out);
        g_failures++;
    }
    free(out);
    free(buf);
}

static void parse_must_fail(const char *s, const char *tag)
{
    int b = rtcma_internal_backend_from_tag(tag);
    if (b < 0) { fprintf(stderr, "test bug: bad tag '%s'\n", tag); abort(); }

    size_t bufsz = rtcma_internal_id_buffer_size();
    void *buf = calloc(1, bufsz);
    assert(buf);
    int rc = rtcma_internal_id_from_string(s, b, buf);
    if (rc != -1) {
        fprintf(stderr, "FAIL: expected reject of '%s' under backend '%s' (got %d)\n",
                s, tag, rc);
        g_failures++;
    }
    free(buf);
}

int main(void)
{
    /* Happy-path round trips for the backends that matter on Linux/
     * macOS/Windows. Inputs are in canonical form (lowercase hex,
     * decimal without leading zeros / sign-only) - id_to_string emits
     * canonical output, so non-canonical inputs would not round-trip
     * and that's by design. */
    roundtrip_ok("pulse:alsa_output.pci-0000_00_1f.3.analog-stereo");
    roundtrip_ok("pulse:");                                  /* empty pulse name */
    roundtrip_ok("alsa:hw:CARD=PCH,DEV=0");                  /* colon in payload */
    roundtrip_ok("alsa:default");
    roundtrip_ok("coreaudio:BuiltInSpeakerDevice");
    roundtrip_ok("oss:/dev/dsp0");
    roundtrip_ok("dsound:00112233445566778899aabbccddeeff");
    roundtrip_ok("dsound:ffffffffffffffffffffffffffffffff");
    roundtrip_ok("winmm:0");
    roundtrip_ok("winmm:4294967295");
    roundtrip_ok("aaudio:0");
    roundtrip_ok("aaudio:-1");
    roundtrip_ok("aaudio:42");
    roundtrip_ok("opensl:7");
    roundtrip_ok("jack:0");
    roundtrip_ok("null:0");

    /* Cross-backend tag must be rejected - restoring a persisted pulse
     * id on a coreaudio machine should fail loudly, not silently hand
     * miniaudio the wrong union field. */
    parse_must_fail("alsa:default",                              "pulse");
    parse_must_fail("pulse:thing",                               "alsa");
    parse_must_fail("dsound:00112233445566778899aabbccddeeff",   "alsa");
    parse_must_fail("winmm:1",                                   "pulse");

    /* Malformed format. */
    parse_must_fail("",                                          "pulse");
    parse_must_fail("nocolon",                                   "pulse");
    parse_must_fail("pulse",                                     "pulse");
    parse_must_fail("dsound:short",                              "dsound");
    parse_must_fail("dsound:zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",   "dsound");
    parse_must_fail("dsound:00112233445566778899aabbccddeeff00", "dsound");  /* 34 chars */
    parse_must_fail("winmm:notanumber",                          "winmm");
    parse_must_fail("winmm:12abc",                               "winmm");
    parse_must_fail("aaudio:",                                   "aaudio");

    /* Unknown backend tag returns -1, doesn't crash. */
    if (rtcma_internal_backend_from_tag("doesnotexist") != -1) {
        fprintf(stderr, "FAIL: backend_from_tag accepted bogus tag\n");
        g_failures++;
    }
    if (rtcma_internal_backend_from_tag(NULL) != -1) {
        fprintf(stderr, "FAIL: backend_from_tag(NULL) didn't return -1\n");
        g_failures++;
    }

    if (g_failures) {
        fprintf(stderr, "device-id serialize test: %d failure(s).\n", g_failures);
        return 1;
    }
    printf("device-id serialize test ok.\n");
    return 0;
}
