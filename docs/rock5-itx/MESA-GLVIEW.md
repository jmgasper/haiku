# Native OpenGL Kit rendering

The `hrev60097+217` USB image qualifies a normal Haiku BGLView rendering
fixture on two native ROCK boots. The renderer reports Mali-G610 (Panfrost),
desktop OpenGL 3.1 and Mesa 25.3.6. Each run alternates rendering between two
live views, destroys both, and repeats with fresh views and contexts. Across
both boots, all 32 frames, 168,960 GL readback pixels, 168,960 independently
captured screen pixels and 4,096 guard bytes pass. All 104 OpenGL Kit GPU
submissions complete, and allocation counts return to baseline.

This qualifies the tested OpenGL Kit rendering path. It does not establish
general application compatibility or OpenGL conformance. The private library
launcher explicitly selects the native CSF backend; the normal system OpenGL
packages are unchanged. Presentation still copies GPU images into bitmaps on
the CPU, and the desktop still uses the EFI framebuffer. Context sharing,
killed graphics-process cleanup, sustained workloads, native display control
and GPU fault/reset recovery remain open.

## Changes and acceptance

The [source bundle](../../tools/rock5-itx/mesa/README.md) adds a normal BGLView
probe using desktop OpenGL calls. It does not supply a custom BitmapHook or
create its own EGL surfaces. BGLView handles its contexts, window surfaces,
locking and swaps through the staged libglvnd/Mesa libraries.

The libglvnd correction moves the bitmap-presence check in BGLView::Draw under
the renderer mutex. Bitmap replacement already uses that mutex; reading the
pointer outside it raced with replacement and retirement. The Mesa source is
unchanged from the qualified [EGL window implementation](MESA-WINDOW.md).

Each boot creates four contexts over two complete display lifetimes, with two
views live at a time. The probe requires distinct current-context handles,
checks recursive LockGL/UnlockGL behavior and resolves a GL entry point through
GetGLProcAddress. Rendering alternates between the views. Each view uses four
sizes, in opposite orders: 64x64, 79x47, 128x72 and 65x63. Resize checks wait for
the view notification before drawing.

The workload uses matrix transforms, glBegin/glVertex triangle drawing, colours,
buffer selection, readback and SwapBuffers. Every GL RGBA8 pixel and guard byte
is checked before swapping. After the view has drawn and synchronized with
app_server, BScreen independently captures every displayed RGB pixel. A host
validator checks the returned bytes with an integer pixel-region oracle and
saves actual images. Two NanoKVM HDMI captures corroborate the visible views.
This is sequential rendering between live contexts, not a concurrent rendering
stress test.

Native property records, accepted/completed queue records and global allocation
observations accompany the pixels. Each display lifetime has two rendering
queues and a bootstrap queue, with 26 completed submissions. The observer
checks buffers, VMs, tiler heaps, synchronization objects, kernel areas and CPU
mappings after each lifetime. All fourteen runtime teardowns across both boots
report clean firmware, engine, fault and retained-page state. No firmware heap
growth event occurs.

The earlier EGL window fixture passes all 36 submissions and 84,480 screen
pixels across both boots. Offscreen Mesa passes all 32 submissions and 32,768
pixels. Earlier kernel GPU regressions pass 1,696 accepted submissions, 1,432
checked completions, eight application compute shaders and 86 property queries.
These counts are separate from the new OpenGL Kit workload.

The full Haiku build and both two-boot QEMU modes pass. QEMU uses explicit
softpipe and reports desktop OpenGL 3.3; it does not emulate Mali. Eighteen
corrupted copies of a real passing transcript are rejected, including changed
GL/screen pixels, alpha, guards, dimensions, context identities and missing
records. The unchanged kernel retains its previously passing 147 host checks.
The +207 SDK remains compatible with all 2,740 libroot and 11,737 libbe export
entries in the actual +217 runtime.

Source reconstruction checks all 11,272 Mesa source files and all 258 libglvnd
source files against the qualified archive-derived baseline, with only the
intended GLView.cpp correction. Symlink targets also match. Initial comparisons
against private edit trees included release-archive omissions and dereferenced
symlink duplicates; the reconstruction receipt records both and the corrected
comparison. Those differences were in the chosen comparison trees.

Normal reboot and shutdown pass. Recovery follows verified shutdown, and Linux
independently verifies the eMMC files, FAT filesystem and reference regions.
The recovery-image hash is unchanged and the watchdog disarms. The source
packet, exact unstripped libraries/probe, controllers and validators are saved
with the native evidence.

## Evidence

Paths are relative to `/mnt/HaikuWork`:

- Source: `fbe492817d389b3f341fab4dcdef0e89efc78b59`.
- Package build: `artifacts/mali-glview-build/20260915T143724Z-730a88/result.json`.
- Full Haiku build: `artifacts/build-20260915T144143Z.log`.
- Image: `artifacts/mali-glview-image/20260915T144209Z-e420ac/manifest.json`,
  SHA-256 `3df1bc92e0495ee619c4fc9ca8d6a849787a36a9ea7cf93a54613806ed5bc876`.
- EL1 and EL2 QEMU: `artifacts/qemu-shell/20260915T144221Z-568482` and
  `artifacts/qemu-shell/20260915T144546Z-a254d4`.
- Native qualification, actual images, visual reviews and exact source:
  `artifacts/automated-mali-glview/20260915T145046Z-879f0e`.
- Native transcripts/serial and desktop frames 007/022:
  `artifacts/interactive/20260915T145103Z-98daf5`.
- Independent eMMC files and regions:
  `artifacts/emmc-file-readback/20260915T150104Z-66158b` and
  `artifacts/emmc-read-reference/20260915T150124Z-01d905`.
- Controller sources, source reconstruction and validator corruption checks:
  `artifacts/mali-glview/20260915T142457Z-f5f317`.
- Verified used-image archive:
  `artifacts/nanokvm-image-archive/20260915T150320Z-da68d8/result.json`,
  SHA-256 `5892459cfe39e2ba3c7d8dc2fdab6789c2921aaf828d809c0d9fc5d5fa2b4150`.
