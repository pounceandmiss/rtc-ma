/* rtcma_device_id.h - internal ma_device_id <-> "backend:payload" codec.
 *
 * Shared between rtcma_audio.c (init paths that pin a device, plus the
 * enumerate path that emits id strings for the caller to persist) and
 * rtcma_device_id.c (the codec itself). The opaque test-facing API
 * (rtcma_internal_id_*) lives in rtcma_internal.h so test_*.c files
 * never need to see ma_device_id's union shape. */

#ifndef RTCMA_DEVICE_ID_H
#define RTCMA_DEVICE_ID_H

#include "miniaudio.h"

/* Resolve the active miniaudio backend, memoizing on the first call.
 * Returns 0 on success, -1 if ma_context_init failed. */
int rtcma_devid_resolve_active_backend(ma_backend *out);

/* Memoize the backend without spinning up a fresh ma_context. Used by
 * rtcma_enumerate_devices to piggyback on the context it already
 * created, so later device-init calls skip an extra ma_context_init. */
void rtcma_devid_set_active_backend(ma_backend b);

/* "<tag>:<payload>" -> ma_device_id union for the given backend. Returns
 * 0 on success, -1 on bad format / wrong backend tag / overlong
 * payload. `out` is zeroed first; unused union members stay zero. */
int rtcma_devid_from_string(const char *s, ma_backend b, ma_device_id *out);

/* ma_device_id -> malloc'd "<tag>:<payload>" ASCII string. Caller frees.
 * Returns NULL on error. Only reads the union member that corresponds
 * to `b`; padding elsewhere in the union is ignored. */
char *rtcma_devid_to_string(ma_backend b, const ma_device_id *id);

#endif /* RTCMA_DEVICE_ID_H */
