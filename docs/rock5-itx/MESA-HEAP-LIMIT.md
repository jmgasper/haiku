# Fixed tiler heap and incremental rendering

The `hrev60097+227` USB image qualifies bounded incremental rendering on two
native ROCK boots. Four contexts keep one tiler heap chunk throughout their
work. All 36 firmware requests for additional chunks are refused, and Mesa
completes 36 incremental rendering passes. All 51,992 RGBA pixels, 1,024 guards,
24 submissions and allocation baselines pass. Normal reboot/shutdown, recovery
and independent storage/image integrity checks pass.

The test fixes each context's tiler heap at one 256 KiB chunk. Larger draws
then require Mesa's incremental rendering path: the firmware asks for another
chunk, the kernel declines because the configured limit is reached, and the
GPU renders a partial batch before reusing its existing heap storage. This is
a limit on the tiler heap, not on all memory used by Mesa or the application.

## Workload and checks

The geometry and independent pixel oracle are the same as the qualified
[heap-growth fixture](MESA-HEAP-PRESSURE.md). Two fresh GLES contexts each draw
8,192 and 16,384 one-pixel quads into a 97 by 67 RGBA8 framebuffer. Coordinates
come from a 786,432-byte vertex buffer containing 98,304 vertices. The four
images are red, green, blue and cyan. Complete RGBA readbacks and 64-byte guards
on each side must match, fences must complete, and context retirement must
restore the global allocation baseline.

The native heap observer requires exactly one chunk and 266,240 bytes, including
the 4 KiB heap control allocation, before and after both draws in each context.
It rejects extra chunks. Mesa runs with `PAN_MESA_DEBUG=perf,sync`; its existing
diagnostic reads the GPU-written incremental-pass counter after completion.
The host requires one positive counter per draw and matches those counters to
the kernel's actual refused heap requests. The five-second kernel job deadline
is unchanged. The software fixture does not claim native heap activity.

The common growth/limit probe uses compile-time options, retaining the accepted
growth workload and validator. The limit validator reads the original transcript
directly; it does not rewrite the growth transcript's labels or values.
Mesa, libglvnd and kernel implementation sources are unchanged from the qualified
growth version.

## Native outcome and limits

The renderer reports Mali-G610 (Panfrost), GLES 3.1 and Mesa 25.3.6. Each context
needs three incremental passes for 8,192 quads and six for 16,384. All four
kernel context summaries report nine heap events, zero growths and nine refusals.
All thirty runtime teardowns report clean firmware/engine state, no GPU/MMU
faults and no retained pages. Allocation baselines are restored after each
context, including buffers, mappings and synchronization objects.

Earlier regressions also pass: 24 growth-fixture submissions; 30 lifetime
submissions with 22 normal final completion records; 60 pipeline, 104 OpenGL
Kit, 36 EGL window and 32 offscreen submissions. The killed lifetime workers
finish and verify their GPU images before termination but do not emit normal
queue-destruction completion records. Kernel regressions retain 1,696 accepted
submissions, 1,432 checked completions, eight compute shaders and 86 properties.

Every native RGBA hash matches the actual software output. Thirty-four corrupted
native limit transcripts are rejected. Six actual NanoKVM HDMI frames show the
two desktops and both kinds of visible graphics fixture. The native source
packet contains 97 files, with every hash verified and all 34 bundle files
matched to the committed source. The recovery image, eMMC files, FAT filesystem
and three reference regions retain their expected hashes; the watchdog disarms.

This tests explicit per-heap limits and normal incremental rendering. It does
not exhaust system RAM or establish sustained-load stability, rendering
conformance, termination during pending GPU work or automatic fault/reset
recovery. Private Mesa libraries remain selected by the fixture launchers.
Presentation still copies GPU images into CPU bitmaps and uses the EFI desktop
framebuffer; native display control and accelerated app_server remain open.

## Screen-capture synchronization

The first +226 QEMU attempt failed in the earlier OpenGL Kit regression, before
reaching the heap-limit fixture. Its final GL readback was correct, while the
65 by 63 screen capture contained 4,095 white pixels. The source, unstripped
binaries, image and full actual readback are preserved. No native +226 run was
performed.

The fixture previously called Sync inside its Draw callback and signalled a
draw counter before returning. BWindow sends AS_END_UPDATE after the callback;
app_server publishes its back buffer when handling that message. BScreen reads
the front buffer, so the old signal could allow capture before publication.

The +227 probe sends a bounded synchronous request to the window thread. That
handler invalidates the whole view, calls UpdateIfNeeded, then Sync after the
update dispatch, and captures the screen once while still on that thread.
Every full pixel and guard check remains required. The host also requires all
sixteen completed-update records. No pixel retry or arbitrary repaint delay
was added.

## Build and software evidence

The complete Haiku build passes. Fresh reconstruction verifies 11,272 Mesa files,
258 libglvnd files and all eleven symlinks against the qualified archive-derived
baseline. The +207 SDK's 2,740 libroot and 11,737 libbe exports remain compatible
with the actual +227 runtime. All probes compile with warnings treated as errors.

Both required QEMU modes pass over normal reboot. An initial invocation used
positional mode arguments instead of --el1, launching an additional EL2 run;
that complete passing run is retained. The actual exception levels were checked
in the serial logs. The original +224 mode evidence was also checked and is
correct. All six software frame sets have identical RGBA hashes. Twenty-three
corrupted limit transcripts and twenty-two corrupted OpenGL Kit transcripts
are rejected, including missing/incomplete publication records and extra captures.

## Evidence

Paths are relative to `/mnt/HaikuWork`:

- Source: `38ac04578d495d8f89048a5a9f78e3256fc57174` (`hrev60097+227`).
- Package: `artifacts/mali-heap-limit-build/20260915T173218Z-9552f5/result.json`.
- Full Haiku build: `artifacts/build-20260915T173331Z.log`.
- Image: `artifacts/mali-heap-limit-image/20260915T173600Z-9a9817/manifest.json`,
  SHA-256 `b271582ef1aafac50d5a7e40b910a8b050cfa1f7ce82bd372b6d52c9f259dc8b`.
- Required EL1 and EL2: `artifacts/qemu-shell/20260915T174050Z-e76e01` and
  `artifacts/qemu-shell/20260915T174051Z-c61faf`; additional EL2
  `artifacts/qemu-shell/20260915T173650Z-b77990`.
- Native qualification, screenshots and source packet: `artifacts/automated-mali-heap-limit/20260915T174459Z-9d8421`.
- Native transcripts and UART: `artifacts/interactive/20260915T174526Z-2007fb`.
- Controllers, reconstruction, ABI and mutation checks:
  `artifacts/mali-heap-limit/20260915T173218Z-cee443`.
- Failed +226 preservation:
  `artifacts/mali-heap-limit/20260915T170811Z-579393/failure-preservation/manifest.json`.

- Independent eMMC files and regions: `artifacts/emmc-file-readback/20260915T175516Z-d86aa5`
  and `artifacts/emmc-read-reference/20260915T175544Z-bbf2dc`.
- Verified used-image archive: `artifacts/nanokvm-image-archive/20260915T175706Z-2faf61/result.json`,
  SHA-256 `79b5d5af03281110e21f2135fb725965f105807a88ff4ce8fae23daf5613ddb6`.
