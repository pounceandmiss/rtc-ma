/* rtcma.h - libdatachannel media adapter.
 *
 * Bind a libdatachannel track handle (the int id from rtcAddTrackEx or
 * delivered to the consumer via rtcSetTrackCallback) to local audio
 * hardware. No SDP, no signalling, no peer-connection management - the
 * consumer drives all of that through libdatachannel directly.
 *
 * Audio pipeline is fixed at 48000 Hz / 20 ms Opus frames.
 *
 * -- Threading ---------------------------------------------------------
 *  libdatachannel callbacks fire on its worker threads. The adapter
 *  parses RTP and pushes Opus payload into a lock-protected jitter ring
 *  there.
 *
 *  Each Player owns a playback-only audio device; each Capturer owns a
 *  capture-only audio device. The two run on independent callback
 *  threads with independent hardware clocks. The Player callback pulls
 *  one 20 ms frame from the jitter ring, opus-decodes it (with PLC on
 *  miss), and writes to the output buffer; the audio device is the
 *  playout clock, which is the only thing that prevents audible
 *  cracking. The Capturer callback receives 20 ms PCM, opus-encodes,
 *  and calls rtcSendMessage. Do NOT add timers or worker threads to
 *  either path.
 *
 *  Public functions are single-threaded per handle. Concurrent calls
 *  on the same RtcmaPlayer / RtcmaCapturer are undefined.
 *
 * -- Lifecycle ---------------------------------------------------------
 *  libdatachannel owns each rtc track. Detach (or destroy) every Player
 *  and Capturer bound to a track BEFORE rtcDeleteTrack on that handle -
 *  otherwise the installed message callback fires against freed state.
 *  rtcma_player_destroy / rtcma_capturer_destroy implicitly detach.
 *
 * -- Same-card caveat --------------------------------------------------
 *  Player and Capturer open separate hardware streams. On PulseAudio,
 *  PipeWire, and ALSA dmix/plug (the defaults on every modern Linux
 *  distro) this is transparent. On bare-metal ALSA with both sides
 *  pinned to the same hw:N,M, one of the opens will fail - hw:
 *  devices are exclusive. Use a plug: alias or pick distinct cards.
 *
 * -- libdatachannel callback-slot caveat -------------------------------
 *  rtcSet*Callback slots are last-writer-wins. rtcma_player_attach
 *  installs the adapter's rtcSetMessageCallback handler, replacing any
 *  previously registered one (e.g. a Tcl trampoline from
 *  libdatachannel-tcl's runtime). rtcma_player_detach sets the slot
 *  back to NULL - it does NOT restore a previously-installed callback.
 *  A consumer that wants Tcl-level message handling after detach must
 *  re-call ::rtc::*::set-message-callback itself.
 *
 *  The libdatachannel user pointer (rtcSet/GetUserPointer) IS used by
 *  the Player on the track id while attached. The libdatachannel-tcl
 *  runtime intentionally leaves it free for exactly this case. Do not
 *  write the user pointer yourself between attach and detach. The
 *  Capturer does not use the user pointer.
 *
 *  A sendrecv track can have both a Player and a Capturer attached: the
 *  Player installs the message callback (recv), the Capturer only ever
 *  calls rtcSendMessage (no callback install). They do not conflict.
 *
 * -- SDP sanitization (signaling layer's responsibility) --------------
 *  rtc-ma builds plain RFC 3550 RTP headers on the send side - no RTP
 *  header extensions, no transport-cc feedback, no RTCP SR/SDES.
 *
 *  libdatachannel's auto-generated answer SDP unconditionally echoes
 *  every extmap from the remote offer (description.cpp:808 in the
 *  bundled version - no capability-aware filtering exists yet). When
 *  the negotiated SDP advertises extensions and transport-cc feedback
 *  that rtc-ma cannot fulfill, libwebrtc-based peers silently drop our
 *  stream: DTLS-SRTP completes, packets reach the receiver, no audio
 *  plays, no RTCP RR ever mentions our SSRC.
 *
 *  Consumers MUST strip these lines from the local description before
 *  shipping it to the remote peer (Jingle / SIP / whatever):
 *
 *      a=extmap:* ...
 *      a=extmap-allow-mixed
 *      a=rtcp-fb:<PT> transport-cc
 *
 *  Applies in both offerer and answerer roles - the extensions are
 *  equally toxic in either direction because rtc-ma never emits them.
 * ------------------------------------------------------------------- */

#ifndef RTCMA_H
#define RTCMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTCMA_SAMPLE_RATE    48000
#define RTCMA_FRAME_MS       20
#define RTCMA_FRAME_SAMPLES  960  /* RTCMA_SAMPLE_RATE * RTCMA_FRAME_MS / 1000 */

typedef struct RtcmaPlayer   RtcmaPlayer;
typedef struct RtcmaCapturer RtcmaCapturer;

/* -- Device enumeration ----------------------------------------------- */

typedef struct {
    char        name[256];
    bool        is_default;
    /* Persistable ASCII identifier in the form "backend:payload". The
     * backend tag is the active miniaudio backend (alsa, pulse,
     * coreaudio, wasapi, dsound, winmm, ...) and the payload is that
     * backend's native identifier: a C string for ALSA/Pulse/CoreAudio,
     * a hex GUID for DirectSound, a decimal integer for WinMM/AAudio,
     * UTF-8 for WASAPI. 7-bit-safe - round-trips through JSON, pipes,
     * SQLite TEXT without escaping. Heap-allocated; freed by
     * rtcma_device_list_free. */
    const char *id;
} RtcmaAudioDevice;

typedef struct {
    RtcmaAudioDevice *playback;
    int               playback_count;
    RtcmaAudioDevice *capture;
    int               capture_count;
} RtcmaDeviceList;

/* Populate `out` with the playback and capture endpoints the active
 * miniaudio backend currently sees. Returns 0 on success, -1 on failure
 * (out is left zeroed). Free with rtcma_device_list_free. */
int  rtcma_enumerate_devices(RtcmaDeviceList *out);
void rtcma_device_list_free(RtcmaDeviceList *list);

/* -- Player: inbound RTP on a track -> speakers ----------------------- */

typedef struct {
    /* Output channel count of the audio device. 1 or 2; default 2.
     * If the track's Opus stream decodes to a different channel count
     * (per SDP fmtp sprop-stereo) the Player up/downmixes. */
    int         channels;
    /* Optional explicit playback endpoint. NULL or empty string = system
     * default. Format is "backend:payload" - see RtcmaAudioDevice.id.
     * The string is parsed and copied before the call returns, so the
     * RtcmaDeviceList may be freed once rtcma_player_new returns. If
     * the backend tag does not match the active backend, init fails. */
    const char *device_id;
    /* If 0, payload type is auto-detected via
     * rtcGetTrackPayloadTypesForCodec at attach. */
    int         payload_type;
} RtcmaPlayerConfig;

/* Open the underlying audio device. Returns NULL on init failure. */
RtcmaPlayer *rtcma_player_new(const RtcmaPlayerConfig *cfg);
void         rtcma_player_destroy(RtcmaPlayer *p);  /* implicit detach */

/* Start the underlying audio stream. Idempotent. Returns 0 / -1. */
int rtcma_player_start(RtcmaPlayer *p);

/* Hot-swap the playback endpoint. NULL or empty string = system
 * default. Channel count is preserved. Any bound track stays bound.
 *
 * Test-then-swap: the new audio device is initialised first. On init
 * failure the old device keeps running and the call returns -1 (Player
 * unchanged). If init succeeds but start fails (rare), the new device
 * is installed but stopped - the Player is still valid, just silent;
 * the caller may retry rtcma_player_start or call reopen again. */
int rtcma_player_reopen(RtcmaPlayer *p, const char *device_id);

/* Bind the Player to rtc_track:
 *   - resolves PT (config override or rtcGetTrackPayloadTypesForCodec)
 *     and decoder channel count (always SDP fmtp sprop-stereo);
 *   - creates an Opus decoder;
 *   - installs the adapter's rtcSetMessageCallback handler;
 *   - calls rtcChainRtcpReceivingSession so RTCP receiver reports flow;
 *   - stores the Player in the track's user pointer.
 *
 * Returns 0 on success. Returns -1 on failure, including the case
 * where rtcGetTrackDescription or rtcGetTrackPayloadTypesForCodec
 * returned <= 0. The callback slot is left as it was before the failed
 * attach. */
int rtcma_player_attach(RtcmaPlayer *p, int rtc_track);

/* Set the track's message callback to NULL and clear its user pointer.
 * Blocks until any in-flight message callback returns (libdatachannel
 * synchronises the slot), so freeing decoder / jitter state immediately
 * after is safe. Safe to call when not attached. Returns 0. */
int rtcma_player_detach(RtcmaPlayer *p);

/* -- Capturer: mic -> rtcSendMessage on a track ----------------------- */

typedef struct {
    /* Capture-device channel count. 1, 2, or 0 (default = 1, matching
     * a mono headset/mic and the mono opus offers most XMPP/Jingle and
     * SIP softphones send). The opus encoder's channel count is
     * independent and is derived from SDP fmtp sprop-stereo at attach;
     * the capture callback mixes between device and encoder channels
     * when they differ. */
    int         channels;
    /* Optional explicit capture endpoint. NULL or empty string = system
     * default. Format is "backend:payload" - see RtcmaAudioDevice.id. */
    const char *device_id;
    /* Override the libdatachannel-resolved opus payload type. Default
     * 0 = pick whatever rtcGetTrackPayloadTypesForCodec returns at
     * attach. Set non-zero only for diagnostic pinning. */
    int         payload_type;
} RtcmaCapturerConfig;

/* Open the underlying audio device. Returns NULL on init failure. */
RtcmaCapturer *rtcma_capturer_new(const RtcmaCapturerConfig *cfg);
void           rtcma_capturer_destroy(RtcmaCapturer *c);  /* implicit detach */

int rtcma_capturer_start(RtcmaCapturer *c);
int rtcma_capturer_reopen(RtcmaCapturer *c, const char *device_id);

/* Bind the Capturer to rtc_track:
 *   - resolves PT (config override or rtcGetTrackPayloadTypesForCodec)
 *     and encoder channel count (always SDP fmtp sprop-stereo);
 *   - reads the rest of the opus fmtp parameters (useinbandfec, usedtx,
 *     cbr, maxaveragebitrate, maxplaybackrate) and applies the
 *     corresponding opus_encoder_ctl calls so we honour whatever the
 *     peer negotiated;
 *   - creates an Opus encoder for the resolved channels.
 *
 * Does NOT install any libdatachannel callback - the Capturer only
 * ever calls rtcSendMessage. Returns 0 on success, -1 on failure. */
int rtcma_capturer_attach(RtcmaCapturer *c, int rtc_track);

/* Unbind. After detach the Capturer holds no track id and audio
 * captured by the device is dropped. Returns 0. */
int rtcma_capturer_detach(RtcmaCapturer *c);

#ifdef __cplusplus
}
#endif

#endif /* RTCMA_H */
