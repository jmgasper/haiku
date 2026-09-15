# Concurrent graphics applications

The +234 USB image passes a bounded concurrent GLES3 workload on two native
ROCK 5 ITX boots. Each boot starts two independent graphics processes with
retained contexts and completes 64 synchronized draw rounds. Actual
draw/fence/readback intervals overlap in all 128 paired rounds across both
boots. All 260 complete frames, 1,689,740 pixels, 33,280 guard bytes and
532 accepted/completed GPU submissions pass.

The second process exits normally. The survivor draws again with its
retained context, then closes. A fresh process renders after all allocation
counters return to baseline. Default Mesa tiler heaps have five 2 MiB chunks
per context: two live heaps use 20,979,712 bytes including their context pages,
remain stable in this workload, and return to zero after both processes exit.
The independent observer remains the only open client.

This measures overlapping application work. The driver still schedules one
active GPU job at a time; this is not a parallel-GPU-execution claim. Each
frame draws 8,192 or 16,384 explicit one-pixel quads into a 97x67 RGBA8 target,
with process/frame-dependent colors and guard values. Full actual pixels are
losslessly recorded and checked independently on the host. The synchronization
barrier and log encoding are outside measured rendering intervals.

Both two-boot emulator modes pass the software version (eight paired rounds
per boot), checking 72 frames, 467,928 pixels and 9,216 guards. All shared
frame hashes agree with native rendering. The validator rejects 27 software
and 36 native corruptions of actual passing transcripts.

The first native attempt lost NanoKVM connectivity during the earlier
heap-pressure fixture, before the new concurrency test ran. NanoKVM rebooted;
the exact cause remains unknown. Automatic Linux recovery and independent
storage/image checks passed. That incomplete attempt and its used USB image
are preserved; it is not qualified.

The second attempt extends the previously qualified RAM-capture method to
seven graphics fixtures. Each test completes before its log is downloaded
through the existing paced staged transfer. Whole-log and per-piece hashes
are verified before RAM unmount. All 28 emulator captures and 14 native
captures pass. The networking and transfer implementations are unchanged.

Earlier offscreen, window, OpenGL Kit, pipeline, process-lifetime, heap-growth,
fixed-heap and kernel regressions pass. All 34 native GPU runtimes finish
without faults or retained allocations. The first inventory HDMI snapshot
showed the boot splash; a later controller-recorded same-boot snapshot shows
the normal desktop. Both are retained, alongside actual window/GLView
captures and second-boot desktop evidence. The final assessment uses that
later image and actual initial heap counts; its two corrections and original
frozen checker are preserved separately.

Native normal reboot/shutdown, verified Linux recovery and independent
eMMC file/region and recovery-image integrity checks pass.
Pending-work termination, GPU fault/reset recovery, general application
compatibility, thermal/DVFS behavior and native display control remain open.
Libraries are private to the fixture, and presentation uses the existing
CPU bitmap / EFI framebuffer path.

## Reproduction and evidence

Run `tools/rock5-itx/mesa/build.py` with its pinned archives and SDK packages.
The package contains 25 checked assets, including `run-concurrency`. The
source and compiler recipes remain in this fork; binaries and full captures
stay under `/mnt/HaikuWork`.

| Evidence | Value beneath `/mnt/HaikuWork` |
| --- | --- |
| Compiled source | `ada2ee8226d43945fcf20831a48feeab8faa1f73` (`hrev60097+234`) |
| Fresh Mesa build | `artifacts/mali-concurrency-build/20260915T205614Z-21b8ac` |
| ARM64 build log | `artifacts/build-20260915T212210Z.log` |
| Image manifest | `artifacts/mali-concurrency-image/20260915T212224Z-ce8af2/manifest.json` |
| Image SHA-256 | `6840f84631090c36e0f092dd77cd236f4274a7bbf857449e1cbea065c6ea8724` |
| EL1 two-boot run | `artifacts/qemu-shell/20260915T212225Z-7de211` |
| EL2 two-boot run | `artifacts/qemu-shell/20260915T212830Z-09303f` |
| Qualification | `artifacts/automated-mali-concurrency/20260915T213404Z-eb5a95/qualification.json` |
| Native session | `artifacts/interactive/20260915T213412Z-9fe012` |
| Independent eMMC file checks | `artifacts/emmc-file-readback/20260915T214628Z-ef6e1a` |
| Independent eMMC region checks | `artifacts/emmc-read-reference/20260915T214649Z-17de50` |
| First incomplete native attempt | `artifacts/automated-mali-concurrency/20260915T211010Z-6baa96` |
| First attempt image archive | `artifacts/nanokvm-image-archive/20260915T212011Z-17e285/result.json` |
| Qualified used-image archive | `artifacts/nanokvm-image-archive/20260915T214858Z-20d2f3/result.json` |

Each native boot checked 130 frames and 266 completed submissions.
Application interval overlap totaled 433,645 us on boot one and 419,972 us
on boot two; these timings are observations, not a performance benchmark.
All 14 native logs total 6,968,045 bytes and retain their per-piece/whole hashes.
The evidence includes 115 frozen files, 42 checked Git source files, original
and reviewed qualification scripts, the assessment diff, both initial/later
first-boot HDMI observations and all six accepted visual reviews.

The source reconstruction independently compared 11,272 Mesa files, 258
libglvnd files and eleven symlinks against the previously qualified source.
The native capture adapter is reproduced in
[`graphics_capture.py`](../../tools/rock5-itx/mesa/graphics_capture.py); it
uses the unchanged qualified RAM capture integrity implementation.

The used 352,321,536-byte boot image was streamed locally, verified, then
removed from NanoKVM. Its expanded SHA-256 is
`9f26c8a1d2bcf38f0af6e4bc937c3e3d2745fc91874cfd79642027f76bd5c114`.
The read-only recovery image stayed selected and NanoKVM retained boot ID
`2597fc80-b134-4c61-9241-c7231d552eb2` throughout the qualified run.
