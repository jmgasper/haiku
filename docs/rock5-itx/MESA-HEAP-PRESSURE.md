# Firmware tiler heap growth

The `hrev60097+224` USB image qualifies actual firmware-requested tiler heap
growth on two native ROCK boots. Four fresh graphics contexts each grow from
one chunk to four, then seven. All 24 firmware requests receive additional
memory; all 51,992 pixels, 1,024 guard bytes and 24 GPU submissions pass.
Allocation baselines, normal reboot/shutdown, recovery and independent storage
and recovery-image integrity checks pass.

This tests the successful growth path under a bounded workload. Refused
allocations, incremental rendering, sustained workloads, GPU fault/reset
recovery and native display control remain open. Window presentation still
copies GPU-rendered bitmaps into the EFI-framebuffer desktop.

## Workload and acceptance

The [source bundle](../../tools/rock5-itx/mesa/README.md) adds the pressure probe,
launcher and independent validator. It uses the previously qualified kernel
and rendering-library implementations. Mesa's supported process-local options
select 256 KiB chunks, one initial chunk and a maximum of 32 chunks before EGL
initialization. The native observer verifies that the reduced initial allocation
actually took effect.

Each context uploads an explicit 786,432-byte vertex buffer containing 98,304
vertices. Two triangles form each nondegenerate one-pixel quad. The first draw
uses 8,192 quads; the second uses 16,384. Geometry covers every pixel of the
97x67 RGBA8 target and repeats with the same colour. Red, green, blue and cyan
distinguish the four frames across two context lifetimes per boot.

Every frame waits for a fence, reads all RGBA bytes with guards on both sides,
and finishes GPU work. The host independently checks the complete image and
guards. All four frame hashes match across four software QEMU runs and both
native runs. Native provenance requires Mali-G610/Panfrost, Mesa 25.3.6,
OpenGL ES 3.1, cached GPU properties and completed submission queues.

The following allocation sequence repeats in all four native contexts:

| Observation | Allocated chunks | Heap bytes including context page |
| --- | ---: | ---: |
| Before dense geometry | 1 | 266,240 |
| After 8,192 quads | 4 | 1,052,672 |
| After 16,384 quads | 7 | 1,839,104 |
| After context destruction | 0 | 0 |

UART evidence independently records six growth events and six successful
extensions per context, with no declined requests. These counts match the
live allocation increase. The full baseline also covers buffers, VM generations,
page tables, synchronization objects, kernel areas and user mappings. All
26 runtime teardowns, including the earlier regressions, report clean engine,
firmware, fault and retained-page state. The per-job hardware deadline remains
five seconds. Recorded draw/finish/readback durations are 3,577–14,660 us;
these short measurements are not a sustained performance result.

Both two-boot QEMU modes pass with explicit softpipe. Twenty-three corrupted
software transcripts and 31 corrupted native transcripts are rejected,
including changed pixels/guards, geometry sizes, renderer identity, heap counts,
completion records and allocation baselines. Previous process, pipeline,
OpenGL Kit, EGL window, offscreen and kernel GPU fixtures all pass. Both desktop
captures and all four window captures pass visual review.

Fresh reconstruction verifies identical contents for 11,272 Mesa files,
258 libglvnd files and eleven symlinks against the qualified +221 libraries.
The unchanged kernel retains its 147 previously passing host checks. All
2,740 libroot and 11,737 libbe export entries in the +207 SDK remain compatible
with the +224 runtime. The native evidence freezes 89 source, controller,
script and unstripped binary files; their hashes are rechecked after the run.

The comparison baseline remains Linux 6.18.52, Mesa 25.3.6 and the previously
qualified Mali firmware. The growth protocol is compared with the local pinned
Panthor sources; this new geometry fixture was not separately run on Linux.

## Retained first software failure

The +223 probe generated coordinates from `gl_VertexID`. Its first QEMU draw
returned 682 red pixels and 5,817 black pixels instead of the complete red
image. No native deployment occurred. The working diagnosis is the non-LLVM
software vertex interpreter restarting IDs at its split-draw boundary: the
linear fetch path omits the split's start offset when invoking the interpreter.
The exact source, unstripped libraries/probes, failed RGBA data and relevant
software-renderer source files are preserved.

The +224 fixture uses explicit vertex coordinates and passes the unchanged
pixel oracle. This does not fix or qualify large `gl_VertexID` draws; that
software-renderer limitation remains a separate issue.

## Evidence

Paths are relative to `/mnt/HaikuWork`:

- Source: `56c585009d8f6654dccb872ddec8e09bf2d2ccda`.
- Package: `artifacts/mali-heap-pressure-build/20260915T164042Z-80b5f7/result.json`.
- Full Haiku build: `artifacts/build-20260915T164110Z.log`.
- Image: `artifacts/mali-heap-pressure-image/20260915T164313Z-e0303e/manifest.json`,
  SHA-256 `d94b960c80514cde8f6c12d9e3bef103984777f9cf1d5e595c0194395aa42758`.
- EL1/EL2 QEMU: `artifacts/qemu-shell/20260915T164314Z-460e59` and
  `artifacts/qemu-shell/20260915T164708Z-af80b7`.
- Native qualification, complete images, source and renderer comparison:
  `artifacts/automated-mali-heap-pressure/20260915T165117Z-0288f7`.
- UART, transcripts and desktop frames 008/029:
  `artifacts/interactive/20260915T165131Z-803cea`.
- Independent eMMC files/regions: `artifacts/emmc-file-readback/20260915T170102Z-262f89`
  and `artifacts/emmc-read-reference/20260915T170117Z-3c63ee`.
- Controllers and validator corruption checks:
  `artifacts/mali-heap-pressure/20260915T164042Z-b8bd39`.
- Failed +223 source and software evidence:
  `artifacts/mali-heap-pressure/20260915T161947Z-45df69/failure-preservation/manifest.json`,
  QEMU `artifacts/qemu-shell/20260915T163535Z-3e867f`.
- Verified used-image archive: `artifacts/nanokvm-image-archive/20260915T170250Z-511881/result.json`,
  SHA-256 `31cc105e18ade56804b41f7a17f1a9fd7b907c492c592932f2da5175eaea1136`.
