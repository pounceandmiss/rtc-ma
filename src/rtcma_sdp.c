#include "rtcma_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* -- fmtp parameter scanner ------------------------------------------
 * RFC 7587 sec.6 lists opus' fmtp parameters as `key=value` pairs joined
 * by ';' on a single `a=fmtp:<pt> ...` line. The probe locates that line
 * by PT and parses individual keys without depending on libdatachannel
 * - same constraint as the original channels-only probe. */

/* Walk a buffer of fmtp args looking for `key=` at a position preceded
 * by ';', ' ', or buffer start. Required to keep `stereo=` from
 * matching inside `sprop-stereo=` and the substring guarantees match
 * what test_sdp_parse expects. Returns true and writes the trailing
 * non-negative integer to *out on the first hit. */
static bool fmtp_get_int(const char *buf, size_t len, const char *key,
                         int *out)
{
    size_t klen = strlen(key);
    if (len < klen + 2) return false;
    for (size_t i = 0; i + klen + 1 <= len; i++) {
        if (memcmp(buf + i, key, klen) != 0) continue;
        if (i > 0) {
            char prev = buf[i - 1];
            if (prev != ';' && prev != ' ') continue;
        }
        if (buf[i + klen] != '=') continue;
        const char *p   = buf + i + klen + 1;
        const char *end = buf + len;
        int  v   = 0;
        bool any = false;
        while (p < end && *p >= '0' && *p <= '9') {
            v   = v * 10 + (*p - '0');
            p++;
            any = true;
        }
        if (any) { *out = v; return true; }
    }
    return false;
}

void rtcma_opus_params_from_sdp(const char *sdp, int pt,
                                RtcmaOpusParams *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->channels = 1;          /* RFC 7587 sec.7 default */

    if (!sdp || pt < 0) return;

    char needle[24];
    int nlen = snprintf(needle, sizeof(needle), "a=fmtp:%d ", pt);
    if (nlen <= 0 || nlen >= (int)sizeof(needle)) return;

    const char *fmtp = strstr(sdp, needle);
    if (!fmtp) return;
    const char *eol = strpbrk(fmtp, "\r\n");
    size_t      line_len = eol ? (size_t)(eol - fmtp) : strlen(fmtp);
    if (line_len < (size_t)nlen) return;

    const char *args     = fmtp + nlen;
    size_t      args_len = line_len - (size_t)nlen;

    int v;
    /* sprop-stereo=1 is what we use to size the decoder - the only
     * fmtp param that actually decides packet framing. stereo=1 alone
     * is just a request and doesn't change the wire format. */
    if (fmtp_get_int(args, args_len, "sprop-stereo", &v) && v == 1)
        out->channels = 2;

    if (fmtp_get_int(args, args_len, "useinbandfec", &v))
        out->useinbandfec = (v != 0);
    if (fmtp_get_int(args, args_len, "usedtx", &v))
        out->usedtx = (v != 0);
    if (fmtp_get_int(args, args_len, "cbr", &v))
        out->cbr = (v != 0);
    if (fmtp_get_int(args, args_len, "maxplaybackrate", &v))
        out->maxplaybackrate = v;
    if (fmtp_get_int(args, args_len, "maxaveragebitrate", &v))
        out->maxaveragebitrate = v;
    if (fmtp_get_int(args, args_len, "minptime", &v))
        out->minptime = v;
}

int rtcma_opus_channels_from_sdp(const char *sdp, int pt)
{
    RtcmaOpusParams p;
    rtcma_opus_params_from_sdp(sdp, pt, &p);
    return p.channels;
}
