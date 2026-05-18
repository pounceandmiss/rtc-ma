#include "rtcma_internal.h"

#include <stdarg.h>
#include <stdio.h>

/* Globals are last-writer-wins: rtcmaInitLogger may be called from any
 * thread, and rtcma_log fires from libdatachannel workers, audio
 * callbacks, and the consumer thread. A torn read of either field is
 * harmless - worst case is one stale message routed by the previous
 * setting before the new one takes effect. C11 atomics give us that
 * guarantee without a mutex. */
static _Atomic rtcmaLogLevel        g_level = RTCMA_LOG_NONE;
static _Atomic(rtcmaLogCallbackFunc) g_cb   = NULL;

void rtcmaInitLogger(rtcmaLogLevel level, rtcmaLogCallbackFunc cb)
{
    atomic_store(&g_level, level);
    atomic_store(&g_cb,    cb);
}

void rtcma_log(rtcmaLogLevel level, const char *fmt, ...)
{
    if (level == RTCMA_LOG_NONE) return;
    if ((int)level > (int)atomic_load(&g_level)) return;

    /* One line per emission. 512 bytes covers the longest existing
     * adapter line (the "send attached" diag, ~150 chars expanded) with
     * headroom; longer messages are truncated. */
    char buf[512];
    int  used = snprintf(buf, sizeof(buf), "rtcma: ");
    if (used < 0 || (size_t)used >= sizeof(buf)) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, sizeof(buf) - (size_t)used, fmt, ap);
    va_end(ap);

    rtcmaLogCallbackFunc cb = atomic_load(&g_cb);
    if (cb) {
        cb(level, buf);
    } else {
        fprintf(stderr, "%s\n", buf);
    }
}
