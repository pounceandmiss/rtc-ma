/* rtcma_device_id.c - ma_device_id <-> "backend:payload" string codec.
 *
 * miniaudio's ma_device_id is a union: each backend uses a different
 * field shape (C string, GUID bytes, integer, UTF-16 string). At
 * runtime exactly one field is meaningful - the one for the backend
 * ma_context_init resolved. The other bytes of the union are
 * uninitialised stack/heap padding, so memcpy'ing the whole struct
 * around is unsafe: bytewise equality breaks across enumerations, and
 * the padding tail can't survive any transport that isn't fully
 * binary-safe (lenpipe with utf-8 conversion, JSON, SQLite TEXT, ...).
 *
 * Solution: surface the *live* field as a "backend:payload" ASCII
 * string and rebuild the union from that string on the way in. The
 * tag prefix lets us refuse cross-backend ids loudly instead of
 * silently handing miniaudio the wrong union member. */

#include "rtcma_device_id.h"
#include "rtcma_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- Active-backend cache ---------------------------------------------
 *
 * miniaudio resolves the backend at the first ma_context_init based on
 * the runtime's enabled-backend priority list; it never changes for the
 * process lifetime. Cache it once and reuse. rtcma_enumerate_devices
 * fills the cache via rtcma_devid_set_active_backend so the codec
 * doesn't pay for a second ma_context_init when a caller then passes a
 * device id into Player/Capturer init. */

static ma_backend      g_backend;
static int             g_backend_known = 0;
static pthread_mutex_t g_backend_lock  = PTHREAD_MUTEX_INITIALIZER;

int rtcma_devid_resolve_active_backend(ma_backend *out)
{
    pthread_mutex_lock(&g_backend_lock);
    if (!g_backend_known) {
        ma_context ctx;
        if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) {
            pthread_mutex_unlock(&g_backend_lock);
            return -1;
        }
        g_backend       = ctx.backend;
        g_backend_known = 1;
        ma_context_uninit(&ctx);
    }
    *out = g_backend;
    pthread_mutex_unlock(&g_backend_lock);
    return 0;
}

void rtcma_devid_set_active_backend(ma_backend b)
{
    pthread_mutex_lock(&g_backend_lock);
    g_backend       = b;
    g_backend_known = 1;
    pthread_mutex_unlock(&g_backend_lock);
}

/* -- Backend-tag table ----------------------------------------------- */

static const char *backend_tag(ma_backend b)
{
    switch (b) {
    case ma_backend_wasapi:     return "wasapi";
    case ma_backend_dsound:     return "dsound";
    case ma_backend_winmm:      return "winmm";
    case ma_backend_coreaudio:  return "coreaudio";
    case ma_backend_sndio:      return "sndio";
    case ma_backend_audio4:     return "audio4";
    case ma_backend_oss:        return "oss";
    case ma_backend_pulseaudio: return "pulse";
    case ma_backend_alsa:       return "alsa";
    case ma_backend_jack:       return "jack";
    case ma_backend_aaudio:     return "aaudio";
    case ma_backend_opensl:     return "opensl";
    case ma_backend_webaudio:   return "webaudio";
    case ma_backend_null:       return "null";
    case ma_backend_custom:     return "custom";
    default:                    return NULL;
    }
}

/* -- Serialise ------------------------------------------------------- */

char *rtcma_devid_to_string(ma_backend b, const ma_device_id *id)
{
    const char *tag = backend_tag(b);
    if (!tag || !id) return NULL;

    char buf[512];
    int n = -1;
    switch (b) {
    case ma_backend_alsa:
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, id->alsa); break;
    case ma_backend_pulseaudio:
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, id->pulse); break;
    case ma_backend_coreaudio:
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, id->coreaudio); break;
    case ma_backend_sndio:
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, id->sndio); break;
    case ma_backend_audio4:
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, id->audio4); break;
    case ma_backend_oss:
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, id->oss); break;
    case ma_backend_webaudio:
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, id->webaudio); break;
    case ma_backend_dsound: {
        char hex[33];
        for (int i = 0; i < 16; i++) sprintf(hex + 2*i, "%02x", id->dsound[i]);
        hex[32] = '\0';
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, hex);
        break;
    }
    case ma_backend_winmm:
        n = snprintf(buf, sizeof(buf), "%s:%u", tag, (unsigned)id->winmm); break;
    case ma_backend_aaudio:
        n = snprintf(buf, sizeof(buf), "%s:%d", tag, (int)id->aaudio); break;
    case ma_backend_opensl:
        n = snprintf(buf, sizeof(buf), "%s:%u", tag, (unsigned)id->opensl); break;
    case ma_backend_jack:
        n = snprintf(buf, sizeof(buf), "%s:%d", tag, id->jack); break;
    case ma_backend_null:
        n = snprintf(buf, sizeof(buf), "%s:%d", tag, id->nullbackend); break;
#ifdef MA_HAS_WASAPI
    case ma_backend_wasapi: {
        /* ma_wchar_win32 is wchar_t on Windows (UTF-16); convert via
         * the Win32 API so we always emit UTF-8 regardless of the C
         * locale. */
        char utf8[256];
        int len = WideCharToMultiByte(CP_UTF8, 0, id->wasapi, -1,
                                      utf8, sizeof(utf8), NULL, NULL);
        if (len <= 0) return NULL;
        n = snprintf(buf, sizeof(buf), "%s:%s", tag, utf8);
        break;
    }
#endif
    default:
        return NULL;
    }
    if (n < 0 || (size_t)n >= sizeof(buf)) return NULL;
    char *out = malloc((size_t)n + 1);
    if (!out) return NULL;
    memcpy(out, buf, (size_t)n + 1);
    return out;
}

/* -- Parse ----------------------------------------------------------- */

int rtcma_devid_from_string(const char *s, ma_backend b, ma_device_id *out)
{
    const char *tag = backend_tag(b);
    if (!s || !tag || !out) return -1;
    size_t tag_len = strlen(tag);
    if (strncmp(s, tag, tag_len) != 0 || s[tag_len] != ':') return -1;
    const char *p = s + tag_len + 1;
    size_t plen   = strlen(p);

    memset(out, 0, sizeof(*out));

    switch (b) {
    case ma_backend_alsa:
        if (plen >= sizeof(out->alsa))      return -1;
        memcpy(out->alsa, p, plen + 1);     return 0;
    case ma_backend_pulseaudio:
        if (plen >= sizeof(out->pulse))     return -1;
        memcpy(out->pulse, p, plen + 1);    return 0;
    case ma_backend_coreaudio:
        if (plen >= sizeof(out->coreaudio)) return -1;
        memcpy(out->coreaudio, p, plen + 1); return 0;
    case ma_backend_sndio:
        if (plen >= sizeof(out->sndio))     return -1;
        memcpy(out->sndio, p, plen + 1);    return 0;
    case ma_backend_audio4:
        if (plen >= sizeof(out->audio4))    return -1;
        memcpy(out->audio4, p, plen + 1);   return 0;
    case ma_backend_oss:
        if (plen >= sizeof(out->oss))       return -1;
        memcpy(out->oss, p, plen + 1);      return 0;
    case ma_backend_webaudio:
        if (plen >= sizeof(out->webaudio))  return -1;
        memcpy(out->webaudio, p, plen + 1); return 0;
    case ma_backend_dsound: {
        if (plen != 32) return -1;
        for (int i = 0; i < 16; i++) {
            unsigned byte;
            if (sscanf(p + 2*i, "%2x", &byte) != 1) return -1;
            out->dsound[i] = (ma_uint8)byte;
        }
        return 0;
    }
    case ma_backend_winmm: {
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p || *end != '\0') return -1;
        out->winmm = (ma_uint32)v;
        return 0;
    }
    case ma_backend_aaudio: {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || *end != '\0') return -1;
        out->aaudio = (ma_int32)v;
        return 0;
    }
    case ma_backend_opensl: {
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p || *end != '\0') return -1;
        out->opensl = (ma_uint32)v;
        return 0;
    }
    case ma_backend_jack: {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || *end != '\0') return -1;
        out->jack = (int)v;
        return 0;
    }
    case ma_backend_null: {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || *end != '\0') return -1;
        out->nullbackend = (int)v;
        return 0;
    }
#ifdef MA_HAS_WASAPI
    case ma_backend_wasapi: {
        int cap = (int)(sizeof(out->wasapi) / sizeof(out->wasapi[0]));
        int len = MultiByteToWideChar(CP_UTF8, 0, p, -1, out->wasapi, cap);
        return len > 0 ? 0 : -1;
    }
#endif
    default:
        return -1;
    }
}

/* -- Test-only opaque wrappers ----------------------------------------
 *
 * Declared in rtcma_internal.h; let unit tests round-trip the codec
 * without seeing the ma_device_id union (so the union shape stays out
 * of the widely-included internal header). Tests treat the union as an
 * opaque byte buffer of size rtcma_internal_id_buffer_size(). */

size_t rtcma_internal_id_buffer_size(void) { return sizeof(ma_device_id); }

int rtcma_internal_backend_from_tag(const char *tag)
{
    if (!tag) return -1;
    for (int b = 0; b <= (int)ma_backend_null; b++) {
        const char *t = backend_tag((ma_backend)b);
        if (t && strcmp(t, tag) == 0) return b;
    }
    return -1;
}

char *rtcma_internal_id_to_string(int backend, const void *id_buf)
{
    return rtcma_devid_to_string((ma_backend)backend,
                                 (const ma_device_id *)id_buf);
}

int rtcma_internal_id_from_string(const char *s, int backend, void *out_buf)
{
    return rtcma_devid_from_string(s, (ma_backend)backend,
                                   (ma_device_id *)out_buf);
}
