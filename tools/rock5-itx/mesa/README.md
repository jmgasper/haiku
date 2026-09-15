# Experimental Haiku Mesa package

This owner-fork port connects Mesa 25.3.6/Panfrost to the qualified Haiku Mali
CSF kernel ABI. It has passed offscreen GLES rendering on two native ROCK boots;
[MESA.md](../../../docs/rock5-itx/MESA.md) records exact evidence and limits.
It is not an upstream Mesa/Haiku release or a system package replacement.

The current source also qualifies [EGL window rendering](../../../docs/rock5-itx/MESA-WINDOW.md):
native GPU textures are copied into Haiku window bitmaps. Its probe checks
complete bitmap/screen pixels, dimensions, resizing and retirement across two
native boots. The accepted window and offscreen images are pinned in those
documents. A [normal OpenGL Kit fixture](../../../docs/rock5-itx/MESA-GLVIEW.md)
also passes on two native boots with desktop OpenGL 3.1.

The BGLView/desktop OpenGL probe checks two live
views, alternating draws, resizes, recursive GL locking, full GL/screen pixel
readback and allocation cleanup. Its libglvnd patch checks the displayed bitmap
under the same renderer mutex used by bitmap replacement. Both QEMU modes and
the exact native image pinned in MESA-GLVIEW.md pass. This does not qualify
general application compatibility or replace the normal system OpenGL packages.

The later +226 QEMU run exposed an observation race in that fixture: its final
GL readback passed, but the screen capture contained the white view background.
The revised probe requests a full update on the window thread, waits for
UpdateIfNeeded and then Sync before taking its single screen capture. Sync
inside Draw runs before the window's AS_END_UPDATE publishes the front buffer.
Every pixel and guard check remains required. This synchronization revision
passes both QEMU modes and two native +227 boots; the failed +226 image, source,
binaries and actual readback remain preserved with its local evidence.

The [GLES pipeline probe](../../../docs/rock5-itx/MESA-PIPELINE.md) passes both
QEMU modes and two native boots on the +219 image. It
checks six operations in each of two contexts at 97 by 67 pixels: padded
texture upload and sampling, depth rejection, stencil masking, additive
blending, scissor clipping and reflected sampling of a GPU-rendered texture.
Complete RGBA readbacks, guard bytes and native allocation baselines are checked
independently. All 155,976 native pixels and sixty submissions pass. This
bounded fixture does not establish GLES conformance.

The [graphics-process lifetime probe](../../../docs/rock5-itx/MESA-LIFETIME.md)
passes both QEMU modes and two native boots on the +221 image. It keeps two
independent processes' contexts open, terminates one
after completed rendering, checks a new frame in the survivor, then checks
normal cleanup and fresh-context reuse. Every returned pixel and guard is
checked. All 51,992 native pixels, allocation baselines and recovery checks
pass. It does not terminate pending GPU work or test automatic GPU reset.

`sources.json` pins both original archives, six SDK packages, seven kernel ABI
headers, host compiler helpers and every patch/probe/validator. The complete
Mesa patch includes the pinned HaikuPorts changes; do not apply those twice.
For libglvnd, apply the pinned HaikuPorts patchset, then the sysroot and mutex
patches, then the bitmap-lock patch. Original source licenses/notices remain in place. This fork's original
bridge and test code is MIT licensed and AI-assisted at the owner's request.

## Reconstruct locally

Use the existing ARM64 compiler/package tool described in the lab README.
Host prerequisites are the pinned Mesa compiler helpers in
`/mnt/HaikuWork/toolchains/mesa-host/bin`, Meson/Mako/Python in `mesa-python`,
Ninja, and the extracted LLVM 18 host dependencies in `mesa-native-deps`.
These compile shader sources for the GPU; they are not Haiku shared libraries.
Source archives and SDK packages are local inputs. No downloads, host package
installation, deployment or board reboot occur in this script.

Place the two archives named in `sources.json` in one directory, and the six
SDK packages in another. The +207 SDK snapshot is retained locally at
`artifacts/mali-haiku-mesa/20260915T100752Z-80955e/packages`; the archives are
in that stage's `reconstruction-inputs` directory. Run from the Haiku checkout:

```sh
source tools/rock5-itx/env.sh
python3 tools/rock5-itx/mesa/build.py \
  --archives /mnt/HaikuWork/artifacts/mali-haiku-mesa/20260915T100752Z-80955e/reconstruction-inputs \
  --packages /mnt/HaikuWork/artifacts/mali-haiku-mesa/20260915T100752Z-80955e/packages \
  --output /mnt/HaikuWork/artifacts/mesa-reconstruction/NEW_BUILD
```

The output must be new. The script checks inputs, applies patches, verifies all
changed Mesa files and ABI headers, constructs a private SDK, cross-builds
GLVND/Mesa/the probe and emits `package/manifest.json`. `--prepare-only` stops
after patched-source and SDK preparation. Logs, commands, input receipts and
failures remain in the output directory. This reconstructs pinned sources and
configuration; build-path/debug information can change binary hashes.

The package manifest lists twenty-three assets for `/boot/home/mesa-trial`. The `run`
launcher uses Haiku's `LIBRARY_PATH` and a private GLVND vendor file. Its probe
accepts `--native`, `--software` and `--absent-device`. Native mode expects
`/dev/graphics/mali_csf/0` and the separately supplied, licensed firmware at
`/boot/home/mali_csffw.bin`. Software mode is the explicit QEMU fixture.
Neither a build result nor software rendering qualifies a native candidate:
run the full Haiku build, both QEMU modes, native cycles and recovery checks.
`run-window --software` and `run-window --native` run the window probe. It opens
a real Haiku window and checks all pixels in both its bitmap and a screen
capture over two contexts and six view resizes. `window_validation.py` checks
the returned data independently and writes actual bitmap/screen PNGs. CPU
bitmap presentation does not accelerate app_server or implement native display
modes. `run-glview --software` and `run-glview --native` run the OpenGL Kit
fixture, checked independently by `glview_validation.py`.
`run-pipeline --software` and `run-pipeline --native` run the GLES pipeline
fixture, checked independently by `pipeline_validation.py`.
`run-lifetime --software` and `run-lifetime --native` run the process fixture,
checked independently by `lifetime_validation.py`.

`render_validation.py` independently compares complete readbacks with integer
pixel-region oracles and can write actual RGBA/PNG artifacts.
`native_render_validation.py` also requires cached native GPU properties,
accepted/completed queues, expected runtime joins and allocation baselines.
Frozen build, controller, host-fixture and qualification scripts for the first
passing image remain with the evidence linked in MESA.md.

`run-heap-pressure --software` and `run-heap-pressure --native` exercise a
bounded geometry workload with 256 KiB tiler chunks, one initial chunk and a
32-chunk limit. The independent `heap_pressure_validation.py` requires all
RGBA pixels and guards, actual native allocation growth, completed queues and
full cleanup across two contexts. Firmware growth counters are checked in the
native UART evidence as a separate gate. The [qualified +224 image](../../../docs/rock5-itx/MESA-HEAP-PRESSURE.md) passes
both QEMU modes and two native boots: 24 actual growth requests, 51,992 pixels,
1,024 guards, 24 submissions, complete cleanup and recovery/integrity checks.
This fixture does not establish sustained load, memory-limit failure or GPU reset.

The pressure fixture uses an explicit 786,432-byte vertex buffer. Its first
GL-generated vertex-ID variant exposed a softpipe split-draw limitation in
QEMU; that candidate was not deployed. Its source and complete failed readback
are retained. This workload does not qualify gl_VertexID across large draws.

`run-heap-limit --software` and `run-heap-limit --native` use the same explicit
geometry with a fixed single 256 KiB chunk. Native mode enables Mesa perf/sync
diagnostics and requires actual incremental-rendering counters, constant heap
size, full pixels/guards, completed queues and cleanup. Firmware refusal counts
are checked separately on UART. The [qualified +227 image](../../../docs/rock5-itx/MESA-HEAP-LIMIT.md)
passes both QEMU modes and two native boots: 36 actual refused requests,
36 incremental passes, 51,992 pixels, 1,024 guards, 24 completed submissions,
full cleanup and recovery/integrity. This limits the tiler heap only; sustained
load, system-memory exhaustion and automatic GPU reset remain unqualified.

`run-sustained --software` and `run-sustained --native` are a new candidate for
continuous drawing in retained contexts: one permits heap growth, the next
fixes one chunk. Native mode requires at least sixty seconds of accumulated
draw/fence/readback time and 1,024 frames per context, with bounded frame and
wall-time limits. Software mode uses 32 frames per context. Each actual RGBA
readback is encoded losslessly as runs and independently expanded and checked
by `sustained_validation.py`, together with guards, timing, native heap
observations, incremental-pass counters, completed queues and cleanup. The
probe compiles; full QEMU/native qualification is pending. This does not claim
GPU utilization, thermal limits, concurrency, conformance or reset recovery.

The first +229 candidate passed both two-boot QEMU modes, but the NanoKVM
stopped responding and restarted during the first native sustained context.
All 5,368 complete captured frames matched; required duration and cleanup were
not reached. Its source, binaries and partial output are preserved. The next
candidate line-buffers stdout, reducing the many small writes used to encode
each frame while retaining every output byte and newline flush. The exact
controller-reset cause remains unresolved; native qualification is still open.

The buffered +230 attempt kept the NanoKVM responsive and rendered 26,511
correct frames over 60.001278 seconds of measured work, but rejected a heap
that grew from seven chunks after two frames to eight by the end. Normal
shutdown, recovery and independent storage checks passed; the native trial
remains incomplete. Neither Mesa nor the kernel growth policy promises that
two frames establish a final heap size. Protocol version 2 therefore samples
the heap every 128 frames and requires at least 20 seconds of measured rendering
and 1,024 frames without further growth, in addition to the one-minute workload.
New growth extends observation within the original frame/wall limits. The fixed
policy still requires one chunk throughout. The host checks sample order, sizes
and timestamps against actual frame timing; all cleanup checks remain required.
