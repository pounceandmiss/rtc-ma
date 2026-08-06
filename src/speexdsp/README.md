# Vendored speexdsp jitter buffer

Source: speexdsp 1.2.1, https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz
sha256 `8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d`

Only the jitter buffer is taken. It is codec-agnostic (opaque payload plus a
timestamp and a span) and pulls in nothing else from speexdsp: no FFT, no
resampler, no echo canceller. The files are verbatim from the release except
`speex/speexdsp_config_types.h`, which upstream generates with configure.

| File | Origin in the release |
| --- | --- |
| `jitter.c` | `libspeexdsp/jitter.c` |
| `arch.h` | `libspeexdsp/arch.h` |
| `os_support.h` | `libspeexdsp/os_support.h` |
| `speex/speex_jitter.h` | `include/speex/speex_jitter.h` |
| `speex/speexdsp_types.h` | `include/speex/speexdsp_types.h` |
| `speex/speexdsp_config_types.h` | hand-written, see the file |
| `COPYING` | `COPYING` (3-clause BSD) |

Built with `FLOATING_POINT`, `EXPORT=` and `DISABLE_WARNINGS`, and without
`HAVE_CONFIG_H` so no generated `config.h` is needed. `DISABLE_WARNINGS`
matters: four `fprintf(stderr)` calls are reachable from the playback path,
and that path runs on the audio device thread.

`rtcma_jitter.c` is the adapter. It drives the buffer in RTP timestamp units
and hands it a no-op destroy callback, which puts it in zero-copy mode and
keeps `jitter_buffer_put` and `jitter_buffer_get` free of any allocation.

To update: copy the files from a new release (there are no local patches to
carry forward) and check `jitter_buffer_get`'s return codes and the
`JitterBufferPacket` layout against `rtcma_jitter.c`.
