#ifndef RTCMA_INTERNAL_H
#define RTCMA_INTERNAL_H

#include "rtcma.h"  // IWYU pragma: keep
#include "speex/speex_jitter.h"

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
 * *out_seq is the 16-bit seq, *out_timestamp the 32-bit RTP timestamp
 * (48 kHz sample units for opus), *out_payload points into msg at the
 * start of the codec payload, and *out_payload_len is its length with
 * any RTP padding stripped. Outputs are untouched on SKIP / MALFORMED. */
RtcmaRtpParse rtcma_parse_rtp(const uint8_t *msg, int size, int expected_pt,
                              uint16_t *out_seq,
                              uint32_t *out_timestamp,
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
 *
 * speexdsp's adaptive jitter buffer (src/speexdsp) behind a thin adapter.
 * It runs in RTP timestamp units, which for opus are 48 kHz samples, and
 * stores opaque payloads: the only codec-specific step is asking opus how
 * many samples a payload spans before handing it over.
 *
 * It picks its own play-out depth from the arrival timings it sees,
 * trading latency against the number of packets a given delay would
 * strand. Surplus depth is shed internally with no signal to the caller;
 * too little produces a stretch request.
 *
 * Producer is libdatachannel's worker thread inside on_track_message;
 * consumer is the audio device callback. speexdsp does no locking of its
 * own and a put can reset the whole buffer, so every entry point here
 * takes the lock.
 *
 * Payloads live in `pool` and are handed over by pointer. The no-op
 * destroy callback is what selects that zero-copy mode; without one,
 * speexdsp calloc()s on every put and free()s on every get, and get runs
 * on the audio thread. Slots are reused round-robin, so a payload stays
 * valid for RTCMA_JITTER_SLOTS further packets - 1.28 s at 20 ms, long
 * past the point where the buffer would have dropped it as unplayable. */

#define RTCMA_JITTER_MAX_PAYLOAD 1500   /* one MTU is enough for Opus 20 ms */
#define RTCMA_JITTER_SLOTS       64     /* payload ring depth */

typedef enum {
    RTCMA_JITTER_SILENT,    /* nothing playable yet; caller fills silence  */
    RTCMA_JITTER_FRAME,     /* payload in out_buf; decode it               */
    RTCMA_JITTER_CONCEAL,   /* gap; conceal *out_span samples              */
    RTCMA_JITTER_STRETCH,   /* depth too low; conceal *out_span samples    */
} RtcmaJitterResult;

typedef struct {
    JitterBuffer    *jb;
    pthread_mutex_t  lock;
    int              frame_samples;  /* chunk we ask for on each get */

    uint8_t          pool[RTCMA_JITTER_SLOTS][RTCMA_JITTER_MAX_PAYLOAD];
    uint32_t         pool_ts[RTCMA_JITTER_SLOTS];
    int              pool_len[RTCMA_JITTER_SLOTS];
    int              next_slot;

    /* Packet the buffer returned with a gap in front of it. The gap plays
     * first as a conceal; this plays on the pull after that. */
    const uint8_t   *held;
    int              held_len;

    uint64_t         gets;              /* pulls so far, for periodic tracing */

    uint64_t         stat_put;
    uint64_t         stat_put_reject;   /* no span from opus, or oversized */
    uint64_t         stat_frame;
    uint64_t         stat_conceal;
    uint64_t         stat_conceal_fec;  /* conceals with a recovery source */
    uint64_t         stat_conceal_dry;  /* conceals with nothing buffered at
                                           all, so no successor can exist */
    uint64_t         stat_stretch;
} RtcmaJitter;

/* frame_samples is the nominal frame at the decoder's rate (960 for 20 ms
 * at 48 kHz). It seeds both the adjustment quantum and the concealment
 * granularity, so concealment spans come back as whole frames. Returns 0,
 * or -1 if speexdsp could not allocate. */
int  rtcma_jitter_init(RtcmaJitter *j, int frame_samples);
void rtcma_jitter_destroy(RtcmaJitter *j);

/* span is the packet's duration in the same units as timestamp. Returns
 * false if the payload is too large to store. Packets that are too late
 * to play are accepted here and dropped inside speexdsp, which still
 * counts their lateness towards the delay estimate. */
bool rtcma_jitter_put(RtcmaJitter *j, uint32_t timestamp, uint16_t seq,
                      const uint8_t *payload, int len, int span);

/* Produce one playback chunk. On FRAME, out_buf/out_len hold the payload
 * and *out_skip is how many leading samples of the decoded frame have
 * already been played and must be dropped. On CONCEAL and STRETCH,
 * *out_span is how many samples to conceal.
 *
 * On CONCEAL only, a non-zero *out_len means out_buf holds the packet that
 * comes immediately after the gap. That is not audio to play: opus carries
 * a copy of the previous frame inside the next packet, so the caller can
 * recover the gap from it instead of extrapolating. The packet stays
 * queued and a later call returns it as a FRAME.
 *
 * Advances the buffer's clock, so call it exactly once per playback
 * period. */
RtcmaJitterResult rtcma_jitter_get(RtcmaJitter *j, uint8_t *out_buf,
                                   int out_buf_cap, int *out_len,
                                   int *out_span, int *out_skip);

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

/* Pull one chunk of PCM. Returns 0 (caller fills silence) while the
 * jitter buffer has nothing to play yet; otherwise the per-channel sample
 * count decoded, whether from a real packet or concealment (typically 960
 * for 20 ms, up to RTCMA_DECODE_MAX_SAMPLES when the peer sends
 * 40/60/120 ms); -1 on decode error. pcm capacity must be at least
 * RTCMA_DECODE_MAX_SAMPLES * channels int16. */
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
