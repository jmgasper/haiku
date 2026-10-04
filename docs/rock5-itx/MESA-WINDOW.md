# Native GPU rendering in Haiku windows

The `hrev60097+215` USB image qualifies Mesa/Panfrost EGL window rendering on
two native ROCK boots. Each boot renders eight frames over two contexts and
six view resizes. All 84,480 bitmap pixels and all 84,480 independently captured
screen pixels across both boots match the expected images. Both NanoKVM HDMI
captures show the rendered window. All 36 window submissions complete, and
GPU allocation counts return to baseline after each context.

The GPU renders the content; a CPU copy presents it through Haiku's existing
bitmap/display path. The desktop still uses the EFI framebuffer. This does not
qualify an accelerated app_server, native VOP2/HDMI modes or general OpenGL
application compatibility. A later +217 [OpenGL Kit fixture](MESA-GLVIEW.md)
qualifies two live BGLViews and desktop OpenGL rendering. Sustained
load, conformance, firmware heap growth, timestamps and GPU fault/reset
recovery remain separate work.

## Implementation and checks

The [pinned Mesa port](../../tools/rock5-itx/mesa/README.md) now supports native
EGL window surfaces through Haiku's BitmapHook interface. GPU render targets
use ordinary Panfrost resources. Presentation maps the completed texture,
copies its BGRA pixels into a new B_RGBA32 bitmap, and publishes that bitmap
through the hook. The drawing thread sees a complete bitmap, and the previous
bitmap is released after replacement. Native texture mapping also exercises
Panfrost's GPU blit into a linear staging image.

EGL surface queries and framebuffer validation update the view dimensions.
Resizing retires old textures; software surfaces detach their borrowed bitmap
before its display target is freed. Native presentation bitmaps have their own
lifetime. Destroying a surface clears its window hook. The probe checks that
the bitmap has been retired before destroying its context and display.

Each context renders at 64x64, 79x47, 128x72 and 65x63 while retaining its EGL
binding. Two overlapping, differently coloured rectangles exercise complete
RGB data, image orientation and changing row strides. The probe checks every
bitmap pixel, including alpha, waits for drawing to complete, and independently
reads every displayed RGB pixel through BScreen. A separate host validator
checks the returned bytes against an integer pixel-region oracle and saves
the actual images. Native GPU properties, command submissions/completions and
allocation observations accompany the pixels. A renderer string alone cannot
satisfy these checks.

All earlier GPU regressions pass on both boots: 1,696 accepted submissions,
1,432 checked completions, eight application compute shaders and 86 property
queries. The earlier offscreen Mesa workload also passes all 32 submissions
and 32,768 pixels. These totals are separate from the window test. All ten
runtime teardowns report clean firmware, engine, fault and retained-page state.
No firmware heap growth event occurred.

The full ARM64 build and both two-boot QEMU modes pass. QEMU checks the explicit
softpipe window fixture, complete bitmap/screen data, resizing, retirement,
offscreen rendering and the previous CPU/absent-GPU checks. It does not emulate
Mali. The unchanged kernel retains its previously passing 147 host checks.
The +207 SDK was checked against the actual +215 runtime: all 2,740 libroot
and 11,737 libbe export entries remain compatible, including object sizes.

Normal reboot and shutdown pass. Recovery follows verified shutdown, with
independent Linux readback preserving the eMMC files, FAT filesystem and three
reference regions. The recovery-image hash is unchanged and the watchdog is
disarmed. The source bundle, exact unstripped binaries, controllers, validators
and failed first native trial remain preserved.

## Retained failure

The +214 candidate passed both software QEMU modes and the native offscreen
regressions, then asserted on its first native window swap. The preserved
binary resolves the stack to `haiku_present_texture`, `pan_alloc_staging` and
`pan_image_layout_init`. Its positional `pipe_box` initializer assumed the
order x/y/z/width/height/depth. Mesa 25.3.6 uses x/width/y/height/z/depth, so the
requested transfer width was zero. Native AFBC staging correctly rejected that
layout. The correction uses Mesa's `u_box_2d` helper.

The failed process remained in the debugger until guarded recovery forced a
reset. That attempt does not count as a clean-shutdown pass. Independent storage
and recovery-image checks passed afterward; its used USB image was archived,
verified and removed from the NanoKVM. The corrected +215 image passed all
build, emulator and native gates described above.

## Evidence

All paths below are relative to `/mnt/HaikuWork`:

- Qualified source: `5189c37a504690de6a8d7af9af00903981e400f1`.
- Mesa/GLVND reconstruction build:
  `artifacts/mali-window-build/20260915T141402Z-43a132/result.json`.
- Full Haiku build: `artifacts/build-20260915T141431Z.log`.
- Image: `artifacts/mali-window-image/20260915T141524Z-504da9/manifest.json`,
  SHA-256 `ea8328d2f97eb8eb0ad45845bd2c7666f44052a6f14e74b0320eefd8007a98f4`.
- EL1 and EL2 QEMU: `artifacts/qemu-shell/20260915T141557Z-3a16a7` and
  `artifacts/qemu-shell/20260915T141855Z-f3d73b`.
- Native qualification, full pixel images, visual reviews and exact source:
  `artifacts/automated-mali-window/20260915T142210Z-e0c115`.
- Native transcripts/serial and desktop frames 007/021:
  `artifacts/interactive/20260915T142224Z-34fad0`.
- Independent eMMC files and regions:
  `artifacts/emmc-file-readback/20260915T143106Z-406a3b` and
  `artifacts/emmc-read-reference/20260915T143121Z-dbf287`.
- Qualified used-image archive:
  `artifacts/nanokvm-image-archive/20260915T143335Z-b1308b/result.json`,
  SHA-256 `db9a8279459ae2adaac7df9a963cdc07cb1bac83b0fcf6e5deabca68da9f1975`.
- Failed +214 native trial, stack, source and recovery checks:
  `artifacts/automated-mali-window/20260915T135620Z-ebf04a`.
- Failed used-image archive:
  `artifacts/nanokvm-image-archive/20260915T141430Z-19babc/result.json`,
  SHA-256 `866c9b46c32add992c1f07277450fa82b96ec6e25cf3ba2910410dce934a9f00`.

The original offscreen qualification and byte-for-byte Linux comparison remain
documented separately in [MESA.md](MESA.md).
