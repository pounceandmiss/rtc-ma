#include "rtcma_internal.h"

#include <string.h>

/* speexdsp calls this whenever it lets go of a packet it was holding. The
 * payloads belong to the pool and are recycled by wrapping, so there is
 * nothing to release. The callback still has to exist: a non-NULL destroy
 * is what selects zero-copy mode. */
static void slot_release(void *data)
{
    (void)data;
}

/* Find the payload stamped `timestamp` among the packets we still hold, so
 * a concealed gap can be recovered from the FEC data in the packet that
 * follows it. Reads the pool directly rather than asking speexdsp for the
 * packet: a second jitter_buffer_get would move the play head, and on a
 * miss it would move it over audio nobody ever emitted. Copies out because
 * the slot is recycled once the ring wraps. */
static int lookahead_payload(RtcmaJitter *j, uint32_t timestamp,
                             uint8_t *out_buf, int out_buf_cap)
{
    for (int i = 0; i < RTCMA_JITTER_SLOTS; ++i) {
        if (j->pool_len[i] <= 0 || j->pool_ts[i] != timestamp) continue;
        int len = j->pool_len[i];
        if (len > out_buf_cap) return 0;
        memcpy(out_buf, j->pool[i], (size_t)len);
        return len;
    }
    return 0;
}

/* Closest stored timestamp to `wanted`, for working out whether a failed
 * lookahead means the packet is absent or means our play grid has drifted
 * off the peer's. Returns false when the pool is empty. */
static bool nearest_payload_ts(RtcmaJitter *j, uint32_t wanted,
                               uint32_t *out_ts, int32_t *out_delta)
{
    bool    found    = false;
    int32_t best     = 0;
    int64_t best_mag = 0;
    for (int i = 0; i < RTCMA_JITTER_SLOTS; ++i) {
        if (j->pool_len[i] <= 0) continue;
        int32_t d   = (int32_t)(j->pool_ts[i] - wanted);
        int64_t mag = d < 0 ? -(int64_t)d : (int64_t)d;
        if (!found || mag < best_mag) {
            best     = d;
            best_mag = mag;
            *out_ts  = j->pool_ts[i];
            found    = true;
        }
    }
    *out_delta = best;
    return found;
}

int rtcma_jitter_init(RtcmaJitter *j, int frame_samples)
{
    memset(j, 0, sizeof(*j));
    pthread_mutex_init(&j->lock, NULL);

    j->frame_samples = frame_samples;
    j->jb = jitter_buffer_init(frame_samples);
    if (!j->jb) {
        pthread_mutex_destroy(&j->lock);
        return -1;
    }
    jitter_buffer_ctl(j->jb, JITTER_BUFFER_SET_DESTROY_CALLBACK,
                      (void *)slot_release);

    /* A floor under whatever depth the estimator picks. At the default of
     * zero it plays at the live edge with about one frame of lookahead, so
     * any interruption in delivery starves it at once - measured on a real
     * call, every concealed gap was the queue running empty rather than a
     * packet going missing, in runs of one to seventeen frames.
     *
     * Three frames covers the short runs outright, which beats concealing
     * them however well. It does not touch the occasional multi-hundred
     * millisecond stall: buying those would cost more latency than the
     * glitch does. */
    spx_int32_t margin = 3 * frame_samples;
    jitter_buffer_ctl(j->jb, JITTER_BUFFER_SET_MARGIN, &margin);
    return 0;
}

void rtcma_jitter_destroy(RtcmaJitter *j)
{
    if (j->jb) {
        jitter_buffer_destroy(j->jb);
        j->jb = NULL;
    }
    pthread_mutex_destroy(&j->lock);
}

bool rtcma_jitter_put(RtcmaJitter *j, uint32_t timestamp, uint16_t seq,
                      const uint8_t *payload, int len, int span)
{
    pthread_mutex_lock(&j->lock);

    if (len <= 0 || len > RTCMA_JITTER_MAX_PAYLOAD || span <= 0) {
        j->stat_put_reject++;
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    int      idx  = j->next_slot;
    uint8_t *slot = j->pool[idx];
    j->next_slot = (idx + 1) % RTCMA_JITTER_SLOTS;
    memcpy(slot, payload, (size_t)len);
    j->pool_ts[idx]  = timestamp;
    j->pool_len[idx] = len;

    JitterBufferPacket p;
    p.data      = (char *)slot;
    p.len       = (spx_uint32_t)len;
    p.timestamp = timestamp;
    p.span      = (spx_uint32_t)span;
    p.sequence  = seq;
    p.user_data = 0;
    jitter_buffer_put(j->jb, &p);
    j->stat_put++;

    int32_t ahead = (int32_t)(timestamp
                              - (uint32_t)jitter_buffer_get_pointer_timestamp(j->jb));

    pthread_mutex_unlock(&j->lock);

    rtcma_log(RTCMA_LOG_VERBOSE,
              "put seq=%u ts=%u span=%d len=%d, %+d samples from the play "
              "point", (unsigned)seq, timestamp, span, len, (int)ahead);
    return true;
}

RtcmaJitterResult rtcma_jitter_get(RtcmaJitter *j, uint8_t *out_buf,
                                   int out_buf_cap, int *out_len,
                                   int *out_span, int *out_skip)
{
    *out_len  = 0;
    *out_span = 0;
    *out_skip = 0;

    pthread_mutex_lock(&j->lock);

    /* A packet held back by the previous call plays now. Its clock already
     * counted that packet as audio we were sitting on, so report the
     * handover instead of ticking a fresh period. */
    if (j->held) {
        int len = j->held_len;
        if (len > out_buf_cap) len = out_buf_cap;
        memcpy(out_buf, j->held, (size_t)len);
        *out_len = len;
        j->held  = NULL;
        jitter_buffer_remaining_span(j->jb, 0);
        j->stat_frame++;
        pthread_mutex_unlock(&j->lock);
        return RTCMA_JITTER_FRAME;
    }

    /* Tracing state, gathered under the lock and emitted once it is
     * released so the audio thread never logs while holding it. */
    bool     diag_miss = false, diag_have_near = false, diag_tick = false;
    uint32_t diag_want = 0, diag_near = 0, diag_at = 0;
    int32_t  diag_delta = 0;
    int      diag_queued = 0, diag_depth = 0;

    JitterBufferPacket p;
    memset(&p, 0, sizeof(p));
    spx_int32_t offset = 0;

    int rc = jitter_buffer_get(j->jb, &p, j->frame_samples, &offset);
    jitter_buffer_tick(j->jb);

    RtcmaJitterResult result;
    if (rc == JITTER_BUFFER_OK) {
        int len = (int)p.len;
        if (len > out_buf_cap) len = out_buf_cap;
        memcpy(out_buf, p.data, (size_t)len);
        *out_len = len;

        if (offset > 0) {
            /* The packet starts after the point we are playing from, so
             * there is a gap in front of it. Conceal the gap now and keep
             * the packet for the next pull. It is also what carries the
             * gap's FEC data, which is why it goes out now as well. */
            j->held     = (const uint8_t *)p.data;
            j->held_len = len;
            *out_span   = offset;
            j->stat_conceal++;
            j->stat_conceal_fec++;
            result = RTCMA_JITTER_CONCEAL;
        } else {
            /* A packet that starts before the play point overlaps audio
             * already emitted; the decoder still has to run over the whole
             * frame, so the caller drops the overlap afterwards. */
            *out_skip = -offset;
            j->stat_frame++;
            result = RTCMA_JITTER_FRAME;
        }
    } else if (rc == JITTER_BUFFER_INSERTION) {
        *out_span = (int)p.span;
        j->stat_stretch++;
        result = RTCMA_JITTER_STRETCH;
    } else {
        /* JITTER_BUFFER_MISSING. A span of zero means the buffer has
         * nothing at all yet and has not picked a play point, which is
         * silence rather than a gap in a stream. */
        *out_span = (int)p.span;
        if (*out_span <= 0) {
            result = RTCMA_JITTER_SILENT;
        } else {
            j->stat_conceal++;
            /* p.timestamp is where the gap starts; the packet after it is
             * one span further on. */
            uint32_t want = p.timestamp + (uint32_t)*out_span;
            *out_len = lookahead_payload(j, want, out_buf, out_buf_cap);
            if (*out_len > 0) {
                j->stat_conceal_fec++;
            } else {
                /* Distinguish a gap in a stream that is still flowing from
                 * one where the queue has run dry. Dry means the successor
                 * is not here yet, which on a real link is usually delivery
                 * running late rather than the packet being lost, so depth
                 * helps where FEC cannot. */
                spx_int32_t queued = 0;
                jitter_buffer_ctl(j->jb, JITTER_BUFFER_GET_AVAILABLE_COUNT,
                                  &queued);
                if (queued <= 0) j->stat_conceal_dry++;

                diag_miss      = true;
                diag_want      = want;
                diag_queued    = (int)queued;
                diag_at        = p.timestamp;
                diag_have_near = nearest_payload_ts(j, want,
                                                    &diag_near, &diag_delta);
            }
            result = RTCMA_JITTER_CONCEAL;
        }
    }

    /* Every 250 pulls is about five seconds of playback. */
    if (++j->gets % 250 == 0) {
        spx_int32_t queued = 0;
        jitter_buffer_ctl(j->jb, JITTER_BUFFER_GET_AVAILABLE_COUNT, &queued);
        uint32_t near_ts = 0;
        int32_t  near_d  = 0;
        diag_tick   = true;
        diag_queued = (int)queued;
        diag_at     = (uint32_t)jitter_buffer_get_pointer_timestamp(j->jb);
        diag_depth  = nearest_payload_ts(j, diag_at, &near_ts, &near_d)
                    ? near_d : 0;
    }

    pthread_mutex_unlock(&j->lock);

    if (diag_miss) {
        if (diag_have_near) {
            rtcma_log(RTCMA_LOG_DEBUG,
                      "conceal at ts=%u: want successor ts=%u, absent; "
                      "queued=%d nearest stored ts=%u (%+d samples)",
                      diag_at, diag_want, diag_queued, diag_near,
                      (int)diag_delta);
        } else {
            rtcma_log(RTCMA_LOG_DEBUG,
                      "conceal at ts=%u: want successor ts=%u, pool empty; "
                      "queued=%d",
                      diag_at, diag_want, diag_queued);
        }
    }
    if (diag_tick) {
        rtcma_log(RTCMA_LOG_DEBUG,
                  "jitter at ts=%u: queued=%d nearest%+d samples away; "
                  "frame=%llu conceal=%llu fec=%llu dry=%llu stretch=%llu",
                  diag_at, diag_queued, (int)diag_depth,
                  (unsigned long long)j->stat_frame,
                  (unsigned long long)j->stat_conceal,
                  (unsigned long long)j->stat_conceal_fec,
                  (unsigned long long)j->stat_conceal_dry,
                  (unsigned long long)j->stat_stretch);
    }
    return result;
}
