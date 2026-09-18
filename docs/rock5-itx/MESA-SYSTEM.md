# Mesa/Panfrost as the system OpenGL

Updated 2026-09-18. This page records the candidate that lets unmodified
BGLView applications use the Mali GPU without a launch environment, and
the evidence gathered for it. It is written to the same standard as the
other Mesa pages: what was actually run, and what remains open.

## What changes

The private Mesa/Panfrost port so far ran only under its trial launchers,
which set the library path, the EGL vendor file, the Mali device, the
firmware file and the CPU polygon switch. The system-default candidate
removes every one of those requirements:

- The Haiku EGL backend takes `/dev/graphics/mali_csf/0` when the driver
  published it and no `HAIKU_CSF_DEVICE` is given. An explicit device still
  never falls back to software; the implicit default does, so the same
  libraries work in the emulator (no device) and on the board.
- Without `HAIKU_CSF_FIRMWARE` the CSF firmware is searched under
  `/boot/system/data/firmware/mali/arch10.8/`, then
  `/boot/system/non-packaged/data/firmware/mali/arch10.8/`, then
  `~/config/non-packaged/data/firmware/mali/arch10.8/`.
- The CPU polygon path (quads, quad strips, polygons with their original
  boundaries) is on by default; `HAIKU_PAN_SW_POLYGON=0` turns it off. The
  first-vertex provoking fix of +258 is included.
- The image installs the six GLVND/Mesa libraries in
  `/boot/system/non-packaged/lib`, which the runtime loader searches before
  `/boot/system/lib`, the EGL vendor file `10_mesa_panfrost.json` in
  `/boot/system/non-packaged/add-ons/opengl/egl_vendor.d` (one of the
  directories the Haiku libglvnd port searches), and a copy of the firmware
  under `/boot/system/non-packaged/data/firmware/mali/arch10.8`.

The trial launchers keep working unchanged. A new launcher, `run-system`,
runs the application fixture (GLTeapot under the application probe) with
every override cleared; the probe's `--system` mode records whether the
device was present. `system_validation.py` applies the application
fixture's own frame, process and logging checks to such a transcript and
additionally requires the recorded device state, the installed paths and
the absence of every override.

## Procedure

The stage `artifacts/mali-system-opengl/20260918T130331Z-86f2de` derives its
controls from the +258 first-provoking stage. Its image is the qualified
+256 image with five changed trial assets (the Mesa library, the polygon
probe and launcher from +258, the application probe with `--system`, and
`run-system`) and the eight system-install files; GLTeapot and libGLU stay
the qualified +256 copies. The two-boot emulator gates run every earlier
software fixture and, new, the system launch with the device absent, which
must fall back to software and render the same fixture. The native cycle
runs every earlier check and then, after the provoking captures, the system
launch on the board with its UART interval (two more firmware runtimes per
boot) and NanoKVM desktop captures.

## Results

The candidate is Mesa at `3139063445` (the pinned patch regenerated with the
three default changes), built in `artifacts/mali-system-opengl-build/20260918T125722Z`
and `artifacts/mali-system-opengl-application-build/20260918T125722Z`; the
image (SHA-256 `65c4c51c445d196c0bed4cf1017477f45d53a463bac6036fb1633de50d9b1df0`)
is the qualified +256 image with five changed trial assets and the eight
system-install files. The host geometry test passes on a sanitizer build
of this source, the exported and imported symbols of `libEGL_mesa.so.0`
are unchanged from +258, and both two-boot emulator gates pass
(`artifacts/qemu-shell/20260918T131452Z-5977e3` for EL2 after a lost USB
shell marker on the first attempt, `20260918T132520Z-ed2adb` for EL1), with
the system launch falling back to software rendering when the device is
absent. All twelve software contact sheets were actually reviewed.

The native run `artifacts/automated-mali-system-opengl/20260918T140259Z-19a24a`
(session `artifacts/interactive/20260918T140301Z-59b6f2`, recovery boot
`6d9de909-593f-4af7-8e57-b822665e99b0` after verified shutdown) passed every
check on both boots: inventory, token lifetime, window, OpenGL Kit,
pipeline, lifetime, heap pressure, heap limit, concurrency, pending,
recovery, context loss, GLTeapot under the traced launcher, the default,
first-provoking and last-provoking polygon captures, and the system
launch. The system launch ran GLTeapot under the application probe with no
environment at all: the probe recorded the device present, both cycles
passed the application fixture's frame, process and logging checks (16
frames per boot), the quad strips in those frames are intact because the
CPU polygon path is now the default, the UART shows two firmware runtimes
per boot inside the launch's interval (runtime marks 31 to 33) with normal
status, and the NanoKVM captured the GLTeapot window rendering on the
desktop. The polygon, application and system contact sheets of both boots
and the desktop, EGL window, OpenGL Kit and both GLTeapot HDMI frames of
each boot were actually reviewed. `review-polygon.py native`,
`review-provoking.py native` and `review-native-results.py` pass (52
software captures, 112 shared frame comparisons, 137 git files, 268 frozen
files). `qualify-native.py` passes every native check, including the
system-launch interval, and then stops at the independent Linux eMMC
readbacks, which need the ROOBI sudo password in the trial's own recovery
boot; that password is not on record. **The candidate is therefore not
qualified**; the +256 image remains the qualified Mesa result and this
image is the working system-default candidate.

The first native run (`…automated-mali-system-opengl/20260918T134041Z-5316b7`)
is retained: the board side passed identically, but the host validator
still required the GPU trace lines that only the traced launcher prints;
950e0e67a4 fixed that, and the stage records a separate procedure revision
for its host-side files (the image's source revision is unchanged). Used
images: `artifacts/nanokvm-image-archive/20260918T135734Z-c0f791-mali-system-opengl-validator`
and the second run's archive named `mali-system-opengl-native-pass`.

Open: the readbacks and qualification; broader application coverage
beyond GLTeapot; a packaged (rather than non-packaged) install.
