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

## Building it again

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

- Only two pages were looked at. No comparison of scrolling or page load
  against the software engine, no long run.
- The web process logs "page stall" lines while a page loads; whether they
  matter was not looked into.
- HTTPS needs the clock: the board has no RTC and sets the time from the
  network at start (`data/boot/rpi/UserBootscript`).
- Video in pages decodes in software, like airTime (`MEDIA.md`).
