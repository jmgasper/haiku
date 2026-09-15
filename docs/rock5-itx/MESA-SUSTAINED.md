# Sustained Mesa rendering

The +232 USB image qualifies continuous checked rendering in retained GLES
contexts on two native ROCK 5 ITX boots. All 91,648 frames, 595,620,352 RGBA
pixels and 11,730,944 guard bytes pass. Every one of 183,304 GPU submissions
completes, and all allocation counts return to baseline after each context.
The four contexts total 240.615917 seconds of measured draw/fence/readback work.

| Boot | Heap policy | Frames | Render seconds | Stable frames | Stable render seconds |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | Growth allowed | 35,456 | 60.075299 | 35,328 | 59.847922 |
| 1 | Fixed one chunk | 15,744 | 60.042220 | 15,742 | 60.030228 |
| 2 | Growth allowed | 26,624 | 60.035550 | 26,496 | 59.730602 |
| 2 | Fixed one chunk | 13,824 | 60.462848 | 13,822 | 60.444822 |

Each boot first permits growth to at most 32 chunks, then fixes the heap at
one chunk. Both growing contexts reach eight 256 KiB chunks by the first
128-frame checkpoint and remain there: 2,101,248 bytes including the heap
header. Fixed contexts remain at 266,240 bytes and complete 78,377 and 68,819
incremental passes after the same numbers of firmware allocation refusals.
All 34 driver runtimes, including earlier fixtures, end with zero fault/fatal
status and no retained allocations.

## Workload and observation

The GLES fixture alternates 8,192 and 16,384 one-pixel quads using an explicit
786,432-byte vertex buffer and a 97 by 67 RGBA8 framebuffer. Every frame waits
for its fence, reads the complete image, checks two 64-byte guards and emits
lossless runs of the actual RGBA bytes. The host expands and checks all pixels;
no expected image is substituted for a readback. Each native context requires
at least sixty seconds of summed draw/fence/readback time and 1,024 frames,
within 65,536-frame and 180-second wall limits. CPU validation and log encoding
are outside the accumulated render interval. There are no duration sleeps.

Protocol 2 samples native heap state every 128 frames. Completion additionally
requires twenty seconds of measured rendering and 1,024 frames since the last
observed growth. The host independently checks every checkpoint's order,
allocation size and timestamp against actual frame durations. Further growth
extends observation within the same upper bounds. Fixed heaps must stay at one
chunk throughout. Native property, firmware, queue and allocation evidence is
required along with all pixel and guard checks.

Full stdout/stderr is saved in a dedicated Haiku RAM filesystem during the
workload. The existing staged transfer path retrieves pieces no larger than
8 MiB at 256 KiB/s after rendering finishes. Guest, NanoKVM and host hashes,
piece sizes and the whole-log hash must match before rendering validation.
All eleven native pieces pass; the complete logs are 44,202,408 and 34,996,101
bytes. RAM filesystem cleanup, ordinary reboot, orderly shutdown and guarded
recovery all succeed. NanoKVM stays on the same boot throughout.

This remains a bounded fixture using private libraries. It does not qualify
concurrent GPU applications, termination during pending graphics work,
automatic GPU reset, conformance, thermal/DVFS policy, performance parity or
native display mode control. Window presentation continues through CPU bitmaps
and Haiku's EFI framebuffer.

## Regression and failure evidence

Both EL1 and EL2 QEMU modes pass two boots each, with actual exception levels
verified from UART. Their 256 software frames, 1,663,744 pixels and 32,768 guards
pass; all RGBA aggregates match the preceding protocol-2 software runs. QEMU
also verifies the actual RAM log and piece hashes and successful unmount.
Thirty-five corrupted software sustained transcripts, fifty-seven corrupted
native transcripts, twenty-two OpenGL Kit cases and twelve damaged capture
cases are rejected. All earlier offscreen, window, OpenGL Kit, pipeline,
process-lifetime, heap-growth, fixed-heap and kernel GPU fixtures pass again.
Six actual native HDMI images were independently viewed. ROOBI independently
checks eMMC files, FAT integrity and three reference regions after recovery;
the recovery image retains its original hash.

The preceding attempts remain preserved:

- +229 passed QEMU but NanoKVM restarted during native output capture. Its
  5,368 complete frames were correct; duration and cleanup were incomplete.
- +230 completed 26,511 correct frames and 60.001278 seconds of measured work,
  but its requirement that heap size stop changing after two frames was too
  strict. A later allocation increased the heap from seven to eight chunks.
  Mesa and the kernel policy do not guarantee a final size after two frames.
- +231 added periodic stability observations. Its growing context completed
  35,840 frames, but NanoKVM restarted during the fixed context. All 46,699
  complete captured frames were correct; the whole trial remained incomplete.
- The first RAM capture wrapper hit a QEMU VFS assertion on unmount. RAMFS had
  not released the root vnode reference acquired when publishing its root.
  +232 releases that reference before deleting the hierarchy. All four QEMU
  boots and both native RAM captures now unmount successfully.

The exact NanoKVM restart cause is unresolved. RAM capture avoids the long
simultaneous USB/Ethernet output relay used by those interrupted attempts.
No networking driver or transfer implementation changed for this milestone.

## Source and evidence

Paths below are relative to `/mnt/HaikuWork`.

- Qualified source: `7040085eab3ecdf5987e2f6958503bf64a488312`, `hrev60097+232`.
- Full build: `artifacts/build-20260915T200009Z.log`.
- Mesa package: `artifacts/mali-sustained-build/20260915T192318Z-009529/package/manifest.json`.
  Its unchanged +231 libraries/probes are reused; current runtime exports
  independently match all 2,740 libroot and 11,737 libbe SDK exports.
- Image: `artifacts/mali-sustained-image/20260915T200221Z-3c46d5/manifest.json`,
  SHA-256 `158cf3ed6b38e83d91ac4edd450408ee5fddb80918f1aff9400696d302db196a`.
- QEMU EL1: `artifacts/qemu-shell/20260915T200232Z-8e4976`;
  EL2: `artifacts/qemu-shell/20260915T200731Z-5d03b2`.
- Native qualification and complete logs:
  `artifacts/automated-mali-sustained/20260915T201225Z-47e488`.
  Its frozen source packet has 107 files; 37 Mesa bundle files and the RAMFS
  change match the qualified commit. The capture helper is preserved with the
  controllers and is published unchanged in the source bundle.
- Native UART, command and transfer transcripts:
  `artifacts/interactive/20260915T201235Z-486717`.
- Controllers, reconstruction/ABI checks and mutation receipts:
  `artifacts/mali-sustained/20260915T200004Z-4f3ff9`.
- eMMC files: `artifacts/emmc-file-readback/20260915T203449Z-ad41da`;
  regions: `artifacts/emmc-read-reference/20260915T203511Z-280c6e`.
- Earlier native failures: `artifacts/mali-sustained/20260915T180839Z-25e334`,
  `20260915T185449Z-8f9d48` and `20260915T192318Z-61b07e`.
- Original RAMFS assertion:
  `artifacts/mali-sustained/20260915T195208Z-27fb7e/unmount-failure-preservation`.
- Verified used-image archive: `artifacts/nanokvm-image-archive/20260915T203709Z-ceb421`,
  SHA-256 `cb3ab70622982da9e9eb5b1489d1417cadf54c613b0a0a2b396c3481f92ef403`.
