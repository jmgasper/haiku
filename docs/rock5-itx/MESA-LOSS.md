# Shared Mesa contexts after GPU command loss

The hrev60097+243 USB image qualifies reset notification and cleanup for two
live, shared Mesa contexts on two native ROCK boots. Both contexts receive one
unknown-attribution reset notification, ignore further rendering and readback,
and close completely. Fresh GLES contexts then render the full pipeline
without rebooting Haiku. Across the two boots, all 28 before/fresh frames,
181,972 pixels, 3,584 guard bytes and 104,496 ignored readback bytes pass.

This is a finite command-fault test after the shared-context draws finish.
Arbitrary hang recovery, innocent-context preservation, robust buffer-access
conformance, general application compatibility, thermal/DVFS and native display
control remain open. Presentation still uses CPU bitmaps and the EFI
framebuffer. The separately prepared GLTeapot trial is not part of this image.

## Driver and Mesa behavior

The version-1 reset-state query is 32 bytes and requires an owned queue. It
distinguishes healthy operation, local failure, recovery pending, quiescent
cleanup and unrecoverable failure. It takes the existing runtime lock and
performs no hardware access, state mutation, allocation or recovery wait. The
existing 96-byte queue-info interface is unchanged. Inputs require zeroed
output/reserved fields. The Mesa bridge updates its output values only after a
successful query.

Mesa reports unknown reset attribution. Pending or failed recovery continues
to report loss; each context consumes a completed notification once. Lost
contexts use Mesa's lost-context dispatch instead of attempting to recreate a
faulted VM immediately. Full EGL display teardown releases the bootstrap queue
and both drawing queues. All affected queues must close before a fresh runtime
starts; quiescent reset does not preserve other contexts automatically.

## Actual native result

Two GLES contexts remain current on separate application threads and share a
program and vertex buffer. Each first renders a complete 97-by-67 checked image.
A separate owned queue completes a store, submits 32 finite 1 MiB NOP streams,
and then the deliberate invalid CS instruction used in the earlier recovery
fixture. One raw error fence and verified quiescent reset are required.

| Observation | First boot | Second boot |
| --- | ---: | ---: |
| Mesa bootstrap / draw / draw queue handles | 68 / 69 / 70 | 68 / 69 / 70 |
| Completed drawing submissions per Mesa draw queue | 3 | 3 |
| Raw queue / submitted / completed / current / pending | 71 / 34 / 33 / 34 / 1 | 71 / 34 / 33 / 34 / 1 |
| Reset-complete IRQ interval | 8 microseconds | 8 microseconds |
| Reset function interval | 112 microseconds | 112 microseconds |
| Contexts reporting `0x8255`, then `0x0000` | 2 | 2 |
| Ignored readback bytes unchanged | 52,248 | 52,248 |
| Complete before/fresh pixels / guards | 90,986 / 1,792 | 90,986 / 1,792 |
| Fresh normal GPU runtimes after this fault | 2 | 2 |

The measured intervals cover the reset interrupt and reset function, not the
whole recovery process. Equal queue handles after reboot are separate
observations. Both boots retain fatal value `0xff49` at `0x120000000`, matching
the deliberate command fault. Engine/transition/interrupt-mask state is idle;
AS0 and AS1 table/attribute registers are zero, configuration is one and status
is inactive. The original runtime error remains an error, recovery is verified,
and retained memory is zero.

Both contexts print their reset return values before asserting. Their ignored
read, clear and sync calls report `GL_CONTEXT_LOST` (`0x0507`); the sync status
is signaled and every output canary remains unchanged. After thread/context/
surface/display teardown, all measured allocation counters return to baseline.
Two new contexts then pass texture, depth, stencil, blend, scissor and
render-to-texture checks with complete pixel and guard comparisons.

## Retained failed trials and corrections

The +241 trial completed kernel reset and address-space cleanup, then failed
the application's reset-status assertion. That binary did not print the two
return values, so their values are not measured native evidence. An
actual-function host reproducer separately found duplicate Mesa state-tracker
delivery: `8255,8255,0000` before the fix and `8255,0000,0000` afterward. The
Haiku polling path now marks the context lost without caching the same event
again. Genuine callback handling and non-Haiku preprocessed function bodies
remain unchanged. A validator correction also admits exactly the normal
empty-runtime firmware probe before initialization, while rejecting malformed,
duplicate, late or unexpected bridge errors.

The +242 trial passed all first-boot graphics tests, including the printed
reset pairs and fresh rendering. Its second boot stopped in the instruction-
cache fixture: eight independently allocated aliases happened to share the
writer's address bit 12. No alias comparison ran and no data mismatch was
observed. +243 reserves eight consecutive pages so exactly four aliases have
the opposite bit. Both native inventories now finish all 129,024 comparisons
with zero mismatches. Code generation, cache operations and CPU coverage are
unchanged. Both failed trials retain source, image, serial logs, orderly
shutdown witnesses, passing storage readbacks and verified local archives.

## Validation and recovery

The full +243 Haiku build passes. Its only change from +242 is the cache
fixture layout. All 85 Mesa bundle pins and all 31 package assets match the
rebuilt +242 package. Reconstruction of that package compared 11,272 Mesa
files, 258 GLVND files and eleven symlinks with +241; only the intended Mesa
state-tracker function changed. The seven actual Mesa functions and eighteen
unchanged kernel host groups pass their source-verified checks. SDK/runtime
exports are compatible, with 2,740 libroot and 11,737 libbe exports checked.

Both QEMU exception modes pass two boots each. Software runs require the
native GPU to be absent and skip reset injection; QEMU does not emulate this
GPU. All 56 shared/fresh software frames, 363,944 pixels and 7,168 guards pass.
All 112 shared software/native frame-hash comparisons match. Twenty-nine
altered software transcripts and 107 altered actual native transcript/UART
inputs are rejected.

The earlier kernel, offscreen, EGL window, OpenGL Kit, pipeline, process-
lifetime, heap-growth, fixed-heap, concurrency, pending-close and raw-reset
regressions pass. Across both boots, 52 runtime records comprise 48 normal
completions and four deliberate failures with verified recovery. The earlier
pending-close test still establishes actual unfinished work at client close.

All ten graphics fixtures capture to RAM before paced, checked retrieval.
The 20 native captures total 10,721,026 bytes; the 40 software captures total
19,873,478 bytes. Piece/whole hashes, process exits and unmounts pass. Both
desktops and both EGL/OpenGL Kit windows were inspected in six actual NanoKVM
HDMI images.

Normal reboot, orderly shutdown and automatic ROOBI recovery pass. NanoKVM's
boot identity is unchanged. Before switching USB media, the shutdown witness
records 25.841 seconds of UART silence, 40.281 seconds of off-state observation
and detached USB. Independent Linux checks preserve the eMMC filesystem,
both test-file hashes and all three reference regions. The recovery image's
checksum and read-only mapping pass.

## Source and evidence

Compiled source: `f46f335b0980e7fe6c4a6fa93fe77bef79269c76` (+243).
Reset ABI: `1caad9652a7b4c33381c2e1d14bd028d7a7600a9` (+240).
Mesa package source: `5d31aa1763f1f45651935558c274d4d6cd8c03b0` (+242).
The native candidate preserves 194 source/control/input files, including 97
checked against the compiled Git revision. Before QEMU, 158 files were frozen.

All paths below are under `/mnt/HaikuWork`:

- Stage: `artifacts/mali-context-loss/20260916T021958Z-a97655`.
- Mesa package: `artifacts/mali-context-loss-build/20260916T014308Z-daff26/package/manifest.json`.
- Full Haiku build: `artifacts/build-20260916T021808Z.log`.
- Image: `artifacts/mali-context-loss-image/20260916T022117Z-5aefa8/manifest.json`,
  SHA-256 `b49e25dfb3a4f8d63a828b59e6252a3a816ab62037d42505d8027acf494b74e5`.
- EL1: `artifacts/qemu-shell/20260916T022118Z-5ff9bb`.
- EL2: `artifacts/qemu-shell/20260916T022932Z-8b66cc`.
- Native qualification: `artifacts/automated-mali-context-loss/20260916T023725Z-d2d48b/qualification.json`.
- Interactive session: `artifacts/interactive/20260916T023747Z-907073`.
- Independent file readback: `artifacts/emmc-file-readback/20260916T025449Z-f5b680`.
- Independent regions: `artifacts/emmc-read-reference/20260916T025503Z-ee63b7`.
- Failed +241: `artifacts/mali-context-loss-fix/20260916T012724Z-b8bc46/failed-candidate-plan.json`.
- Failed +242: `artifacts/mali-cache-fixture-fix/20260916T021632Z-b24927/failed-plan.json`.

The two actual loss transcripts have SHA-256
`0a1f0aff0512ef0b869978879972937509174a012507d2159577b5953ecc0ff8` and
`f27e16ca4e067dfec9ff4058f7b723f88b9fd35371f86e015c92d66b51bef7b3`.

The used NanoKVM image was archived, verified and removed from its temporary
remote location. Receipt: `artifacts/nanokvm-image-archive/20260916T025643Z-065eb2/result.json`.
The expanded 352,321,536-byte image has SHA-256
`5b534a58202abdfa6e5a003ccb5ae990dc78b9fe8854646ae02e0ec1110b16d8`. The read-only recovery mapping is retained,
with 430,702,592 bytes free on NanoKVM.
