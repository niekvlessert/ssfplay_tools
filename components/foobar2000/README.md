# foo_ssfplay

This directory contains a foobar2000 input component skeleton that wraps the
`ssfplay` decoder. It is intentionally gated behind `SSFPLAY_BUILD_FOOBAR2000`
because the foobar2000 SDK is not redistributed in this repository.

```sh
cmake -S . -B build-fb2k -DSSFPLAY_BUILD_FOOBAR2000=ON -DFB2K_SDK_DIR=/path/to/foobar2000-SDK
cmake --build build-fb2k --target foo_ssfplay
```

The classic foobar2000 SDK targets Windows desktop components. macOS component
support is not wire-compatible with the Windows SDK; this source is a starting
point for the Windows component and documents the dependency boundary rather
than pretending to ship a universal foobar component without the SDK.
