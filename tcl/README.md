# ::rtcma:: - Tcl 9 bindings for rtc-ma

A thin Tcl 9 wrapper over `include/rtcma.h`. Configuration and
lifecycle only - the data path stays entirely C-side, so there are no
Tcl trampolines on the audio or RTP hot path.

## Build

```sh
cmake -S . -B build -DRTCMA_BUILD_TCL=ON -G Ninja
cmake --build build
```

Produces:

- `build/tcl/rtcma.so` - loadable module for `package require rtcma`.
- `build/tcl/pkgIndex.tcl` - drop on `auto_path` or `TCLLIBPATH`.
- `build/tcl/librtcma_tcl.a` - static archive for embedders linking
  into a custom `tclsh` (call `Rtcma_Init` directly).

```sh
TCLLIBPATH=$PWD/build/tcl tclsh
% package require rtcma
0.1.0
```

## Handles

Commands return int handles, mirroring libdatachannel-tcl's
convention so handles can be passed around like `::rtc::*` track ids.
Two kinds: player handles and capturer handles. The binding
type-checks on lookup, so passing a player handle to a capturer
command raises `RTCMA invalid-handle`.

## Error convention

Every command that maps to a `0/-1` C return:

- Returns `0` as the result on success.
- On failure raises a Tcl error with result `"rtcma error"` and
  `::errorCode {RTCMA failure}`. Wrap in `catch` if you need to
  recover.

Lookups raise `::errorCode {RTCMA invalid-handle}`.

## Commands

### `::rtcma::enumerate-devices`

Returns a dict `{playback {<entry>...} capture {<entry>...}}` where
each entry is itself a dict:

- `name` (string) - Human-readable, from the active miniaudio backend.
- `default` (bool) - The system default for this direction.
- `id` (string) - Persistable `backend:payload` identifier (see below).

The `id` is plain 7-bit ASCII in the form `<backend>:<payload>`, where
`backend` is the active miniaudio backend (`alsa`, `pulse`,
`coreaudio`, `wasapi`, `dsound`, `winmm`, ...) and `payload` is that
backend's native identifier - a C string for ALSA/Pulse/CoreAudio, a
hex GUID for DirectSound, a decimal integer for WinMM/AAudio, UTF-8
for WASAPI. The string round-trips through JSON, pipes, and SQLite
TEXT without escaping, and equality across enumerations is plain
string equality (matches `ma_device_id_equal`).

Pass an `id` from the dict back as `-device-id` on
`::rtcma::player::new` / `::rtcma::capturer::new` / their `reopen`
commands. Persisted ids stay valid across processes and rtc-ma
restarts as long as the same backend is in use; restoring an id with
a different backend tag fails at device-init time.

### `::rtcma::player::*`

Inbound RTP on a track -> speakers. Owns its own playback audio device.

- `::rtcma::player::new ?opts?` - Open a playback device and allocate
  state. Options: `-channels` (1 or 2; default 2), `-device-id STRING`,
  `-payload-type` (0 = auto from SDP). Returns a handle.
- `::rtcma::player::start $p` - Start the audio stream. Idempotent.
- `::rtcma::player::reopen $p ?-device-id STRING?` - Hot-swap the
  endpoint. Empty/omitted string = system default. Test-then-swap: on
  failure the old device stays running.
- `::rtcma::player::attach $p $rtc_track` - Bind to a libdatachannel
  track id. Installs `rtcSetMessageCallback`, auto-detects PT and
  decoder channels from SDP.
- `::rtcma::player::detach $p` - Clear callback + user pointer. Blocks
  until any in-flight callback returns.
- `::rtcma::player::destroy $p` - Implicit detach + free. Invalidates
  the handle.

### `::rtcma::capturer::*`

Mic -> outbound RTP via `rtcSendMessage`. Owns its own capture audio
device. Never installs any libdatachannel callback.

- `::rtcma::capturer::new ?opts?` - Open a capture device and allocate
  state. Options: `-channels` (0, 1, or 2; default 0 = mono),
  `-device-id STRING`, `-payload-type` (0 = auto from SDP). Returns a
  handle.
- `::rtcma::capturer::start $c` - Start the audio stream. Idempotent.
- `::rtcma::capturer::reopen $c ?-device-id STRING?` - Hot-swap the
  endpoint. Empty/omitted string = system default.
- `::rtcma::capturer::attach $c $rtc_track` - Bind to a libdatachannel
  track id. Creates an Opus encoder.
- `::rtcma::capturer::detach $c` - Unbind. Captured audio is dropped.
- `::rtcma::capturer::destroy $c` - Implicit detach + free.

## Device selection - picking specific endpoints

```tcl
package require rtcma

set devs [::rtcma::enumerate-devices]

# Pick a playback endpoint by name.
set pb_id ""
foreach entry [dict get $devs playback] {
    if {[dict get $entry name] eq "USB Headphones"} {
        set pb_id [dict get $entry id]
        break
    }
}

# Pick the default mic.
set cap_id ""
foreach entry [dict get $devs capture] {
    if {[dict get $entry default]} {
        set cap_id [dict get $entry id]
        break
    }
}

# Pin at construction.
set p [::rtcma::player::new   -channels 2 -device-id $pb_id]
set c [::rtcma::capturer::new -channels 2 -device-id $cap_id]
```

Omit `-device-id` (or pass an empty string) to use the system default
for that direction. The Player and Capturer open independent hardware
streams - see the top-level README's "Same-card caveat" if you're on
bare ALSA with both sides pinned to the same `hw:N,M`.

## Hot-swap mid-call

```tcl
# Switch playback to whatever the user just picked in a settings UI.
::rtcma::player::reopen $p -device-id $new_pb_id

# Fall back to system default (e.g. headphones unplugged).
::rtcma::player::reopen $p -device-id ""
```

Reopen is test-then-swap: if the new device fails to init, the old
one keeps running and the command raises an error. The player /
capturer remains valid and bound either way.

## End-to-end shape

The rtcma side, with the libdatachannel-tcl side sketched in pseudocode
- see [`../../libdatachannel-tcl/README.md`](../../libdatachannel-tcl/README.md)
for its actual command surface (`::rtc::pc::*`, `::rtc::track::*`, etc.).

```tcl
package require rtc       ;# libdatachannel-tcl
package require rtcma

# ... set up a peer connection and an outgoing Opus track via
# ::rtc::pc::new / ::rtc::pc::add-track / etc. ...
set pc      ...
set tr_out  ...

# Outbound mic -> rtc track.
set c [::rtcma::capturer::new -channels 2]
::rtcma::capturer::attach $c $tr_out
::rtcma::capturer::start  $c

# Inbound - bind a Player whenever a track arrives.
set p [::rtcma::player::new -channels 2]
::rtc::pc::on-track $pc [list apply {{p tr} {
    ::rtcma::player::attach $p $tr
    ::rtcma::player::start  $p
}} $p]

# ... drive offer/answer via libdatachannel-tcl as usual ...

# Teardown: detach rtcma handles BEFORE deleting the libdatachannel
# tracks. ::rtcma::*::destroy implicitly detaches.
::rtcma::capturer::destroy $c
::rtcma::player::destroy   $p
::rtc::pc::delete $pc
```

## libdatachannel-tcl coexistence

`::rtcma::player::attach` installs the C-level
`rtcSetMessageCallback` handler, replacing whatever was there (e.g. a
Tcl trampoline previously set via `::rtc::*::set-message-callback`).
Detach sets the slot to `NULL` - it does **not** restore the previous
callback. If you want Tcl-level message handling on the track after
detach, re-call `::rtc::*::set-message-callback` yourself.

The per-track user pointer (`rtcSet/GetUserPointer`) is used by the
Player while attached. libdatachannel-tcl leaves it free.