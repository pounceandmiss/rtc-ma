# Tcl-level round-trip test for ::rtcma::enumerate-devices ids.
#
# Loads rtcma.so by absolute path (passed as argv 0) - bypasses
# `package require` so a stale install on auto_path can't shadow the
# fresh build. Exercises the glue around Tcl_NewStringObj / Tcl_GetString
# that the C unit tests can't reach.
#
# Tagged AUDIBLE in CMakeLists because player::new opens a real
# ma_device.

if {[llength $argv] < 1} {
    puts stderr "usage: tclsh test_tcl_device_id.tcl <path-to-rtcma.so>"
    exit 2
}
load [lindex $argv 0] Rtcma

set failures 0
proc fail {msg} {
    global failures
    puts stderr "FAIL: $msg"
    incr failures
}

# 1. Every id from enumerate-devices is a non-empty 7-bit ASCII string
#    matching <tag>:<payload>. Catches the old "256-byte bytearray with
#    NUL padding" failure mode at the Tcl boundary.
set devs [::rtcma::enumerate-devices]
set all  [concat [dict get $devs playback] [dict get $devs capture]]
if {[llength $all] == 0} {
    puts stderr "skip: no devices reported (no audio backend?)"
    exit 0
}
foreach entry $all {
    set id   [dict get $entry id]
    set name [dict get $entry name]
    if {$id eq ""} { fail "empty id for '$name'" ; continue }
    if {![regexp {^[a-z][a-z0-9]*:} $id]} {
        fail "id for '$name' missing <tag>: prefix: '$id'"
    }
    # No NUL bytes, no high-bit bytes - would have indicated the binding
    # was leaking the raw ma_device_id union.
    set n [string length $id]
    for {set i 0} {$i < $n} {incr i} {
        scan [string index $id $i] %c c
        if {$c < 0x20 || $c > 0x7e} {
            fail "id for '$name' has non-printable byte 0x[format %02x $c] at $i"
            break
        }
    }
}

# 2. Round-trip the default playback id through player::new / reopen.
#    Validates that -device-id accepts the same string it was handed.
set pb_default ""
foreach entry [dict get $devs playback] {
    if {[dict get $entry default]} {
        set pb_default [dict get $entry id]
        break
    }
}
if {$pb_default ne ""} {
    set p [::rtcma::player::new -channels 2 -device-id $pb_default]
    ::rtcma::player::reopen $p -device-id ""
    ::rtcma::player::reopen $p -device-id $pb_default
    ::rtcma::player::destroy $p
} else {
    puts stderr "note: no default playback device - skipping round-trip"
}

# 3. Same for the default capture id.
set cap_default ""
foreach entry [dict get $devs capture] {
    if {[dict get $entry default]} {
        set cap_default [dict get $entry id]
        break
    }
}
if {$cap_default ne ""} {
    set c [::rtcma::capturer::new -channels 1 -device-id $cap_default]
    ::rtcma::capturer::reopen $c -device-id ""
    ::rtcma::capturer::reopen $c -device-id $cap_default
    ::rtcma::capturer::destroy $c
}

# 4. Cross-backend rejection. "rtcma:" is not a real backend tag, so
#    id_from_string must refuse - independent of backend permissiveness
#    (Pulse used to silently fall back to default for unrecognised
#    blobs; the tag check catches it before miniaudio sees it).
if {![catch {::rtcma::player::new -channels 2 -device-id "rtcma:bogus"} h]} {
    fail "expected ::rtcma::player::new to reject 'rtcma:bogus' but got handle $h"
    catch {::rtcma::player::destroy $h}
}

# 5. Empty string == system default.
set p [::rtcma::player::new -channels 2 -device-id ""]
::rtcma::player::destroy $p

if {$failures > 0} {
    puts stderr "tcl device-id test: $failures failure(s)"
    exit 1
}
puts "tcl device-id test ok."
exit 0
