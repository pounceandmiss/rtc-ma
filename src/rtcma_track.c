#include "rtcma_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Parse an RFC 3550 RTP header (with RFC 8285 sec.4 extension support for
 * both one-byte 0xBEDE and two-byte 0x1000 profiles - the length-in-
 * 32-bit-words field is in the same place either way) and report back
 * whether this packet should be queued for the bound decoder.
 *
 * libdatachannel delivers every packet on the SSRC regardless of PT,
 * so the m-line's CN(13)/DTMF(110)/RED(63)/PCMU/PCMA traffic surfaces
 * here too. SKIP those instead of letting opus_decode reject them with
 * -4. MALFORMED is reserved for packets we can't even safely strip the
 * header from. */
RtcmaRtpParse rtcma_parse_rtp(const uint8_t *msg, int size, int expected_pt,
                              uint16_t *out_seq,
                              const uint8_t **out_payload,
                              int *out_payload_len)
{
    if (!msg || size < 12) return RTCMA_RTP_MALFORMED;
    const uint8_t *p = msg;

    int version = (p[0] >> 6) & 0x3;
    if (version != 2) return RTCMA_RTP_MALFORMED;

    int csrc_count = p[0] & 0x0F;
    int has_ext    = (p[0] >> 4) & 0x1;
    int header_len = 12 + csrc_count * 4;
    if (size < header_len) return RTCMA_RTP_MALFORMED;

    if (has_ext) {
        if (size < header_len + 4) return RTCMA_RTP_MALFORMED;
        int ext_len_words = ((int)p[header_len + 2] << 8) | p[header_len + 3];
        header_len += 4 + ext_len_words * 4;
        if (size < header_len) return RTCMA_RTP_MALFORMED;
    }

    int padding = 0;
    if ((p[0] >> 5) & 0x1) {
        padding = p[size - 1];
        if (padding > size - header_len) return RTCMA_RTP_MALFORMED;
    }

    int payload_len = size - header_len - padding;
    if (payload_len <= 0) return RTCMA_RTP_MALFORMED;

    int pt = p[1] & 0x7F;
    if (expected_pt >= 0 && pt != expected_pt) return RTCMA_RTP_SKIP;

    if (out_seq)         *out_seq         = ((uint16_t)p[2] << 8) | p[3];
    if (out_payload)     *out_payload     = p + header_len;
    if (out_payload_len) *out_payload_len = payload_len;
    return RTCMA_RTP_ACCEPT;
}

/* -- Receive: libdatachannel hands us raw RTP packets. -----------------
 * Decoding happens lazily on each caller pull via rtcma_recv_track_pull_pcm
 * - driven by the audio device's data callback so decode rate matches
 * playback rate exactly (no drift, no underrun cracks). */
static void on_track_message(int id, const char *msg, int size, void *ptr)
{
    (void)id;
    RtcmaRecvTrack *t = ptr;
    if (!t) return;

    uint16_t       seq;
    const uint8_t *payload;
    int            payload_len;
    if (rtcma_parse_rtp((const uint8_t *)msg, size, t->payload_type,
                        &seq, &payload, &payload_len) != RTCMA_RTP_ACCEPT)
        return;

    rtcma_jitter_put(&t->jitter, seq, payload, payload_len);
}

/* Resolve PT and fmtp opus parameters via libdatachannel, honouring
 * caller overrides. pt_inout of 0 means "probe via
 * rtcGetTrackPayloadTypesForCodec." channels_override of 0 means "take
 * the value the SDP probe yields"; non-zero clamps `params->channels`
 * to that value (used by the public Player/Capturer API to let callers
 * pin to hardware constraints). Returns 0 on success; -1 if a probe
 * fails or the resolved channels are out of range. Per the rtc-ma
 * design, NO silent fallback to PT=111 - wrong PT produces silent
 * receive that is awful to track down. */
static int resolve_pt_and_params(int rtc_track_id,
                                 int *pt_inout, int channels_override,
                                 RtcmaOpusParams *params)
{
    if (*pt_inout == 0) {
        int pts[8] = {0};
        int n = rtcGetTrackPayloadTypesForCodec(rtc_track_id, "opus", pts,
                                                (int)(sizeof(pts) / sizeof(pts[0])));
        if (n <= 0) {
            fprintf(stderr, "rtcma: rtcGetTrackPayloadTypesForCodec(opus) "
                            "returned %d on track %d\n", n, rtc_track_id);
            return -1;
        }
        *pt_inout = pts[0];
    }

    char sdp[4096];
    int got = rtcGetTrackDescription(rtc_track_id, sdp, sizeof(sdp));
    if (got <= 0) {
        fprintf(stderr, "rtcma: rtcGetTrackDescription returned %d on "
                        "track %d\n", got, rtc_track_id);
        return -1;
    }
    rtcma_opus_params_from_sdp(sdp, *pt_inout, params);

    if (channels_override != 0) params->channels = channels_override;

    if (params->channels < 1 || params->channels > 2) {
        fprintf(stderr, "rtcma: resolved channels=%d out of range\n",
                params->channels);
        return -1;
    }
    return 0;
}

int rtcma_recv_track_attach(RtcmaRecvTrack *t, int rtc_track_id,
                            int channels_override, int pt_override)
{
    if (!t || rtc_track_id < 0)        return -1;
    if (t->rtc_track_id >= 0)          return -1;  /* already attached */

    int             pt = pt_override;
    RtcmaOpusParams params;
    if (resolve_pt_and_params(rtc_track_id, &pt, channels_override,
                              &params) < 0)
        return -1;

    int err = 0;
    OpusDecoder *dec = opus_decoder_create(RTCMA_SAMPLE_RATE,
                                           params.channels, &err);
    if (!dec || err != OPUS_OK) {
        fprintf(stderr, "rtcma: opus_decoder_create failed: %d\n", err);
        if (dec) opus_decoder_destroy(dec);
        return -1;
    }

    fprintf(stderr,
            "rtcma: recv attached track=%d pt=%d ch=%d "
            "fec=%d maxplaybackrate=%d\n",
            rtc_track_id, pt, params.channels,
            params.useinbandfec, params.maxplaybackrate);

    rtcma_jitter_init(&t->jitter);

    /* Populate the fields on_track_message reads (payload_type for the
     * PT filter, jitter for the enqueue) BEFORE arming the callback.
     * Otherwise libdatachannel can fire the callback in the window
     * between rtcSetMessageCallback and the field assignments below,
     * with t->payload_type still zero from the calloc - parse_rtp would
     * then SKIP every packet whose PT != 0 (i.e., every real packet).
     *
     * rtc_track_id stays at -1 until the very end, because pull_pcm uses
     * it as the "fully attached" gate. */
    t->dec             = dec;
    t->payload_type    = pt;
    t->channels        = params.channels;

    /* User pointer must be set BEFORE the callback so on_track_message
     * never sees a NULL ptr. */
    rtcSetUserPointer(rtc_track_id, t);
    int rc = rtcSetMessageCallback(rtc_track_id, on_track_message);
    if (rc < 0) {
        fprintf(stderr, "rtcma: rtcSetMessageCallback failed: %d\n", rc);
        rtcSetUserPointer(rtc_track_id, NULL);
        t->dec          = NULL;
        t->payload_type = 0;
        t->channels     = 0;
        rtcma_jitter_destroy(&t->jitter);
        opus_decoder_destroy(dec);
        return -1;
    }
    rtcChainRtcpReceivingSession(rtc_track_id);

    t->rtc_track_id = rtc_track_id;
    return 0;
}

void rtcma_recv_track_detach(RtcmaRecvTrack *t)
{
    if (!t || t->rtc_track_id < 0) return;

    /* libdatachannel synchronises the callback slot: assigning NULL
     * takes the same recursive mutex held during dispatch, so this
     * blocks until any in-flight on_track_message returns. Once both
     * calls return, no future callback can fire - safe to tear down. */
    int id = t->rtc_track_id;
    t->rtc_track_id = -1;
    rtcSetMessageCallback(id, NULL);
    rtcSetUserPointer(id, NULL);

    if (t->dec) {
        opus_decoder_destroy(t->dec);
        t->dec = NULL;
    }
    rtcma_jitter_destroy(&t->jitter);
}

int rtcma_recv_track_pull_pcm(RtcmaRecvTrack *t, int16_t *pcm,
                              size_t pcm_capacity_samples)
{
    if (!t || !pcm) return -1;
    if (t->rtc_track_id < 0 || !t->dec) return 0;     /* not attached: silence */
    /* Caller must size the buffer for the worst-case opus frame (120 ms).
     * Real-world WebRTC stacks ship 20 ms most of the time but libwebrtc
     * switches to 40/60 ms under bandwidth pressure; without space for
     * the actual decoded frame opus_decode fails with OPUS_BUFFER_TOO_SMALL. */
    if (pcm_capacity_samples < (size_t)RTCMA_DECODE_MAX_SAMPLES * (size_t)t->channels)
        return -1;

    uint8_t opus_buf[RTCMA_JITTER_MAX_PAYLOAD];
    int     opus_len = 0;
    bool present = rtcma_jitter_get(&t->jitter, opus_buf, sizeof(opus_buf),
                                    &opus_len);

    if (present) {
        int decoded = opus_decode(t->dec, opus_buf, opus_len,
                                  pcm, RTCMA_DECODE_MAX_SAMPLES, 0);
        if (decoded < 0) {
            fprintf(stderr, "rtcma: opus_decode failed: %d\n", decoded);
            return -1;
        }
        return decoded;
    }

    /* Jitter empty. If we've ever started playing, run PLC for this
     * slot - opus extrapolates one nominal-duration frame from decoder
     * state. Sounds nicer than zero-fill for short gaps. Ask for 20 ms;
     * matching the typical packetisation keeps PLC pacing aligned with
     * the rest of the stream. If the jitter hasn't primed yet, return
     * 0 so the caller fills silence. */
    if (t->jitter.playing) {
        int decoded = opus_decode(t->dec, NULL, 0,
                                  pcm, RTCMA_FRAME_SAMPLES, 0);
        if (decoded < 0) {
            fprintf(stderr, "rtcma: opus_decode (PLC) failed: %d\n", decoded);
            return -1;
        }
        return decoded;
    }
    return 0;
}

/* -- Send side --------------------------------------------------------- */

/* Map a maxplaybackrate (Hz) onto the closest OPUS_BANDWIDTH_* enum.
 * Anything >= 48 kHz means no cap (the encoder's own default). */
static int max_bandwidth_for_rate(int rate_hz)
{
    if (rate_hz >=  48000) return OPUS_BANDWIDTH_FULLBAND;
    if (rate_hz >=  24000) return OPUS_BANDWIDTH_SUPERWIDEBAND;
    if (rate_hz >=  16000) return OPUS_BANDWIDTH_WIDEBAND;
    if (rate_hz >=  12000) return OPUS_BANDWIDTH_MEDIUMBAND;
    return OPUS_BANDWIDTH_NARROWBAND;
}

int rtcma_send_track_attach(RtcmaSendTrack *t, int rtc_track_id,
                            int channels_override, int pt_override)
{
    if (!t || rtc_track_id < 0)         return -1;
    if (t->rtc_track_id >= 0)           return -1;

    int             pt = pt_override;
    RtcmaOpusParams params;
    if (resolve_pt_and_params(rtc_track_id, &pt, channels_override,
                              &params) < 0)
        return -1;

    int err = 0;
    /* AUDIO (not VOIP) + auto signal detection: VOIP mode is heavily
     * tuned for speech (aggressive noise gating, SILK-only at low
     * rates) and produces audible artifacts on pure tones. AUDIO with
     * auto signal-type detection adapts per-frame between speech and
     * music modes - cleaner on both at the cost of slightly less
     * speech-per-bit efficiency. */
    OpusEncoder *enc = opus_encoder_create(RTCMA_SAMPLE_RATE,
                                           params.channels,
                                           OPUS_APPLICATION_AUDIO, &err);
    if (!enc || err != OPUS_OK) {
        fprintf(stderr, "rtcma: opus_encoder_create failed: %d\n", err);
        if (enc) opus_encoder_destroy(enc);
        return -1;
    }

    /* Bitrate ceiling: 128 kbps unless the peer's maxaveragebitrate
     * narrows us. RFC 7587 sec.6.1: value is total opus bits/sec. */
    int bitrate = 128000;
    if (params.maxaveragebitrate > 0 && params.maxaveragebitrate < bitrate)
        bitrate = params.maxaveragebitrate;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));

    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(params.channels));

    if (params.maxplaybackrate > 0) {
        opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(
            max_bandwidth_for_rate(params.maxplaybackrate)));
    }
    if (params.useinbandfec) {
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
        /* FEC only emits redundancy when the encoder *expects* loss.
         * 5 % matches what Chromium offers as its default and is a
         * conservative guess for unmanaged XMPP transport. */
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));
    }
    if (params.usedtx) {
        opus_encoder_ctl(enc, OPUS_SET_DTX(1));
    }
    if (params.cbr) {
        opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    }

    /* Generate SSRC, initial sequence number, and initial RTP timestamp
     * from CLOCK_MONOTONIC + this track's address. RFC 3550 sec.5.1 calls
     * for both initial values to be random for SRTP security; nothing
     * outside this process consumes them so monotonic-clock entropy is
     * plenty.
     *
     * We do NOT install rtcSetOpusPacketizer or rtcChainRtcpSrReporter.
     * The track keeps an empty media handler chain, so rtcSendMessage
     * -> Track::outgoing falls straight through to transportSend, which
     * SRTP-encrypts the bytes we hand it and ships them. The 12-byte
     * RFC 3550 header is built ourselves in rtcma_send_track_push_pcm
     * (see RtcmaSendTrack doc in rtcma_internal.h for the rationale). */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    uint32_t ssrc = (uint32_t)(ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 16) ^
                               (uintptr_t)t);
    if (ssrc == 0) ssrc = 0xC0FFEE;

    t->ssrc           = ssrc;
    t->next_seq       = (uint16_t)(ts.tv_nsec ^ ((uintptr_t)t >> 4));
    t->next_timestamp = (uint32_t)(ts.tv_nsec * 2654435761u) ^ ssrc;

    /* Log the libdatachannel-side direction. If this comes back as
     * RECVONLY/INACTIVE, Track::outgoing silently drops every message
     * (COUNTER_MEDIA_BAD_DIRECTION) and our wire stays silent - exactly
     * the symptom we'd otherwise mistake for "packets sent but peer
     * doesn't hear." 3 = SENDRECV is the expected value for a peer that
     * offered sendrecv. */
    rtcDirection dir = RTC_DIRECTION_UNKNOWN;
    int dir_rc = rtcGetTrackDirection(rtc_track_id, &dir);

    fprintf(stderr,
            "rtcma: send attached track=%d pt=%d ch=%d br=%d "
            "fec=%d dtx=%d cbr=%d maxbw=%d ssrc=%u "
            "direction=%d (rc=%d)\n",
            rtc_track_id, pt, params.channels, bitrate,
            params.useinbandfec, params.usedtx, params.cbr,
            params.maxplaybackrate, ssrc,
            (int)dir, dir_rc);

    atomic_store(&t->diag_send_logged, 0);
    atomic_store(&t->diag_send_ok,     0);
    atomic_store(&t->diag_send_fail,   0);
    atomic_store(&t->diag_last_rc,     0);
    pthread_mutex_init(&t->enc_lock, NULL);
    t->enc          = enc;
    t->channels     = params.channels;
    t->payload_type = pt;
    t->rtc_track_id = rtc_track_id;
    return 0;
}

void rtcma_send_track_detach(RtcmaSendTrack *t)
{
    if (!t || t->rtc_track_id < 0) return;
    t->rtc_track_id = -1;
    if (t->enc) {
        opus_encoder_destroy(t->enc);
        t->enc = NULL;
    }
    pthread_mutex_destroy(&t->enc_lock);
    t->ssrc = 0;
}

/* Build a 12-byte RFC 3550 RTP header (no CSRC list, no extensions, no
 * padding). version=2, marker=0 (RFC 7587 sec.4.5 - only set at start of a
 * talkspurt; rtc-ma doesn't do DTX-driven talkspurt detection, so plain
 * 0 matches what gajim/rtpopuspay emits in the steady state and what
 * libwebrtc audio receivers expect). All multi-byte fields are written
 * in network byte order. */
static void build_rtp_header(uint8_t *hdr, int pt,
                             uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    hdr[ 0] = 0x80;                            /* V=2, P=0, X=0, CC=0  */
    hdr[ 1] = (uint8_t)(pt & 0x7F);            /* M=0, PT              */
    hdr[ 2] = (uint8_t)(seq >> 8);
    hdr[ 3] = (uint8_t)(seq & 0xFF);
    hdr[ 4] = (uint8_t)(ts  >> 24);
    hdr[ 5] = (uint8_t)(ts  >> 16);
    hdr[ 6] = (uint8_t)(ts  >>  8);
    hdr[ 7] = (uint8_t)(ts  & 0xFF);
    hdr[ 8] = (uint8_t)(ssrc >> 24);
    hdr[ 9] = (uint8_t)(ssrc >> 16);
    hdr[10] = (uint8_t)(ssrc >>  8);
    hdr[11] = (uint8_t)(ssrc & 0xFF);
}

int rtcma_send_track_push_pcm(RtcmaSendTrack *t, const int16_t *pcm,
                              size_t samples_per_channel)
{
    if (!t || !pcm)                              return -1;
    if (t->rtc_track_id < 0 || !t->enc)          return -1;
    if (samples_per_channel != RTCMA_FRAME_SAMPLES) return -1;

    /* 12-byte RTP header + max Opus packet (1275 bytes at 48 kHz/20 ms,
     * round to 2000 to leave headroom). Single contiguous buffer so
     * rtcSendMessage / transportSend / srtp_protect see one message. */
    unsigned char pkt[12 + 2000];

    pthread_mutex_lock(&t->enc_lock);
    int enc_size = opus_encode(t->enc, pcm, (int)samples_per_channel,
                               pkt + 12, sizeof(pkt) - 12);
    pthread_mutex_unlock(&t->enc_lock);

    if (enc_size < 0) {
        fprintf(stderr, "rtcma: opus_encode failed: %d\n", enc_size);
        return -1;
    }

    build_rtp_header(pkt, t->payload_type,
                     t->next_seq, t->next_timestamp, t->ssrc);

    int rc = rtcSendMessage(t->rtc_track_id,
                            (const char *)pkt, 12 + enc_size);

    /* Advance only on success: keeps the wire stream gapless (no seq
     * holes) during DTLS warmup, when rtcSendMessage returns -2 for
     * the first ~10 frames. Once DTLS is up the receiver sees a
     * continuous seq + timestamp progression starting at the
     * randomised initial values. */
    if (rc >= 0) {
        t->next_seq       += 1;
        t->next_timestamp += RTCMA_FRAME_SAMPLES;
    }

    /* Bookkeeping for the detach summary. */
    if (rc >= 0)
        atomic_fetch_add(&t->diag_send_ok, 1);
    else
        atomic_fetch_add(&t->diag_send_fail, 1);

    /* Log: first 4 calls verbosely, then any change in rc (transition
     * fail -> ok or vice versa), then periodic samples every 250 calls.
     * This catches both "send never works" and "send eventually works
     * after DTLS but stops again later." */
    int n        = atomic_fetch_add(&t->diag_send_logged, 1);
    int last_rc  = atomic_exchange(&t->diag_last_rc, rc);
    bool changed = (n > 0) && ((last_rc < 0) != (rc < 0));
    bool sample  = (n % 250) == 0;
    if (n < 4 || changed || sample) {
        fprintf(stderr,
                "rtcma: send #%d track=%d opus_size=%d "
                "rtcSendMessage_rc=%d%s\n",
                n, t->rtc_track_id, enc_size, rc,
                changed ? "  <- state-change" : "");
    }

    return (rc >= 0) ? 0 : -1;
}
