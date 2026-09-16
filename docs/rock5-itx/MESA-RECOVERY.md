# Bounded Mali command-fault recovery

The hrev60097+238 USB image recovers from a deliberate GPU command exception
on two native ROCK boots. Three affected jobs receive errors, the driver
verifies reset and cleanup, and all affected clients close. A fresh native
queue and fresh Mesa contexts then render correctly without rebooting Haiku.
All 24 subsequent graphics frames, 155,976 pixels and 3,072 guard bytes pass.

This is the first qualified runtime fault-recovery path. It does not establish
notification or recovery for Mesa contexts kept open across the reset,
preservation of innocent contexts, arbitrary hangs, or repeated faults within
one boot. Native display control remains open; presentation still uses CPU
bitmaps and the EFI framebuffer.

## Driver behavior

After a runtime operation fails, the scheduler stops new submissions and
records the affected queues under its lock. The original bounded teardown is
still attempted. After the firmware interrupt handlers have been removed and
synchronized, recovery issues a bounded GPU soft reset and requires the actual
reset-complete interrupt. The separate diagnostic reset retains its original
idle-state admission checks.

Recovery explicitly clears both firmware and application address spaces
(AS0/AS1), checks their table and attribute registers, and verifies that GPU
engines, transitions and interrupt masks are idle. The existing power cycle
must also restore all ten checked platform registers. Only this complete
result permits queue allocations to be freed when clients close. Failed or
uncertain recovery retains memory that the device could still access.

The original failed operation remains an error. The selected and queued jobs
receive their own error fences; they are not reported as successful. All
affected queues must close before a fresh runtime can start. No new public
queue ABI, direct userspace register access or networking change is involved.

## Actual native fault and reuse

Two raw descriptor clients each complete a checked store. The first submits
32 valid 1 MiB NOP streams, then one mapped command containing opcode `0xff`.
That opcode is unassigned in the pinned Mesa 25.3.6 command XML. A queued job
from the same client and a job from the second client depend on the faulting
job's output fence. The test requires all three jobs to have been admitted
before the failure.

Both boots report fatal value `0xff49` at GPU address `0x120000000`. The pinned
Linux 6.18.52 Panthor definitions decode this as exception type `0x49`
(`CS_INSTR_INVALID`) and data `0xff`, consistent with the submitted opcode.
Each boot's UART records are matched to its actual userspace queue handles
and sequences.

| Observation | First boot | Second boot |
| --- | ---: | ---: |
| Faulting / affected queue handles | 61 / 62 | 61 / 62 |
| First queue submitted / completed / current / pending | 35 / 33 / 34 / 2 | 35 / 33 / 34 / 2 |
| Second queue submitted / completed / current / pending | 2 / 1 / 0 / 1 | 2 / 1 / 0 / 1 |
| Jobs receiving `B_IO_ERROR` fences | 3 | 3 |
| Reset-complete IRQ interval | 8 microseconds | 7 microseconds |
| Reset function interval | 112 microseconds | 111 microseconds |
| Fresh checked native store submissions | 2 | 2 |
| Fresh GLES contexts / completed submissions | 2 / 30 | 2 / 30 |
| Complete graphics pixels / guards checked | 77,988 / 1,536 | 77,988 / 1,536 |

The IRQ and reset-function intervals are not the duration of the entire
recovery process. The raw fixture takes 145.229 and 134.713 milliseconds,
including preceding NOP work, waits and client cleanup. Handles are scoped to
each boot; equal numbers after reboot are separate observations.

Both address spaces finish with zero table/attribute registers, configuration
one and inactive status. Engines, transitions and masks are zero. The original
runtime error is preserved, verified recovery is flagged and retained memory
is zero. Both clients close, every measured allocation returns to its baseline,
and the fresh store queue checks 4,096 words per boot. Two new GLES contexts
then pass texture, depth, stencil, blend, scissor and render-to-texture checks.

## Validation and recovery

All eighteen Mali host test groups pass, including 35 recovery-model cases
and the production runtime's held-recovery, three-error-fence and fresh-queue
checks under address and undefined-behavior sanitizers. Initial test-runner
import and missing host format-macro failures are retained with their fixes.
The full ARM64 Haiku build, fresh 29-asset Mesa package and SDK/runtime ABI
checks pass. Source reconstruction matches the previously qualified inputs:
11,272 Mesa files, 258 GLVND files and eleven symlinks.

Both QEMU exception modes pass two boots each. The new software fixture
requires the native device to be absent and checks subsequent software
rendering; QEMU does not emulate this GPU or its reset. All 48 software frames,
311,952 pixels and 6,144 guards pass. Ninety-six shared software/native frame
hash comparisons match. Twenty-four corrupted software transcripts and sixty
corrupted native transcript/UART variants are rejected.

Both native boots retain the earlier kernel, offscreen, EGL window, OpenGL Kit,
pipeline, lifetime, heap-growth, fixed-heap, concurrency and pending-close
regressions. There are 46 runtime records: two deliberately failed and safely
recovered runtimes, and 44 normal completions. Three fresh normal runtimes
follow each fault: the store queue and two Mesa contexts. The earlier
pending-work fixture still verifies actual unfinished work at client close.

All nine graphics fixtures log to RAM before paced, checked retrieval. All
18 native captures (9,004,754 bytes) and 36 software captures (16,890,352 bytes)
pass piece/whole hashes, exit-status checks and clean unmounts. All six required
HDMI images were visually inspected: both desktops and the EGL/OpenGL Kit
test windows are visible in their respective captures.

Normal reboot, orderly shutdown and automatic ROOBI recovery pass. NanoKVM's
boot identity is unchanged. Shutdown includes 25.629 seconds of UART silence,
39.967 seconds of off-state observation and detached USB before switching to
the read-only recovery image. Independent Linux checks preserve the eMMC
filesystem, both test-file hashes and all three reference regions. The
recovery image checksum and read-only mapping are verified.

## Source and evidence

Compiled source: `23f108ce11dca9ab668c3bc681d8608d578ee134` (+238). The driver
changes are in `CsfReset.h`, `CsfRun.h` and `CsfRuntime.cpp`; the fixture,
launcher and independent validator are in `tools/rock5-itx/mesa`. The native
evidence preserves 174 source/control/input files, including 93 source files
checked against the compiled Git revision. Before QEMU, 142 files were frozen.

All paths below are under `/mnt/HaikuWork`:

- Stage: `artifacts/mali-fault-recovery/20260915T225357Z-30cdb3`.
- Fresh Mesa build: `artifacts/mali-fault-recovery-build/20260915T231405Z-30cdb3`.
- Full Haiku build: `artifacts/build-20260915T230910Z.log`.
- Image: `artifacts/mali-fault-recovery-image/20260915T231119Z-018a24/manifest.json`,
  SHA-256 `2b16d1716a64b63a0d0c3f1d34cda708f17fc4846f482de9d1c6e8604950f984`.
- EL1: `artifacts/qemu-shell/20260915T231657Z-dcedd6`.
- EL2: `artifacts/qemu-shell/20260915T233258Z-12b7c4`.
- Native qualification: `artifacts/automated-mali-fault-recovery/20260915T234018Z-1d8576/qualification.json`.
- Interactive session: `artifacts/interactive/20260915T234025Z-45c4fe`.
- Independent file readback: `artifacts/emmc-file-readback/20260915T235636Z-1c74ea`.
- Independent regions: `artifacts/emmc-read-reference/20260915T235646Z-e0df67`.

The two actual recovery transcripts have SHA-256
`a20cc1a816ca685716bad472beb2fccb335429eb716ae74a34b7ee2a0ab41582` and
`4fbf6f85d7a7c4e7fb0855360a2240f0540643530c1431b0442433d2bfa048d8`.

The used NanoKVM image was archived, verified and removed from its temporary
remote location. Its local receipt is
`artifacts/nanokvm-image-archive/20260915T235809Z-aaa56e/result.json`; the expanded
352,321,536-byte image has SHA-256
`c0a4fc59c54346d864ae15965b1bcba9f67b9c4eb4b6d75b843ee0af9a71c0a0`.
The read-only recovery mapping is restored, with 430,702,592 bytes free on NanoKVM.
