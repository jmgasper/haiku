# Graphics-process cleanup

The `hrev60097+221` USB image qualifies graphics-process cleanup on two native
ROCK boots. Two processes hold independent EGL/GLES contexts. One is terminated
with its resources still open, the survivor renders a new checked image, and
a fresh process renders after both original processes have exited. All eight
frames, 51,992 pixels and 1,024 guard bytes pass. Native allocation counts return
to baseline, and normal reboot, shutdown, recovery and storage integrity pass.

The terminated processes finish their GPU work before SIGKILL. This tests
cleanup of live graphics resources and continued use by another application.
Termination during pending graphics work, automatic GPU fault/reset recovery,
context sharing and sustained rendering remain open. The earlier
[pipeline](MESA-PIPELINE.md), [OpenGL Kit](MESA-GLVIEW.md), [EGL window](MESA-WINDOW.md)
and [offscreen](MESA.md) fixtures remain qualified. The desktop still uses the
EFI framebuffer and window presentation still uses CPU bitmap copies.

## Workload and acceptance

The [source bundle](../../tools/rock5-itx/mesa/README.md) adds a process fixture,
launcher, independent validator and build packaging. The kernel and rendering
libraries are unchanged from the qualified pipeline image. Each worker is a
separate executable process with its own EGL context and 97x67 pbuffer; the
renderer reports OpenGL ES 3.1, Mali-G610 (Panfrost) and Mesa 25.3.6.

Each boot performs this sequence:

1. The first worker uploads and renders a texture pattern, checks every pixel
   and guard, waits for completion, and keeps its context and resources open.
2. A second worker does the same with a different pattern. Both processes and
   their native allocations must be live simultaneously.
3. The parent terminates the second worker and verifies SIGKILL through waitpid.
   The surviving worker uploads and renders the new pattern in its existing
   context, checks the complete image, then destroys its resources normally.
4. After the allocation baseline is restored, a third process creates a fresh
   context, renders a checked image and exits normally. The baseline must be
   restored again.

The parent uses separate control/readiness pipes and worker logs. Descriptors
are closed across exec unless needed by the worker. Readiness checks have
deadlines, child cleanup runs on failure, and diagnostic logs are retained in
the returned transcript. The host checks actual log lengths, process/phase
order, exit causes, renderer identity and every returned RGBA byte and guard.
It writes the actual images and independently recomputes their expected data.
All four frame hashes match across four QEMU software runs and both native runs.

The native observer records this allocation sequence on both boots. Its own
read-only client is excluded from the rendering-client column:

| Observation | Rendering clients | GPU buffers | Tiler heaps |
| --- | ---: | ---: | ---: |
| Initial baseline | 0 | 0 | 0 |
| First worker live | 1 | 20 | 1 |
| Both workers live | 2 | 40 | 2 |
| After second worker termination | 1 | 20 | 1 |
| After survivor exit | 0 | 0 | 0 |
| Fresh worker live | 1 | 20 | 1 |
| Final baseline | 0 | 0 | 0 |

The full baseline also covers buffer bytes, VM generations/page tables, heap
chunks/page tables, synchronization objects, kernel areas and CPU mappings.
Native cached GPU properties and accepted submission traces accompany all
three workers. Each boot accepts fifteen submissions: seven in the survivor,
four in the terminated worker and four in the fresh worker. The normal workers
report eleven final queue completions. The terminated worker has completed its
rendering/readback and glFinish before readiness, but SIGKILL prevents its
userspace queue-destruction report. Across both boots, thirty accepted
submissions and twenty-two final normal-worker completion records are retained.
These are deliberately separate counts.

The full Haiku build and both two-boot QEMU modes pass. QEMU uses explicit
softpipe and exercises the actual Haiku process lifecycle, without emulating
Mali. Twenty altered software transcripts and twenty-six altered native
transcripts are rejected, including missing survivor rendering, wrong exit
signals, changed pixels/guards, GPU identity or sequence/completion errors,
and incorrect allocation counts. Log sizes are recomputed in these mutations
so semantic errors reach the relevant checks.

All earlier native fixtures pass: sixty pipeline, 104 OpenGL Kit, thirty-six
EGL window and thirty-two offscreen submissions complete. Kernel regressions
retain 1,696 accepted submissions, 1,432 checked completions, eight compute
shaders and 86 property queries. All twenty-two runtime teardowns report clean
engine, firmware, fault and retained-page state. No firmware heap-growth event
occurs. Both desktops and all four window/OpenGL Kit captures pass visual review.

Reconstruction verifies unchanged contents for 11,272 Mesa files, 258 libglvnd
files and eleven symlinks. The unchanged kernel retains its previously passing
147 host checks. All 2,740 libroot and 11,737 libbe export entries in the +207
SDK remain compatible with the actual +221 runtime. The native evidence freezes
78 source, controller, script and unstripped binary files; all hashes are
rechecked after the run.

Recovery follows verified shutdown. Linux independently verifies the eMMC
files, FAT filesystem and three reference regions. The recovery-image hash is
unchanged, the watchdog disarms, and no new hardware intervention is required.

## Evidence

Paths are relative to `/mnt/HaikuWork`:

- Source: `b550d8d744c12b7c3c3ac0fa17a4cb73e0f675fb`.
- Package build: `artifacts/mali-lifetime-build/20260915T154858Z-1e1ef4/result.json`.
- Full Haiku build: `artifacts/build-20260915T155221Z.log`.
- Image: `artifacts/mali-lifetime-image/20260915T155259Z-346a07/manifest.json`,
  SHA-256 `5c8686f102cd59c524bb4e833b8d6776abccea03b92123b1e73b7cd3689378d0`.
- EL1/EL2 QEMU: `artifacts/qemu-shell/20260915T155438Z-3cd3de` and
  `artifacts/qemu-shell/20260915T155822Z-78e5fc`.
- Native qualification, actual images, source and cross-renderer comparison:
  `artifacts/automated-mali-lifetime/20260915T160223Z-fd105d`.
- Serial, transcripts and desktop frames 007/026:
  `artifacts/interactive/20260915T160230Z-73a276`.
- Independent eMMC files and regions:
  `artifacts/emmc-file-readback/20260915T161215Z-54f10f` and
  `artifacts/emmc-read-reference/20260915T161302Z-da7ecf`.
- Controllers, reconstruction and validator corruption checks:
  `artifacts/mali-lifetime/20260915T153527Z-c0dc48`.
- Verified used-image archive:
  `artifacts/nanokvm-image-archive/20260915T161454Z-9b74d3/result.json`,
  SHA-256 `3f985109b414c60a3bd356237ca7c935273dc05a89b3d000e260143acfeccc96`.
