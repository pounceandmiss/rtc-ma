# rtc-ma - libdatachannel miniaudio adapter

A small C library that binds a libdatachannel track handle (the `int` id
from `rtcAddTrackEx` or `rtcSetTrackCallback`) to local audio hardware
via [miniaudio](https://miniaud.io/). Inbound RTP becomes speaker
playback; mic input becomes outbound RTP. Opus only.


## Integration shape

```c
#include <rtc/rtc.h>
#include <rtcma.h>

/* 1. You drive libdatachannel as normal. */
int pc = rtcCreatePeerConnection(&cfg);
/* ... rtcSetLocalDescription, rtcSetRemoteDescription, etc. ... */

/* 2. Bind a Player to the inbound track. The Player owns its own
 *    playback audio device; PT auto-detects from the track's SDP. */
void on_track(int pc, int tr, void *user) {
    RtcmaPlayer *p = rtcma_player_new(&(RtcmaPlayerConfig){ .channels = 2 });
    rtcma_player_attach(p, tr);          /* installs rtcSetMessageCallback */
    rtcma_player_start(p);
    /* keep p somewhere; rtcma_player_destroy before rtcDeleteTrack(tr) */
}

/* 3. Bind a Capturer to your outgoing track. The Capturer owns its
 *    own capture audio device. Do NOT call rtcSetOpusPacketizer -
 *    rtc-ma builds RFC 3550 RTP headers itself and ships them with
 *    no media handler installed on the track. */
int track_a = rtcAddTrackEx(pc, &tinit);
RtcmaCapturer *c = rtcma_capturer_new(&(RtcmaCapturerConfig){ .channels = 2 });
rtcma_capturer_attach(c, track_a);
rtcma_capturer_start(c);
```

A sendrecv track can have both a Player and a Capturer attached - the
Player owns the recv callback, the Capturer only sends. They don't
conflict.

## Public API

Everything is in `include/rtcma.h`. 

- `RtcmaPlayer` - inbound RTP -> speaker. Owns a playback audio device.
  Installs `rtcSetMessageCallback` on attach, NULL on detach. Hot-swap
  the underlying hardware mid-call with `rtcma_player_reopen`.
- `RtcmaCapturer` - mic -> outbound RTP. Owns a capture audio device.
  No callback slot used. Hot-swap with `rtcma_capturer_reopen`.
- `rtcma_enumerate_devices` returns playback and capture endpoints
  separately; pass the opaque `id` blob back as `device_id` to pin a
  specific endpoint.

## Lifecycle

libdatachannel owns the tracks. You must detach (or destroy) every
Player and Capturer bound to a track *before* `rtcDeleteTrack` on that
handle. `rtcma_player_detach` / `rtcma_capturer_detach` block until any
in-flight callback returns, so freeing state immediately after detach
is safe.

## SDP sanitization (consumer's job)

rtc-ma builds plain RFC 3550 RTP headers on the send side - no header
extensions, no transport-cc feedback, no RTCP
SR/SDES. libdatachannel's auto-generated answer SDP unconditionally
echoes every `extmap` and `rtcp-fb` line from the remote offer, so
when the peer is e.g. libwebrtc the negotiated SDP advertises
capabilities rtc-ma cannot fulfill. DTLS-SRTP completes, packets reach
the receiver, **no audio plays**, and no RTCP RR ever mentions our
SSRC.

Before shipping the local description to the remote peer, strip these
lines from the audio m-section:

```
a=extmap:* ...
a=extmap-allow-mixed
a=rtcp-fb:<PT> transport-cc
```

Applies in both offerer and answerer roles.

## SDP fmtp parameters honoured

A handful of opus `fmtp` parameters are read at
`rtcma_capturer_attach` and applied to the encoder via
`opus_encoder_ctl`, so the Capturer honours whatever the peer
negotiated:

- `sprop-stereo` - encoder channel count (1 or 2). Independent of the
  audio device channel count; the capture callback mixes between the
  two. Also drives the Player's decoder channel count.
- `maxaveragebitrate` - clamps the 128 kbps default bitrate ceiling.
- `maxplaybackrate` - clamps `OPUS_SET_MAX_BANDWIDTH`
  (NB/MB/WB/SWB/FB). Does not change the 48 kHz wire rate or the
  encoder's PCM rate - it's a bandwidth ceiling, not a sample-rate
  override.
- `useinbandfec` - `OPUS_SET_INBAND_FEC` + 5% packet-loss hint.
- `usedtx` - `OPUS_SET_DTX`.
- `cbr` - `OPUS_SET_VBR(0)`.

## Same-card caveat (bare ALSA)

Player and Capturer open independent hardware streams. On PulseAudio,
PipeWire, and ALSA `dmix`/`plug` (the defaults on every modern Linux
distro) this is transparent - two streams coexist on one card. On
bare-metal ALSA with both sides pinned to the same `hw:N,M`, one of
the opens will fail at construction or reopen time: `hw:` devices are
exclusive. Prefer the `default` enumeration entry or a `plug:` alias
(both shareable through `dmix`), or pick distinct cards. Picking
endpoints from `rtcma_enumerate_devices` on a stock system gets you
shareable aliases - the conflict only happens if you hand-construct a
raw `hw:` id.

## Tcl bindings

Optional Tcl 9 bindings expose the same surface. Build with
`-DRTCMA_BUILD_TCL=ON`. See [`tcl/README.md`](tcl/README.md) for the
command reference and device-selection examples.

## libdatachannel-tcl coexistence

rtc-ma uses the per-track user pointer (`rtcSet/GetUserPointer`); `rtcma_player_attach` replaces any previously-installed `rtcSetMessageCallback` (e.g. a Tcl trampoline); detach sets the slot to NULL - it does *not* restore a previous callback.

## Threading

- libdatachannel worker threads run the message callback: RTP parse +
  jitter `put`.
- Each Player and Capturer runs its own miniaudio data callback on a
  per-device thread. Player thread does jitter `get` + opus decode +
  speaker write; Capturer thread does mic read + opus encode +
  `rtcSendMessage`. The two callback threads are independent - the
  hardware clock on each side is the clock for that direction.
- No decoder/encoder threads, no timers. That's deliberate.
  Decoupling decode from the audio clock produced audible cracking.
- Public functions on a given handle are single-threaded.

## Building

System deps:

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Needs `libdatachannel`, `opus`, and `pkg-config` installed. 

Fully static (no runtime `.so` deps beyond glibc / libgcc_s):

```sh
cmake -S . -B build-static \
  -DRTCMA_BUNDLE_LIBDATACHANNEL=ON -DRTCMA_BUNDLE_OPUS=ON -G Ninja
cmake --build build-static
```

Builds pinned mbedtls + libdatachannel + opus from source under
`build-static/vendor/` and folds them (plus `-static-libstdc++`, scoped
to the bundled libdatachannel target) into `librtcma.a`. The first
configure clones the source trees, so allow a minute or two.

The two flags are independent - set only one to bundle a single
dependency while consuming the other from the system / a
`CMAKE_PREFIX_PATH`-provided install. For example, bundle just opus
while linking against an existing libdatachannel install:

```sh
cmake -S . -B build-bundle-opus \
  -DRTCMA_BUNDLE_OPUS=ON \
  -DCMAKE_PREFIX_PATH=/path/to/libdatachannel-install -G Ninja
```

`-DRTCMA_BUNDLE_DEPS=ON` is kept as a deprecated alias that turns on
both finer flags when neither has been set explicitly.

## Tests

```sh
# CI-safe
ctest --test-dir build --label-exclude audible
# full set, will screech, needs working audio backend
ctest --test-dir build --output-on-failure  
```