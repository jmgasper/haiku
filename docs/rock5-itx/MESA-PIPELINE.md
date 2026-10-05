# Native GLES pipeline checks

The `hrev60097+219` USB image passes six GLES pipeline operations in two
contexts on each of two native ROCK boots. All 24 complete RGBA8 images,
155,976 pixels and 3,072 readback guard bytes match the independent oracle.
All 60 native submissions complete, and allocation counts return to baseline
after every context lifetime. The renderer reports OpenGL ES 3.1,
Mali-G610 (Panfrost) and Mesa 25.3.6.

This extends the qualified [offscreen](MESA.md), [EGL window](MESA-WINDOW.md)
and [normal OpenGL Kit](MESA-GLVIEW.md) fixtures. It is bounded API coverage;
GLES conformance, general application compatibility, process termination during
graphics work, sustained rendering and GPU fault/reset recovery remain open.
Window presentation still copies GPU images into CPU bitmaps, and the desktop
uses the EFI framebuffer. Native display control remains separate work.

## Workload and validation

Each context uses a 97x67 RGBA8 target with a depth24/stencil8 attachment.
The odd dimensions exercise edges and row layout. The source texture has a
binary colour pattern, nearest filtering and clamped coordinates. Its upload
uses 408-byte rows, including 20 padding bytes per row, with guards at both
ends. The complete source allocation remains unchanged after rendering.

| Case | Operation and expected result |
| --- | --- |
| Texture | Upload padded rows and sample every source texel into the target. |
| Depth | Draw overlapping near and far rectangles, then a farther rectangle. The near fragment survives overlap and the farther redraw is rejected. |
| Stencil | Write a rectangular stencil mask with colour writes disabled, then colour only the intersection with another rectangle. |
| Blend | Add red and green with `GL_ONE, GL_ONE`; binary channel endpoints give exact saturated results. |
| Scissor | Clip a blue clear and green rectangle to the same scissor rectangle. |
| Render to texture | Render the depth scene into one texture, then sample that GPU-written texture into a second target with both axes reflected. |

Every case waits on a GL fence and reads the entire RGBA image into guarded
memory. The probe checks every channel and guard byte. The independent host
validator recomputes the expected pixels, validates every row and saves the
actual RGBA data and PNG. All twelve image hashes agree across four QEMU
softpipe runs and both native Mali runs. The host validator rejects thirty
altered copies of an actual passing transcript, including missing effects,
unreflected output, changed pixels or alpha, damaged guards, incorrect target
formats and missing completion or retirement records.

Native property records and queue submission/completion records accompany
each context. Each lifetime completes fifteen submissions. The observer
requires global buffer, VM, tiler-heap, synchronization, kernel-area and CPU
mapping counts to return to baseline. Across both boots, 109,856 bytes of source
upload memory remain unchanged, including 5,360 padding bytes and 512 guards.
The readback guards above are counted separately.

The new [source bundle](../../tools/rock5-itx/mesa/README.md) adds the probe,
launcher, validator and build packaging. Reconstructing the libraries from
pinned archives produces unchanged contents for all 11,272 Mesa files,
258 libglvnd files and their eleven symlinks compared with the qualified
OpenGL Kit build. No kernel or rendering-library change was needed for these
cases. The package contains fifteen assets in the private trial directory.

The full Haiku build and both QEMU modes pass before native deployment; each
mode tests a boot and normal reboot. The +207 SDK remains compatible with all
2,740 libroot and 11,737 libbe export entries in the actual +219 runtime.
The unchanged kernel retains its previously passing 147 host checks.

All earlier native regressions pass: 104 OpenGL Kit, 36 EGL window and 32
offscreen Mesa submissions complete; kernel tests retain 1,696 accepted
submissions, 1,432 checked completions, eight compute shaders and 86 property
queries. Both desktop captures and four window/OpenGL Kit captures pass visual
review. All eighteen runtime teardowns report clean engine, firmware, fault
and retained-page state. No firmware heap-growth event occurs.

Normal reboot, verified shutdown and automatic ROOBI recovery pass. Linux
independently verifies the eMMC files, FAT filesystem and three reference
regions. The recovery-image hash is unchanged and the watchdog disarms.
Seventy-one frozen source, controller, script and unstripped binary files
accompany the native evidence, with all hashes rechecked after qualification.

## Evidence

Paths are relative to `/mnt/HaikuWork`:

- Source: `178b12ef485e8989fd10c910aef12bf52967df87`.
- Package build: `artifacts/mali-pipeline-build/20260915T152010Z-0b82a1/result.json`.
- Full Haiku build: `artifacts/build-20260915T152233Z.log`.
- Image: `artifacts/mali-pipeline-image/20260915T152317Z-28d233/manifest.json`,
  SHA-256 `672914b587e1ae9e018434aeac6d075484076297c2f4f05cd92bbbdd5433113f`.
- EL1/EL2 QEMU: `artifacts/qemu-shell/20260915T152348Z-52ee8d` and
  `artifacts/qemu-shell/20260915T152717Z-b4e1eb`.
- Native qualification, actual images, source and cross-renderer comparison:
  `artifacts/automated-mali-pipeline/20260915T153113Z-3ca4d9`.
- Serial, transcripts and desktop frames 008/026:
  `artifacts/interactive/20260915T153119Z-c97a63`.
- Independent eMMC files and regions:
  `artifacts/emmc-file-readback/20260915T154033Z-5d62d4` and
  `artifacts/emmc-read-reference/20260915T154102Z-53f2d6`.
- Controllers, source reconstruction and validator corruption checks:
  `artifacts/mali-pipeline/20260915T150931Z-ae8113`.
- Verified used-image archive:
  `artifacts/nanokvm-image-archive/20260915T154215Z-3cbc87/result.json`,
  SHA-256 `bc053420c2c83373b89a10701f64ebfcfb4837a50efdc251c31bcfe3d40ef207`.
