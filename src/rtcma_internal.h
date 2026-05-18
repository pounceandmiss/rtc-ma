#ifndef RTCMA_INTERNAL_H
#define RTCMA_INTERNAL_H

#include "rtcma.h"  // IWYU pragma: keep

#include <opus/opus.h>
#include <pthread.h>
#include <rtc/rtc.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* -- Logging (rtcma_log.c) --------------------------------------------
 *
 * Internal counterpart to the public rtcmaInitLogger API. All adapter
 * code that previously wrote to stderr directly should go through this
 * with an appropriate level - the caller decides what to keep via
 * rtcmaInitLogger.
 *
 * The "rtcma: " prefix is added automatically; callers pass only the
 * message body and no trailing newline. */
void rtcma_log(rtcmaLogLevel level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* -- RTP demux (rtcma_track.c) ----------------------------------------
 *
 * Pure RFC 3550 / 8285 RTP header parser. Extracted from on_track_message
 * so it can be unit-tested without libdatachannel handles. libdatachannel
 * delivers every RTP packet on a given SSRC to the track callback - it
 * does NOT filter by payload type - so the m-line's other PTs (CN=13,
 * telephone-event=110, RED=63, ...) reach us alongside opus(111). Without
 * a PT filter those non-opus payloads get pushed into the jitter ring
 * and fail opus_decode with OPUS_INVALID_PACKET. */

typedef enum {
    RTCMA_RTP_ACCEPT,     /* parse ok, PT matches expected_pt          */
    RTCMA_RTP_SKIP,       /* parse ok, PT does not match (CN/DTMF/RED) */
    RTCMA_RTP_MALFORMED,  /* truncated / wrong version / bad lengths   */
} RtcmaRtpParse;

/* msg/size: raw RTP packet from libdatachannel. expected_pt: the PT we
 * negotiated for this track (caller's t->payload_type). On ACCEPT,
 * *out_seq is the 16-bit seq, *out_payload points into msg at the start
 * of the codec payload, and *out_payload_len is its length with any RTP
 * padding stripped. Outputs are untouched on SKIP / MALFORMED. */
RtcmaRtpParse rtcma_parse_rtp(const uint8_t *msg, int size, int expected_pt,
                              uint16_t *out_seq,
                              const uint8_t **out_payload,
                              int *out_payload_len);

/* -- Device-id serialisation (rtcma_audio.c) --------------------------
 *
 * Exposed for unit testing. The C public API only ever exchanges
 * "backend:payload" strings; the union form (ma_device_id) stays
 * internal. Tests treat the union as an opaque byte buffer.
 *
 *   rtcma_internal_id_buffer_size()      -> sizeof(ma_device_id)
 *   rtcma_internal_backend_from_tag(s)   -> ma_backend enum int, or -1
 *   rtcma_internal_id_to_string(b, buf)  -> malloc'd ASCII or NULL
 *   rtcma_internal_id_from_string(s,b,b) -> 0 on success, -1 on reject
 */
size_t  rtcma_internal_id_buffer_size(void);
int     rtcma_internal_backend_from_tag(const char *tag);
char   *rtcma_internal_id_to_string(int backend, const void *id_buf);
int     rtcma_internal_id_from_string(const char *s, int backend,
                                      void *out_buf);

/* opus_decode at 48 kHz can return up to 120 ms per packet = 5760
 * samples per channel. Most WebRTC senders use 20 ms (960 samples)
 * but libwebrtc switches to 40/60 ms under bandwidth adaptation and
 * `opus_decode` returns OPUS_BUFFER_TOO_SMALL (-2) if the caller's
 * buffer can't hold the actual frame. Decoder-side buffers (here,
 * audio.c on_playback, and RtcmaPlayer.pb_leftover) must be sized to
 * RTCMA_DECODE_MAX_SAMPLES x channels. */
#define RTCMA_DECODE_MAX_SAMPLES 5760

/* -- Jitter buffer ----------------------------------------------------
 * Tiny reorder ring keyed by RTP sequence number. Stores raw codec
 * payload (post-RTP-strip) so the consumer decodes on drain. Drops late
 * packets older than the play head by more than RTCMA_JITTER_RING/2 slots.
 *
 * Sized at 8 slots = 160 ms of 20 ms Opus frames. Producer (libdatachannel
 * worker thread, inside the message callback) calls rtcma_jitter_put;
 * consumer (audio playback callback) calls rtcma_jitter_get when it
 * needs another frame to decode.
 *
 * Not codec-specific - the payload is opaque bytes. */

#define RTCMA_JITTER_RING        8
#define RTCMA_JITTER_MAX_PAYLOAD 1500   /* one MTU is enough for Opus 20 ms */

typedef struct {
    uint8_t  data[RTCMA_JITTER_MAX_PAYLOAD];
    int      len;
    uint16_t seq;
    bool     present;
} RtcmaJitterSlot;

typedef struct {
    RtcmaJitterSlot  slots[RTCMA_JITTER_RING];
    uint16_t         play_seq;        /* next seq to drain */
    bool             primed;          /* true once we've seen first packet */
    bool             playing;         /* true once initial fill reached */
    int              fill_count;      /* slots currently holding data */
    int              prime_threshold; /* fill_count >= this before play starts */
    pthread_mutex_t  lock;

    /* Stats (helpful for tests and future logging) */
    uint64_t         stat_put;
    uint64_t         stat_get_present;
    uint64_t         stat_get_miss;
    uint64_t         stat_get_miss_empty;  /* miss while fill_count==0 */
    uint64_t         stat_late_drop;
    uint64_t         stat_dup_drop;
} RtcmaJitter;

void rtcma_jitter_init(RtcmaJitter *j);
void rtcma_jitter_destroy(RtcmaJitter *j);
/* Insert a packet. Idempotent on duplicate seq. Returns true if accepted. */
bool rtcma_jitter_put(RtcmaJitter *j, uint16_t seq,
                      const uint8_t *payload, int len);
/* Drain one slot. Returns true if a packet is present; out_len/out_buf
 * filled. Returns false if slot is empty (caller should do PLC) - in
 * that case out_len is 0. Always advances the play head once primed. */
bool rtcma_jitter_get(RtcmaJitter *j, uint8_t *out_buf, int out_buf_cap,
                      int *out_len);

/* -- SDP probe (rtcma_sdp.c) ------------------------------------------
 * Pure SDP parser. Pulls the opus fmtp parameters tacky needs to honour
 * for inter-op with non-tacky XMPP/Jingle clients (gajim, Dino,
 * Conversations, ...). Exposed so it can be unit-tested without
 * libdatachannel handles.
 *
 * Defaults follow RFC 7587 sec.7: mono, no FEC, no DTX. Unspecified
 * rate/bitrate fields stay at 0 - callers treat zero as "no cap." */
typedef struct {
    int channels;            /* 1 or 2 - sprop-stereo=1 -> 2, else 1   */
    int useinbandfec;        /* fmtp useinbandfec=1                    */
    int usedtx;              /* fmtp usedtx=1                          */
    int cbr;                 /* fmtp cbr=1                             */
    int maxplaybackrate;     /* Hz, 0 = unspecified                    */
    int maxaveragebitrate;   /* bits/sec, 0 = unspecified              */
    int minptime;            /* ms, 0 = unspecified                    */
} RtcmaOpusParams;

void rtcma_opus_params_from_sdp(const char *sdp, int pt,
                                RtcmaOpusParams *out);

/* Legacy thin wrapper, retained because test_sdp_parse.c still
 * exercises the channels-only contract directly. */
int  rtcma_opus_channels_from_sdp(const char *sdp, int pt);

/* -- Per-track adapter (rtcma_track.c) --------------------------------
 *
 * One instance per attached track id. The Player owns the recv-side
 * adapter; the Capturer owns the send-side adapter. They are independent
 * even when bound to the same rtc_track_id - sendrecv tracks just have
 * one of each.
 *
 * Lifetime / threading:
 *  - rtcma_recv_track_attach installs rtcSetMessageCallback. After that,
 *    on_track_message fires on libdatachannel worker threads and pushes
 *    into the jitter ring.
 *  - rtcma_recv_track_pull_pcm is called from the audio data callback;
 *    it pulls one slot, opus-decodes, returns one 20 ms frame. Decode
 *    is driven by the audio device clock - do NOT call this on a timer.
 *  - rtcma_send_track_push_pcm is called from the audio data callback
 *    once a full 20 ms accumulator is ready; it opus-encodes and calls
 *    rtcSendMessage. enc_lock serialises multiple senders in case a
 *    future API exposes external PCM push.
 *  - detach calls rtcSetMessageCallback(id, NULL) which blocks until any
 *    in-flight on_track_message returns (libdatachannel's
 *    synchronized_callback mutex). So tearing down jitter / decoder /
 *    encoder immediately after detach is safe.
 */

typedef struct {
    int              rtc_track_id;    /* -1 when not attached */
    int              payload_type;
    int              channels;

    OpusDecoder     *dec;
    RtcmaJitter      jitter;
} RtcmaRecvTrack;

typedef struct {
    int              rtc_track_id;    /* -1 when not attached */
    int              payload_type;
    int              channels;

    /* SSRC, RTP sequence number, and RTP timestamp for the *next*
     * outgoing packet. We build the 12-byte RFC 3550 header ourselves
     * in rtcma_send_track_push_pcm and ship it via rtcSendMessage with
     * NO media handler installed on the track, so libdatachannel's
     * transport path SRTP-encrypts our bytes verbatim - sidestepping
     * libdatachannel's OpusRtpPacketizer entirely.
     *
     * Why bypass: the C API for rtcSetOpusPacketizer has no MID/extmap
     * setter and the packetizer hard-codes marker=1 on every audio
     * packet (it computes mark = i == payloads.size()-1 and Opus is
     * always a single payload). Libwebrtc-based receivers drop streams
     * whose RTP shape doesn't match what the negotiated extmaps and
     * audio profile imply.
     *
     * Initial seq and timestamp are randomised per RFC 3550 sec.5.1; both
     * advance by exactly one frame on every successful send. */
    uint32_t         ssrc;
    uint16_t         next_seq;
    uint32_t         next_timestamp;

    OpusEncoder     *enc;
    pthread_mutex_t  enc_lock;        /* serialises encode + rtcSendMessage */

    /* Send-path diagnostic counters. Log first N attempts, transitions
     * between rc=fail and rc=ok, and an aggregate count on detach so a
     * silent send-side failure (closed track, missing transport, malformed
     * packet rejected by packetizer) is visible. */
    _Atomic int      diag_send_logged;     /* # of calls already verbose-logged */
    _Atomic uint64_t diag_send_ok;         /* rtcSendMessage rc >= 0           */
    _Atomic uint64_t diag_send_fail;       /* rtcSendMessage rc <  0           */
    _Atomic int      diag_last_rc;         /* last seen rc (for change-detect) */
} RtcmaSendTrack;

/* Receive: resolve PT + channels (config override or SDP probe),
 * create decoder, install rtcSetMessageCallback + RTCP receiving
 * session, store track id. channels_override / pt_override of 0 means
 * "probe SDP." Returns 0 on success; -1 on probe failure, decoder
 * failure, or libdatachannel error. On failure the recv track is left
 * unattached and the libdatachannel callback slot is untouched. */
int  rtcma_recv_track_attach(RtcmaRecvTrack *t, int rtc_track_id,
                             int channels_override, int pt_override);
void rtcma_recv_track_detach(RtcmaRecvTrack *t);

/* Pull one Opus frame. Returns 0 (caller fills silence) until
 * prime_threshold reached; returns the per-channel sample count actually
 * decoded (typically 960 for 20 ms, but can be up to
 * RTCMA_DECODE_MAX_SAMPLES when peer sends 40/60/120 ms) on success or
 * PLC once playing; returns -1 on decode error. pcm capacity must be at
 * least RTCMA_DECODE_MAX_SAMPLES * channels int16. */
int  rtcma_recv_track_pull_pcm(RtcmaRecvTrack *t, int16_t *pcm,
                               size_t pcm_capacity_samples);

/* Send: resolve PT + channels (config override or SDP probe), apply
 * encoder ctls from SDP fmtp (useinbandfec, usedtx, cbr,
 * maxaveragebitrate, maxplaybackrate), store track id. channels / pt of
 * 0 means "probe SDP." Mirrors rtcma_recv_track_attach. Does NOT
 * install any libdatachannel callback. Returns 0 / -1. */
int  rtcma_send_track_attach(RtcmaSendTrack *t, int rtc_track_id,
                             int channels_override, int pt_override);
void rtcma_send_track_detach(RtcmaSendTrack *t);

/* Encode one 20 ms frame and rtcSendMessage it. samples_per_channel
 * MUST equal RTCMA_FRAME_SAMPLES. Returns 0 / -1. */
int  rtcma_send_track_push_pcm(RtcmaSendTrack *t, const int16_t *pcm,
                               size_t samples_per_channel);

#endif /* RTCMA_INTERNAL_H */
