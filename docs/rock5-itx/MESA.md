# Native Mesa rendering

The `hrev60097+210` USB image now runs Mesa 25.3.6/Panfrost through Haiku's
native Mali CSF interface. Two native boots each render four complete 64-by-64
RGBA8 images over two GLES contexts. All 32,768 pixels across both boots match
the independently qualified Linux reference byte for byte; all 1,024 readback
guard bytes pass. The renderer is `Mali-G610 (Panfrost)` and the reported API is
`OpenGL ES 3.1 Mesa 25.3.6`.

This is offscreen rendering. The desktop still uses the EFI framebuffer.
Accelerated window presentation, native VOP2/HDMI control, conformance,
sustained rendering, GPU timestamps, firmware heap OOM/growth and automatic
GPU fault/reset recovery remain unqualified.

## Changes

The [pinned Mesa port](../../tools/rock5-itx/mesa/README.md) contains the native
backend, C++ ABI bridge, EGL/HGL corrections and libglvnd build changes.
Panfrost now uses Haiku client buffers, CPU areas, atomic VM mapping batches,
persistent queues, shared/snapshot synchronization, tiler heaps and worker-cached
GPU properties. Queue creation initializes the firmware when necessary and
joins an existing runtime otherwise. A bootstrap queue pins the runtime while
Mesa uses the captured hardware properties.

The backend commits its VM bookkeeping only after a successful native batch.
Queued work retains the kernel's immutable VM generations. Native binary and
timeline fences replace the Linux-specific synchronization calls, including
calls outside Mesa's common kernel-backend abstraction. The C++ bridge converts
each request explicitly, retains Haiku's negative errno values and writes
outputs only after success. Linux DMA-BUF and sync_file interoperability are
unsupported. The native factory requires `HAIKU_CSF_DEVICE`; a failed native
open cannot silently qualify through software fallback.

The Haiku EGL frontend now honors requested API/profile/version attributes,
derives advertised API versions from the actual screen, initializes visuals
and supports pbuffers without a window hook. Pbuffer swaps preserve their
contents. Current-context references survive EGL termination/reinitialization;
retired handles are rejected and final unbinding releases the display.
Drawable retirement is reported before freeing the framebuffer. Unimplemented
extensions are no longer advertised.

The SDK is pinned to the +207 Haiku/devel packages; the tested runtime is +210.
All 2,740 libroot and 11,737 libbe export entries were checked for ABI
compatibility, including object sizes. QEMU and native execution use the
actual +210 runtime and staged GLVND/EGL/GLES libraries. This port does not
replace the normal system OpenGL packages.

## Acceptance

Each native boot checks all four full framebuffer readbacks, every guard byte,
pbuffer rendering/swap preservation and termination with a current context.
Real GPU properties and accepted/completed native queues accompany the pixels;
renderer strings alone cannot satisfy the validator. Each boot records 16
Mesa submissions and 16 completions, with no pending or failed submission at
queue destruction. Buffer, VM, heap, synchronization, kernel-area and CPU-map
counts return to the independently sampled baseline after each context.

Earlier GPU regressions also pass across both boots: 1,696 accepted submissions,
1,432 checked completions and eight application compute shaders. Those totals
are separate from Mesa's 32 submissions/completions. All six runtime teardowns
report clean firmware, engine, fault and retained-page state. No firmware heap
growth event occurred, so these tests do not qualify that path.

The full ARM64 build and both two-boot QEMU modes pass. QEMU executes the
explicit softpipe fixture, EGL lifecycle checks and absent-native-device
rejection; it does not emulate Mali. The new host fixtures check the native
bridge's request bytes, the synchronization adapter and 6,000 randomized VM
transactions with injected native-call failures under ASan/UBSan. These extend
the previously passing 147 Haiku host checks; no kernel source changed here.

Both native desktops were inspected. Normal reboot and shutdown pass, followed
by recovery to ROOBI only after verified shutdown. Independent Linux readback
preserves the eMMC fixture files, FAT filesystem and three reference regions.
The recovery image hash remains unchanged and the watchdog disarms.

A fresh build from the saved recipe also passes. All 11,272 Mesa source files
and all 258 libglvnd source files match the qualified source trees byte for
byte. The rebuilt binaries are retained separately; the native qualification
belongs to the exact image hash below. The used NanoKVM boot image was streamed
to local storage, verified again and removed from the NanoKVM.

## Retained failure

The first native image passed the earlier GPU regressions, reported the correct
GPU properties and submitted Mesa's initial 40-byte command stream. Context
creation then hit Haiku's `mutex->owner == -1` assertion. The preserved binary
and stack identify `panfrost_bo_unreference`: `panfrost_open_device` had left
`bo_map_lock` uninitialized. Zero-filled storage is not a valid Haiku mutex.
The correction explicitly initializes and destroys that lock.

The failed process stayed in the debugger until the guarded trial timed out.
Recovery required a forced transition; it was not a clean-shutdown pass.
Independent storage/recovery checks passed afterward. Both corrected native
boots and their full build/QEMU gates pass. The failed image, logs, exact
unstripped library, source patch and recovery evidence remain preserved.

## Evidence

All paths below are relative to `/mnt/HaikuWork`:

- Haiku source: `02838b66da391328d7fd61858ff51d190a8de618`.
- Corrected image: `artifacts/mali-mesa-image/20260915T124436Z-33878e/manifest.json`,
  SHA-256 `837b21274b3b9b38a07dee646c2a54d7d912bc71ee8a4331a7eab2b22063a49d`.
- Mesa source/build/package receipts:
  `artifacts/mali-haiku-mesa/20260915T100752Z-80955e`.
- Full Haiku build: `artifacts/build-20260915T124307Z.log`.
- EL1 and EL2 QEMU: `artifacts/qemu-shell/20260915T124524Z-c8a97d` and
  `artifacts/qemu-shell/20260915T124934Z-6f3aef`.
- Native qualification, full Linux pixel comparison and frozen source/binary:
  `artifacts/automated-mali-mesa/20260915T125320Z-27e59d`.
- Native serial/transcripts and desktop frames 007/018:
  `artifacts/interactive/20260915T125341Z-9db835`.
- Independent eMMC files/regions:
  `artifacts/emmc-file-readback/20260915T130528Z-6b8a16` and
  `artifacts/emmc-read-reference/20260915T130615Z-565aea`.
- Source reconstruction: `artifacts/mesa-reconstruction/20260915T131119Z-96b4bc/reconstruction-review.json`.
- Used-image archive: `artifacts/nanokvm-image-archive/20260915T130745Z-3ada74/result.json`,
  SHA-256 `980fe378c0deae173439560d747052f9ffa39087b82132bd1f6b6f4cf86aab1d`.
- Failed native trial: `artifacts/automated-mali-mesa/20260915T123229Z-927db4`;
  its image SHA-256 is
  `effdf0d8a0507cf031e9d1b05d6edb70bfa11ea9f0cb3c76999b1cdf74ee0627`.

The separate Linux reference is
`artifacts/native-linux-mesa-reference/20260915T043315Z-087ca7/qualification.json`.
It uses Linux 6.18.52, Mesa 25.3.6 and the same official Mali firmware.
The four frame SHA-256 values repeat exactly on both Haiku boots:

| Context/frame | RGBA8 SHA-256 |
| --- | --- |
| 0/0 | `f7f36ce80f94024378d11e70f2b3bf2a9119e3ed9b0743e61fb46bd1226a17ac` |
| 0/1 | `48198520dacd604015e7536c8300b7ec41447fee0f30809453dd41c106cc3183` |
| 1/0 | `79a6b6570725160ea656e4092bb9a952bb7715859a1b2cf0904ced8ccf993979` |
| 1/1 | `9abceb3bd4155ec733c89d6ae7104a9e35e2a01d84242704be2b00db8a61613e` |
