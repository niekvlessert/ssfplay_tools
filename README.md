# ssfplay

`ssfplay` is an embeddable C library plus command-line player/converters for
Sega Saturn Sound Format (`.ssf` and `.minissf`) music. It packages Mednafen
1.32.1's reduced SSF playback core without SDL, video, or the full Saturn
emulator.

The library is GPLv2-or-later because it derives from Mednafen.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ssfplay_all_tools
ctest --test-dir build --output-on-failure
```

The default build uses a static `ssfplay` core and creates three standalone
tools: `ssfplay_cli`, `ssf2wav`, and `ssf2vgm`. Set
`-DSSFPLAY_BUILD_SHARED=ON` only when you also want an embeddable shared
library. Set `-DSSFPLAY_BUILD_CLI=OFF` when only the library is needed.

Default output quality is resampler quality `10` for the player, converters,
and library callers that use `ssfplay_config_init()`.

## Play SSF

```sh
build/ssfplay_cli "Nights Into Dreams (EMU).zophar/25 Twin Seeds - Growing Wings.ssf"
build/ssfplay_cli --quality 10 --rate 44100 input.ssf
```

`ssfplay_cli` uses WinMM on Windows, AudioQueue on macOS, and ALSA on Linux
when ALSA development headers are available at build time.

## Convert to WAV

```sh
build/ssf2wav "Nights Into Dreams (EMU).zophar/25 Twin Seeds - Growing Wings.ssf" twin-seeds.wav
build/ssf2wav --rate 48000 --quality 8 --length-ms 30000 --fade-ms 5000 input.ssf output.wav
```

By default, `ssf2wav` honors the SSF `length` and `fade` tags. Output is
interleaved signed 16-bit stereo PCM.

## Capture native SCSP VGM

```sh
build/ssf2vgm input.ssf output.vgm
build/ssf2vgm input.ssf output.vgz
build/ssf2vgm --length-ms 30000 --report report.json input.ssf output.vgm
```

`ssf2vgm` writes uncompressed VGM v1.71 using native SCSP register commands and
RAM blocks. It captures the tagged track length without the fade. The JSON
report inventories observed SCSP features and compares direct SSF PCM with a
Mednafen replay of the serialized event stream.

Use a `.vgz` output filename for lossless gzip compression. VGZ contains the
complete VGM stream and is supported by libvgm and most VGM players. It is
strongly recommended for SCSP captures; a representative one-second NiGHTS
capture shrinks from about 1.9 MB to 246 KB.

SCSP RAM is also the sound CPU's working memory, so faithful files can be
large. Atomic 16-bit bus writes must be represented as two ordered VGM byte
writes; the report records this warning and any resulting waveform difference.
The local `libvgm` source tree is not required to build `ssfplay`, but its
`vgm2wav` utility is useful for compatibility checks.

Capture only reaches behavior executed by the SSF. It cannot recover sounds
missing from the rip, assets loaded from disc, gameplay/SH-2-triggered sounds,
CD-DA or external SCSP input, or dormant sounds whose trigger protocol is
unknown.

The automated all-track smoke test intentionally writes one-second files named
`*.smoke-1s.vgm` under `build/smoke-test-vgm-1s`. These are validation
artifacts, not completed track conversions. Run `ssf2vgm` without
`--length-ms` to use each SSF's tagged duration.

## Embed

```c
#include <ssfplay/ssfplay.h>

ssfplay_decoder* decoder = NULL;
ssfplay_result result = ssfplay_open("track.ssf", NULL, &decoder);
int16_t pcm[1024 * 2];
size_t frames = 0;
result = ssfplay_render(decoder, pcm, 1024, &frames);
ssfplay_close(decoder);
```

Referenced `.ssflib` files are resolved relative to the top-level SSF file.
Absolute paths and parent-directory traversal are rejected. V1 supports one
active decoder at a time and calls must be externally serialized because the
underlying Mednafen sound core is global. `ssfplay_last_error()` describes
failures that occur before a decoder handle can be returned.

Installed CMake projects can use:

```cmake
find_package(ssfplay REQUIRED)
target_link_libraries(my_player PRIVATE ssfplay::ssfplay)
```

## Optional Builds

WASM requires Emscripten:

```sh
emcmake cmake -S . -B build-wasm -DSSFPLAY_BUILD_CLI=OFF -DSSFPLAY_BUILD_WASM=ON
cmake --build build-wasm --target ssfplay_wasm
```

The foobar2000 component lives in `components/foobar2000` and is gated behind
the official foobar2000 SDK:

```sh
cmake -S . -B build-fb2k -DSSFPLAY_BUILD_FOOBAR2000=ON -DFB2K_SDK_DIR=/path/to/foobar2000-SDK
cmake --build build-fb2k --target foo_ssfplay
```

GitHub Actions builds native tool packages for Windows, Linux, macOS Intel, and
WASM artifacts. The native packages contain only `ssfplay_cli`, `ssf2wav`,
`ssf2vgm`, and license/docs.
