# Termination during pending graphics work

The hrev60097+236 USB image passes termination of a graphics process with work
still pending at the kernel's client-close boundary on two native ROCK boots.
The surviving process renders through its original context, a fresh process
renders afterward, and all allocation counts return to baseline. All fourteen
completed frames, 90,986 pixels and 1,792 guard bytes pass independent checks.

This extends the earlier [completed-work lifetime](MESA-LIFETIME.md) and
[concurrent-application](MESA-CONCURRENCY.md) fixtures. It qualifies the existing
cleanup behavior with a new close-time diagnostic; scheduling and Mesa's native
backend are unchanged. GPU fault/reset recovery and native display control
remain open. The private Mesa package still presents windows through CPU
bitmaps and the EFI framebuffer.

## Workload and close observation

Three separate processes own independent GLES 3 contexts. The survivor first
renders a checked frame. A second process calibrates a finite integer fragment
shader on a 97 by 67 RGBA8 framebuffer, increasing its per-pixel iteration
count through 1,024, 4,096, 16,384 and 65,536. Every completed frame is read back
with guards. The guest checks it using affine exponentiation; the independent
host oracle uses a linear recurrence to derive the expected bytes.

The second process repeats the last checked workload and seed, flushes it and
queries a fence with zero timeout. After `GL_TIMEOUT_EXPIRED`, its parent
terminates it with `SIGKILL`. The kernel logs queue state under the scheduler
lock immediately before marking the closing client's queues for destruction.
The host matches this record to the terminated worker's actual submission log
from the same boot, including a command-bearing pending sequence.

| Observation | First boot | Second boot |
| --- | ---: | ---: |
| Final checked calibration interval | 139.911 ms | 129.343 ms |
| Calibrated iterations per pixel | 65,536 | 65,536 |
| Queue handle at close | 58 | 58 |
| Submitted / completed at close | 11 / 10 | 11 / 10 |
| Pending / current sequence | 1 / 11 | 1 / 11 |
| Queue error / failed sequence | 0 / 0 | 0 / 0 |
| Kill request to survivor draw start | 135.791 ms | 147.003 ms |
| Completed frames checked | 7 | 7 |

Queue handles are unique within each boot; their reuse after reboot does not
combine observations across boots. A zero-time fence result alone would not
establish the close-time state, because completion can race process cleanup.
Both the fence and the kernel observation are required here.

The calibration interval covers application draw, fence wait and readback. The
kill-to-survivor interval also includes process reaping, copying its saved log
and allocation checks; it is an upper bound on cleanup time. Neither is a
GPU-only timer. The `current` field identifies the scheduler's selected job,
not an independent hardware-busy sample. Active work can finish normally while
close waits for queue retirement. This test does not establish immediate
preemption or recovery from a GPU fault or hang.

After termination, the survivor renders a different seed and exits normally.
A fresh process then renders and exits. Each two-process checkpoint has two
heaps with ten default 2 MiB chunks. The survivor returns to its earlier
allocation state; after normal exits, buffers, VMs, heaps, synchronization
objects and kernel areas return to zero apart from the observer's own client.
There are no heap-growth or refusal events in this new workload.

## Validation

All seventeen Mali host test groups pass, including the production queue
runtime under address and undefined-behavior sanitizers. The first host run
exposed missing unsigned format macros in the host OS shim; its failed log is
retained, the definitions were added, and the complete suite then passed.
The full ARM64 Haiku build and fresh 27-asset Mesa package pass. Reconstructed
sources match the previously qualified build: 11,272 Mesa files, 258 GLVND
files and eleven symlinks. SDK/runtime exported ABI checks pass.

Both QEMU exception modes pass two boots each, with the earlier graphics
regressions and the new software process-lifecycle fixture. Its sixteen frames,
103,984 pixels and 2,048 guard bytes match across all four boots. QEMU uses
32 shader iterations and does not establish pending native GPU work. All
24 comparisons of shared software/native frame hashes match. Thirty-two
corrupted software transcripts and forty-eight corrupted native transcript/UART
cases are rejected.

Both native boots retain the earlier kernel, offscreen, EGL window, OpenGL Kit,
pipeline, lifetime, heap-growth, fixed-heap and concurrency regressions.
All 38 GPU runtimes finish with zero errors, faults, fatal events and retained
memory. The pending fixture accepts 42 submissions across both boots; twenty
final completion records belong to the normally exiting survivor/fresh workers.
No final userspace completion record is claimed for the terminated workers.

All eight graphics fixtures log to RAM before retrieval through the existing
paced transfer mechanism. All sixteen native captures (7,716,707 bytes) and
32 software captures (14,334,705 bytes) pass whole-log and piece hashes,
exit-status checks and clean unmounts. All six required HDMI images were
visually reviewed. The second boot's initial image caught the boot splash;
its retained later window-completion image shows the actual desktop, as allowed
by the assessor frozen before testing.

Normal reboot, orderly shutdown and automatic recovery pass. The NanoKVM boot
identity remains unchanged. Shutdown includes 25.557 seconds of UART silence,
39.019 seconds of off-state observation and detached USB before switching to
the read-only recovery image. Independent Linux checks preserve the eMMC
filesystem, both test-file hashes and all three reference regions. The recovery
image's checksum and read-only mapping are verified.

## Source and evidence

Compiled source: `5b8713595ca70f8386cfcc0e5769caa8a0c42ab0` (+236). The diagnostic
is in `CsfRuntime.cpp`; the fixture, validator and launcher are in
`tools/rock5-itx/mesa`. The native evidence preserves 124 source/control/binary
files, with 47 source files checked against the compiled Git revision.

All paths below are under `/mnt/HaikuWork`:

- Stage: `artifacts/mali-pending/20260915T220246Z-dde0ca`.
- Fresh Mesa build: `artifacts/mali-pending-build/20260915T220651Z-c933a6`.
- Full Haiku build: `artifacts/build-20260915T220832Z.log`.
- Image: `artifacts/mali-pending-image/20260915T221014Z-53e6cf/manifest.json`,
  SHA-256 `10f059125b1cddf26fbaa798b1179db5d98a6e5cfa8d8a709c26bebd3b78b3d2`.
- EL1: `artifacts/qemu-shell/20260915T221015Z-6e345a`.
- EL2: `artifacts/qemu-shell/20260915T221626Z-45c971`.
- Native qualification: `artifacts/automated-mali-pending/20260915T222213Z-8e4687/qualification.json`.
- Interactive session: `artifacts/interactive/20260915T222226Z-af8a84`.
- Independent file readback: `artifacts/emmc-file-readback/20260915T223519Z-13606d`.
- Independent regions: `artifacts/emmc-read-reference/20260915T223540Z-a7300e`.

The two actual pending-work transcripts have SHA-256
`28bfeadd7467016a0c1f777c78542e9e3126d7a1565cbfca1d68d1372062d2c1` and
`648f6a825be49e3301c06e89c5864f14630266105c5a02360c8cac1fe016c4ac`.

The used NanoKVM image was archived, verified and removed from its temporary
remote location. Its local archive receipt is
`artifacts/nanokvm-image-archive/20260915T223752Z-da67de/result.json`; the expanded
352,321,536-byte image has SHA-256
`a8a513f65bba99e58326ee5c0dbd01111d74c03a7f68e37643ba3ef72c104b65`.
The read-only recovery mapping is restored, with 430,702,592 bytes free on NanoKVM.
