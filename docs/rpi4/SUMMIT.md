# Summit on the Raspberry Pi 4

State 2026-10-04: Summit composites on the GPU and has WebGL.
`https://get.webgl.org/` says "Your browser supports WebGL" and shows the
spinning cube (`evidence/summit-gl1.jpg`); the WebGL Aquarium
(`webglsamples.org/aquarium`) runs at 21 frames a second with 500 fish on a
1024x1024 canvas (`evidence/summit-aquarium.jpg`). With the arm64 engine of
the ROCK 5 image the same pages said "your browser does not seem to support
WebGL": that engine was built without GL.

## What changed

Summit's WebKit has had GL compositing and WebGL since September, but only in
the x86 build. The arm64 engine (`/mnt/HaikuWork/build/summit-arm64`) was the
software configuration: app_server drawing, no Skia, `ENABLE_WEBGL=OFF`. For
the Pi the same source tree is built a second time, in
`/mnt/HaikuWork/rpi4/summit-gl`, with

    -DUSE_HAIKU_GL_COMPOSITING=ON -DUSE_SKIA=ON -DENABLE_WEBGL=ON
    -DENABLE_ASYNC_SCROLLING=ON

and every other option as in the existing arm64 build (taken over from its
CMake cache). `ENABLE_ASYNC_SCROLLING` is the GL configuration's default and
has to be said out loud when options are carried over: without it the
coordinated scrolling sources do not compile. No source change was needed.

The GL configuration needs libraries the software one does not:
FreeType, Fontconfig (with Expat), HarfBuzz with ICU, and libepoxy.
`tools/rpi4/summit/build-gl-deps.sh` cross-builds them into a prefix of their
own and writes a toolchain file that searches it first. Fontconfig is
configured for Haiku's font directories and looks for its configuration in
`/boot/system/lib/summit-webkit/etc/fonts`, where the package puts it.

The engine goes into the `summit_webkit` package as before
(`tools/airos/build-arm64-app-packages.sh ... summit_webkit` with
`SUMMIT_ENGINE`, `SUMMIT_ENGINE_LOG` and `SUMMIT_ENGINE_EXTRA_DEPS` pointing
at the new build): version 1.10.0-2. The Summit and Natter packages are the
existing ones; they start with the new engine.

At run time the web process opens the system's EGL, which on this board is
Mesa's v3d driver (see `GPU.md`): WebGL goes through ANGLE to GLES on the
GPU, and the page is composited with GL.

## A newer engine (2026-10-04, night)

The Summit session (its own `docs/performance.md`, top section, has the
details) built a newer engine with the same GL configuration and put it on
the board without a restart (`pkgman install` of local files):
`summit_webkit` 1.10.0-3 (engine from Summit 9c9c2f7) and
`summit` 0.1.0~git20261004-1. The old packages are kept on the board in
`~/summit-package-backup-20261004`. The third-party libraries bundled with
the engine are rebuilt with GNU hash tables (Summit's
`tools/pi/build-deps-gnu-hash.sh`); `summit-gl/WebKitBuild` was rebuilt in
place from the newer source.

What that session measured on the Pi (warm start unless said):

| | Before | After |
|---|---|---|
| First frame of a page | 5.85 s | 1.4 s |
| Speedometer 3.1 | 1.02 | 1.36 |
| Scrolling | 7 fps with the plain newer engine (a direct-present bug, fixed) | ~30 fps |
| Cold first start after installing, to the browser's code | 11.2 s | 8.7 s, mostly the SD card at 25 MHz |
| H.264 `<video>` | nothing (see `MEDIA.md`, "Other players") | plays: Summit falls back to libavcodec when the chosen decoder fails; software, ~2.6 cores |

Both packages are staged for the next image in
`/mnt/HaikuWork/rpi4/packages-arm64`; 1.10.0-2 was moved to
`/mnt/HaikuWork/rpi4/packages-arm64-replaced`. Run
`tools/rpi4/stage-packages.sh` before building the image: the image file
built for the Media Kit fix (sha256 8f8a158e…) still has 1.10.0-2.

## Hardware video, quicker quits (2026-10-05)

The Summit session installed `summit_webkit` 1.10.0-9 (engine from Summit
e13519f) and `summit` 0.1.0~git20261005-1 (`pkgman install` of local files)
and staged both in `/mnt/HaikuWork/rpi4/packages-arm64` (1.10.0-7 and
0.1.0~git20261004-1 moved to `packages-arm64-replaced`). Run
`tools/rpi4/stage-packages.sh` before the next image. Its details are in
Summit's `docs/performance.md`, top section.

| | Before | After |
|---|---|---|
| H.264 `<video>`, MSE 720p30 | libavcodec, ~60% of pictures, 1.4 cores | `rpi_mmal`, all pictures, 0.65-0.95 cores |
| H.264, MSE 1080p30 | libavcodec, 73% of pictures, 2.6 cores | `rpi_mmal`, 97-99%, 1.35 cores |
| Quit with YouTube loading, until the window goes | 14-21 s | 1.2-1.4 s |

Summit now loads `rpi_mmal` by name (as airTime does) for eight-bit 4:2:0
progressive H.264 up to 1920x1088, and draws its I420 pictures on the GPU.
The board was restarted twice that day: once from a kernel panic, once to
clear the firmware's decoder service. What Summit found that belongs to the
OS is in its `docs/kunanyios-platform-issues.md` ("Raspberry Pi 4"):

- a panic in `VMAnonymousCache::Commit()` reached from `madvise(MADV_FREE)`
  (`_user_memory_advice` -> `Discard()`) with the system nearly out of
  memory;
- VCHIQ's bounce buffers (about 3 MB, physically contiguous, below 1 GB)
  that cannot be allocated after a while of use, after which the
  firmware's mmal service answers nothing until the next boot;
- helper teams left suspended for good when their parent dies inside
  `load_image()`;
- RAM disks that hang after a force-killed program ran from them;
- SD card writes (25 MHz) that hold up other processes' writes and team
  teardown for seconds.

## Building it again

The release engine now uses the **WebRTC configuration** in
`/mnt/HaikuWork/rpi4/summit-rtc/WebKitBuild`. Preserve this configuration:
the earlier `summit-gl` build below does not contain all the features in
1.10.0-10. With that build already configured:

```sh
tools/rpi4/summit/build-engine.sh
SUMMIT_ENGINE=/mnt/HaikuWork/rpi4/summit-rtc/WebKitBuild \
SUMMIT_ENGINE_LOG=/mnt/HaikuWork/rpi4/summit-rtc/performance-build.log \
SUMMIT_ENGINE_EXTRA_DEPS=/mnt/HaikuWork/rpi4/summit-gl/deps \
SUMMIT_SRC=/mnt/HaikuWork/apps/summit \
SUMMIT_WEBKIT_VERSION=1.10.0-11 \
    tools/airos/build-arm64-app-packages.sh \
    /mnt/HaikuWork/rpi4/packages-arm64 summit_webkit
tools/rpi4/stage-packages.sh
```

The build helper checks the pinned port snapshot and required GL/WebRTC
options, applies `webkit-haiku-exports.patch` idempotently, and rebuilds
the engine and its helper executables. The export map preserves public
embedding and process entry points while removing private symbols from
dynamic lookup. It does not disable browser features. Native comparisons,
limitations and exact binary hashes are recorded in `PERFORMANCE.md`.
When changing the map, run `check-exports.py` against the previous library,
the candidate and all consumers; name libstdc++ with `--provider` for its
ordinary allocation operators. Then run browser and embedded-view checks.

The first GL-only build was configured as follows (historical):

    tools/rpi4/summit/build-gl-deps.sh
    cd /mnt/HaikuWork/rpi4/summit-gl
    . /mnt/HaikuWork/build/summit-arm64/hosttools/env.sh
    cmake -S /mnt/HaikuWork/build/summit-arm64/WebKit -B WebKitBuild -G Ninja \
        -C init-cache.cmake -DCMAKE_TOOLCHAIN_FILE=$PWD/haiku-arm64-gl.cmake \
        -DUSE_HAIKU_GL_COMPOSITING=ON -DUSE_SKIA=ON -DENABLE_WEBGL=ON \
        -DENABLE_ASYNC_SCROLLING=ON
    ninja -C WebKitBuild -j7      # about 2 GB per compile job in WebCore

`init-cache.cmake` holds the ENABLE_/USE_ options of the software build's
`CMakeCache.txt` (all but the four above).

## Open

- Only two pages were looked at before the newer engine; see that section
  for the Summit session's measurements since.
- The SD card runs at 25 MHz (default speed): a faster card mode would
  shorten Summit's cold start most.
- The web process logs "page stall" lines while a page loads; whether they
  matter was not looked into.
- HTTPS needs the clock: the board has no RTC and sets the time from the
  network at start (`data/boot/rpi/UserBootscript`).
- Video in pages decodes in software, like airTime (`MEDIA.md`).
