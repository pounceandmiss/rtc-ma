#include "rtcma_internal.h"

#include <string.h>

/* Signed-arithmetic seq compare: positive => a is later than b modulo 65536. */
static int16_t seq_cmp(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b);
}

void rtcma_jitter_init(RtcmaJitter *j)
{
    memset(j, 0, sizeof(*j));
    pthread_mutex_init(&j->lock, NULL);
    /* 3 frames = 60 ms target depth. The play head holds this much buffered
     * ahead of itself; higher absorbs more jitter at the cost of latency. */
    j->prime_threshold = 3;
}

void rtcma_jitter_destroy(RtcmaJitter *j)
{
    pthread_mutex_destroy(&j->lock);
}

bool rtcma_jitter_put(RtcmaJitter *j, uint16_t seq,
                      const uint8_t *payload, int len)
{
    if (!j || !payload || len <= 0 || len > RTCMA_JITTER_MAX_PAYLOAD)
        return false;

    pthread_mutex_lock(&j->lock);
    j->stat_put++;

    /* First packet ever: anchor the play head to it. */
    if (!j->primed) {
        j->play_seq = seq;
        j->primed   = true;
    }

    int16_t delta = seq_cmp(seq, j->play_seq);

    /* Too old to be useful: its playout slot has already drained. Drop.
     * Window is RING/2 to give legitimate small reorders a chance, while
     * still rejecting stragglers from way behind. */
    if (delta < -(RTCMA_JITTER_RING / 2)) {
        j->stat_late_drop++;
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    /* Too far ahead: ring would wrap onto itself and overwrite slots we
     * still want to play. Force the play head forward (skipping pending
     * frames as PLC misses) rather than letting writes collide with
     * unread slots. */
    if (delta >= RTCMA_JITTER_RING) {
        uint16_t new_play = (uint16_t)(seq - (RTCMA_JITTER_RING - 1));
        while (seq_cmp(new_play, j->play_seq) > 0) {
            RtcmaJitterSlot *s = &j->slots[j->play_seq % RTCMA_JITTER_RING];
            if (s->present) {
                s->present = false;
                j->fill_count--;
            }
            j->play_seq++;
        }
        delta = seq_cmp(seq, j->play_seq);
    }

    RtcmaJitterSlot *slot = &j->slots[seq % RTCMA_JITTER_RING];

    /* Duplicate: same seq, already filled. Drop (RTP allows duplicates). */
    if (slot->present && slot->seq == seq) {
        j->stat_dup_drop++;
        pthread_mutex_unlock(&j->lock);
        return false;
    }

    /* Collision: a stale slot from RTCMA_JITTER_RING frames ago that we
     * never drained. Shouldn't happen if put rate ~= get rate, but be
     * defensive. */
    if (slot->present && slot->seq != seq) {
        j->fill_count--;
    }

    memcpy(slot->data, payload, (size_t)len);
    slot->len     = len;
    slot->seq     = seq;
    slot->present = true;
    j->fill_count++;

    /* Track the live edge so get() can tell a late frame (hold and conceal)
     * from a genuinely lost one (skip forward). */
    if (!j->have_highest || seq_cmp(seq, j->highest_seq) > 0) {
        j->highest_seq  = seq;
        j->have_highest = true;
    }

    pthread_mutex_unlock(&j->lock);
    return true;
}

bool rtcma_jitter_get(RtcmaJitter *j, uint8_t *out_buf, int out_buf_cap,
                      int *out_len)
{
    if (!j || !out_buf || !out_len) return false;
    *out_len = 0;

    pthread_mutex_lock(&j->lock);

    /* Initial buffering: don't start draining until fill_count crosses
     * the prime threshold. Once we start playing, we keep going even if
     * the ring temporarily empties (those gaps become PLC misses, the
     * caller fills with concealment audio). Without this latch, a drop
     * back below the threshold would re-pause playout mid-call. */
    if (!j->playing) {
        if (!j->primed || j->fill_count < j->prime_threshold) {
            pthread_mutex_unlock(&j->lock);
            return false;
        }
        j->playing = true;
    }

    RtcmaJitterSlot *slot = &j->slots[j->play_seq % RTCMA_JITTER_RING];
    bool present = (slot->present && slot->seq == j->play_seq);

    if (present) {
        if (slot->len > out_buf_cap) {
            /* Caller's buffer too small; treat as miss to avoid overflow. */
            present = false;
        } else {
            memcpy(out_buf, slot->data, (size_t)slot->len);
            *out_len = slot->len;
        }
        slot->present = false;
        j->fill_count--;
        j->stat_get_present++;
        j->hold_count = 0;
        j->play_seq++;
        pthread_mutex_unlock(&j->lock);
        return present;
    }

    /* Miss. Only advance the play head when newer frames have piled up past
     * the target depth - then this slot is a real gap to skip. At or under
     * target the frame is merely late: hold position so it still plays when
     * it arrives, and let the caller conceal meanwhile. Without this the head
     * runs past the live edge and strands frames that arrive a beat late.
     * MAX_HOLD bounds the stall if a live-edge frame is truly lost. */
    j->stat_get_miss++;
    if (j->fill_count == 0) j->stat_get_miss_empty++;

    int16_t depth = j->have_highest ? seq_cmp(j->highest_seq, j->play_seq) : 0;
    if (depth > j->prime_threshold || j->hold_count >= RTCMA_JITTER_MAX_HOLD) {
        j->play_seq++;
        j->hold_count = 0;
    } else {
        j->hold_count++;
    }

    pthread_mutex_unlock(&j->lock);
    return false;
}
