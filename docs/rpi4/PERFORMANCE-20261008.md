# Raspberry Pi performance, 8 October 2026

This is a new twelve-hour performance session starting at 09:22 UTC on
8 October (20:22 in Hobart), scheduled through 21:22 UTC. The scope is
memory efficiency, CPU efficiency and GPU performance on the native Pi,
beginning with measured, inexpensive changes. This page records results
as they are verified; it does not replace the earlier `PERFORMANCE.md`.

## Starting state

The live board identifies itself as the 4 GB Pi 4B revision 1.5 and runs
`hrev60206+750`, built 8 October at 13:59. It has four Cortex-A72 cores at
1.8 GHz, V3D 4.2.14.0 at 500 MHz and Mesa 25.3.6. Ethernet now uses
`192.168.1.213`; the saved `.214` address is obsolete for this boot.
The 128 GB card currently has a 3.5 GiB BFS filesystem and the installer
still offers its first storage step. No storage expansion was requested
or performed in this performance session.

The active desktop is 1920x1080 on HDMI1. HDMI0 is disconnected according
to the display driver; NanoKVM provides serial and recovery access but
no HDMI picture. Native screenshots use app_server. AirTop's 640x480 DSI
panel is running. Screen blanking is stopped before visible graphics tests.
The starting core services are healthy, with no crashed teams or kernel
faults in the current boot. About 409 MiB of physical RAM is in use,
including about 127 MiB of cache. A twenty-second idle sample is about
1.5% of all four cores, mostly AirTop and its offscreen drawing in app_server.

The installed EGL library's SHA-256 is
`cf68fae91254d6e10a53384922f41277d5dbf910303ab7be53e1a1a7f0113bcd`.
It matches the pre-session local Mesa stage. The source branch is `rpi4`
at `03f2e9bff9`; uncommitted DSI touch-probe changes existed at session
start and are preserved separately from this work.

Evidence: `/mnt/HaikuWork/rpi4/evidence/performance-20261008`, including
`inventory.json`, `boot-drivers.json`, `idle-baseline.json`,
`graphics-baseline.json`, `throughput-baseline.json` and `desktop-awake.png`.
The baseline passes the GLES clear/triangle test, 48 readback combinations
and 40 texture ownership rounds. A 1280x720 native EGL window completes
98 draws/s for 240 frames; this measures completed BView draws, not monitor
refresh. Memory copies already use the prior optimized ARM64 libroot.

## V3D buffer-cache table growth

Mesa's size-indexed buffer cache allocated a replacement linked-list-head
array each time a larger BO was retired. The old array remained a child
of the screen allocation until screen destruction. Incremental buffer
growth therefore retained a series of increasingly large metadata arrays,
even after all cached GPU buffers had aged out.

`mesa-v3d-cache-growth.patch` releases the old array after relinking its
nonempty lists, grows capacity geometrically, and leaves the existing
cache intact when metadata allocation fails (the new BO is simply freed).
Cache lookup now checks table capacity under the same mutex as growth.

The native `rpi4_cache_growth_probe` keeps one GLES3 context alive while
creating 1,024 buffers, growing from 1 MiB in 8 KiB increments. It verifies
the first and last 64 bytes of each buffer through write/read maps, and
pauses every 32 buffers so that old GPU allocations expire. It records
heap residency separately from mapped GPU buffers. This is a controlled
retention test, not an estimate of every application's memory saving.

The first baseline retains 25,382,912 heap bytes after GPU-buffer retirement;
the candidate retains 2,334,720. Both keep only 16,384 bytes of mapped GPU
storage at that point. Both start with 2,187,264 heap bytes. `listimage`
confirms that the candidate process loaded the isolated candidate library.
A second baseline repeats the same memory figures exactly. The installed
candidate, launched without environment overrides, also repeats its figures
exactly: **23,048,192 bytes (22.0 MiB) less retained heap**, about 91% less
total heap residency in this specific workload. Full process residency falls
from 27,992,064 to 4,943,872 bytes after the GPU cache expires.

An old/new/new/old window comparison, 480 completed 1280x720 frames per run,
reports 95.68 / 95.57 / 94.93 / 95.36 draws per second, all with correct
pixels. This is a memory fix; these results do not establish an FPS gain.
The native growth test's CPU time varies between runs, so no CPU improvement
is claimed from it.

The candidate is now the default
`/boot/system/non-packaged/lib/libEGL_mesa.so.0`, SHA-256
`4fed15bffa9be5140aabd386b9bef9fcaf7b8a6300e6c18633e9698703f73349`.
The original checksum is verified in
`/boot/home/performance-20261008/rollback/libEGL_mesa.so.0`. Installation
used a synced rename; no kernel, boot firmware or user settings changed.
The installed library is confirmed by `listimage` and the full 1,024-step
test passes on it without a vendor override. The local Mesa stage and
ordered image-build patch series also contain the change.

The host sanitizer fixture extracts the actual cache-growth routine and
uses Mesa's list helpers with a tracked allocator. The old routine retains
2,048 tables (33,570,816 bytes); the candidate retains one 32,768-byte table
after six allocations. Nonempty lists, multiple BOs in a bucket, failed
growth, recovery after failure and cleanup pass under ASan/UBSan.
Native candidate tests also pass 48 readback combinations, 80 large texture
ownership rounds at 1281x721, and pixel-exact EGL window resizing/retirement.

The current full system image boots to the desktop in QEMU. QEMU has no
V3D device, so native tests supply graphics evidence. An earlier attempt
with the old, unpadded minimum image produced no boot log and is not a pass.
The full-image QEMU copy is padded to the required 4 GiB card size.

Evidence: `cache-growth-{before,before2,after,installed}.json`,
`cache-host-{before,after}.log`, `candidate-pixels.json`,
`window-abba-progress.json`, `mesa-install.json`, `installed-loaded.json`,
`qemu-full.log` and `qemu-full.png`. Native allocation-failure injection
was not performed; that path is covered by the host fixture.

## Next bottleneck: window presentation

A whole-system 2 ms sampling profile of 2,000 completed 1280x720 EGL draws
on the installed library attributes 4.778 seconds (54.43% of samples in the
EGL worker) to `memcpy`, and 2.180 seconds (24.83%) to `fd_ioctl`. Its
app_server window thread spends 11.082 sampled seconds (77.11%) in `memcpy`
and 2.718 seconds (18.91%) in `Painter::FillRectNoClipping`. These are
per-thread sample fractions, not percentages of total system capacity.
The profiled run still verifies the final pixels. The caller-stack run
locates about half the app_server window samples in back-to-front copying,
a quarter in bitmap drawing, and a fifth in background clearing. These are
separate presentation stages, not duplicate invocations of one copy.
Evidence: `window-profile.txt`, `window-profile-result.json`, and
`window-profile-stacks.txt`.

### Wider solid fills: not adopted

A native comparison checks the existing four/eight-byte loops against
unrolled GPR stores, NEON stores and aligned NEON stores on heap RAM and
cached/uncached V3D BO mappings. All four-byte alignments, lengths from zero
through 1,024 bytes, longer row boundaries, three colors and surrounding
guards pass. Cached full-frame fills remain near 2.9 GB/s for all
implementations. Wider unaligned stores regress some uncached cases.
Small cached spans improve, but these results do not justify changing the
shared app_server fill code. No such change was installed.
Evidence: `fill/fill-bench.cpp`, `fill/native.txt`.

### Shared GPU buffers for EGL windows

`mesa-haiku-shared-present.patch` creates two cached linear staging textures
for eligible V3D windows and imports each texture's memory as a BBitmap.
A GPU blit writes the retired buffer, a read map waits for completion and
invalidates CPU caches, and the view receives the complete bitmap. Its
synchronous DrawBitmap read must finish before BitmapHook returns that
buffer for reuse. The resource and area mappings remain alive for the
bitmap's lifetime. Resize/destruction retire the bitmap before its area and
resource. Small windows, software rendering and failed shared-bitmap
creation retain the CPU-copy path. A V3D capability check excludes other
drivers. `HAIKU_V3D_SHARED_PRESENT=0` disables the new path.

This removes one full-frame CPU copy. app_server still copies the bitmap
into its back buffer and then to the front buffer. It applies to EGL window
surfaces and BGLView, not Summit's separate pbuffer/readback presentation.

The first build was opt-in and qualified through a private vendor file.
Its SHA-256 is
`dcdef4352ba99a3d92240e742e90b9cf017ee79a844b6628b2f34218cc8d4d77`.
The default-enabled build is now installed as
`/boot/system/non-packaged/lib/libEGL_mesa.so.0`, SHA-256
`687f349b194b788d17bb4fc12c22a335f6116e2e992decdf2e068655f7decfd3`.
The previous cache-fixed library is verified in
`/boot/home/performance-20261008/rollback/libEGL-cache-fixed.so.0`.
Installation used a synced rename under the hardware lease; no reboot,
kernel change or user-setting change was needed. The default-enabled build
also passes the QEMU boot/GLES/window checks and private native pixel checks
before installation.

An off/on/on/off comparison under the exclusive hardware lease measures
480 completed draws per run. Figures below average each pair; rates are
completed BView draws per second, not physical monitor refresh.

| Window | Copy draws/s | Shared draws/s | Copy client CPU ms/frame | Shared client CPU ms/frame |
| --- | ---: | ---: | ---: | ---: |
| 640x480 | 236.03 | 288.05 | 2.174 | 1.246 |
| 1280x720 | 95.58 | 119.53 | 4.369 | 2.002 |
| 1281x721 | 90.87 | 104.67 | 4.530 | 2.176 |
| 1920x908 | 55.05 | 68.56 | 6.985 | 3.068 |

At 1280x720, client plus whole-app_server CPU falls from 12.098 to
9.847 ms/frame, about 19%. app_server's own work is essentially unchanged;
the saved client copy explains the gain. The measurements also count V3D
storage once at its kernel owner and normal bitmap storage at its client
mapping, excluding duplicate GPU aliases. Their combined warm storage
falls from 23,490,560 to 19,808,256 bytes at 1280x720, a 3,682,304-byte
(3.51 MiB) saving. At 1920x908 the saving is 6,975,488 bytes (6.65 MiB).
The extra retained GPU staging texture replaces two normal bitmap buffers.

The candidate image boots to the QEMU desktop and passes softpipe clear,
triangle and a 512x320 EGL window pixel test. The first QEMU window fixture
extended below its 640x480 screen and failed screen pixels; the corrected
in-bounds run passes, with the original result retained. QEMU does not
exercise V3D or shared GPU storage.

Native off/on checks pass nine sizes across two contexts, including odd
widths and padded rows. A coordinated 203.5-second run checks all bitmap
and screen pixels for 9,216 frames, 288 size cases and 32 context lifetimes.
Every lifetime returns to zero kernel V3D buffer bytes and zero shared
bitmap areas. The normal OpenGL Kit fixture passes four cycles, eight
BGLView contexts, 32 frames, complete GL/screen pixels, recursive locking,
resizing, guards and shared-area cleanup.

Two concurrent 640x480 clients also render correctly. One is killed after
three seconds; the survivor completes 4,000 frames with exact final pixels.
A fresh context then passes. Kernel GPU bytes and shared mappings return
to zero after the survivor and fresh context close. This is a process-loss
check, not a GPU hang/reset test. Native allocation-failure injection remains
untested; unsupported and disabled shared paths are exercised directly.

Early long-test screen failures were caused by a separate Summit performance
controller opening a browser over the fixture. The actual GPU bitmap passed.
The retained `shared-failure-screen.ppm` shows the browser window, and MCP
job history records its overlapping launch. Both local controllers now take
`/mnt/HaikuWork/rpi4/state/hardware.lock` for each complete native run. The
original twelve-hour lease was replaced by short leases so both tasks can
proceed. Overlapping trials are retained but excluded from the final
measurements above. No Summit product source changed for this coordination.

Evidence: `shared-present/`, including `qemu-results.txt`,
`shared-{off,on}-pixels.txt`, `shared-isolated-stress.txt`,
`isolated-stress-result.json`, `shared-glview.txt`,
`isolated-bench-summary.json`, `concurrent-result.json`, and `run-list.json`.

After installation, without a vendor or feature override, nine-size EGL
checks, the OpenGL Kit fixture, clear/triangle, 48 readback combinations and
80 texture-ownership rounds at 1281x721 pass. A 1,500-frame 1280x720 run
checks the shared path explicitly and finishes at 119.03 completed draws/s,
with zero GPU buffer bytes and shared mappings after teardown. All twenty
device health checks pass. Evidence: `default-qemu-results.txt`,
`default-private-result.json`, `shared-installed-checks.txt`,
`installed-result.json`, and `installed-health.json`.

### BGLView redundant background clearing

The OpenGL Kit's `BGLView::Draw()` paints its bitmap and fills any uncovered
updated area with LowColor, including before the first swap and after a
resize. Its opaque default ViewColor nevertheless made app_server clear
the updated area first. `libglvnd-haiku-view-background.patch` sets the
default view background to `B_TRANSPARENT_COLOR` in both constructors.
LowColor and the existing Draw implementation stay intact. Applications
can still set an explicit ViewColor.

This is a Pi-specific libglvnd build, separate from the shared ROCK image
extras. Reconstructing the old library from the pinned 1.7.0 snapshot plus
the existing redraw-coalescing patch gives the exact installed baseline
SHA-256, `282ac83b276c8019e933fd924f59f2f952f77012fe8fa3b8ebe3042d992f3fbd`.
Both incremental and clean builds of the new patch give
`b1aac205ad0076f4c2f30a23128b787712e8b2251dd1383245bc59633d7ee419`.
The coalescing and bitmap-lock fixes are preserved. `build-glvnd.sh` stages
the Pi library; the normal Mesa build invokes it and UserBuildConfig uses
that stage for `libGL.so.1`.

The initial causal comparison uses the installed baseline library with
explicit opaque/transparent/transparent/opaque ViewColor settings, under
the exclusive hardware lease. Each run renders 480 frames, waits for every
draw, and checks the final complete screen image. Initial empty and
shrink/grow background coverage also pass. Pair means:

| View | Opaque draws/s | Transparent draws/s | Opaque app_server ms/frame | Transparent app_server ms/frame |
| --- | ---: | ---: | ---: | ---: |
| 640x480 | 276.98 | 346.06 | 2.872 | 2.162 |
| 1280x720 | 116.34 | 145.54 | 7.887 | 6.139 |
| 1281x721 | 102.38 | 124.33 | 8.030 | 6.238 |
| 1856x900 | 70.25 | 87.49 | 13.241 | 10.369 |

At 1280x720, app_server CPU per frame falls 22.2%; client CPU remains
approximately 2.22 ms/frame. Combined client plus whole-app_server CPU
falls from 10.098 to 8.362 ms/frame, about 17.2%, and completed draw rate
rises 25.1%. Rates count completed BGLView draws, not monitor refresh.
These measurements already include the shared GPU presentation improvement;
they are a separate BGLView fixture and should not be directly combined
with the preceding EGL fixture's baseline numbers. Summit's pbuffer path
and views that already select transparent backgrounds receive no such gain.

The candidate passes the same screen coverage checks with its default
color, a missing-renderer fallback (background pixels plus visible error
text), software rendering, an explicit opaque override, and shared GPU
presentation disabled. The ordinary OpenGL Kit fixture passes 24 repetitions:
192 context lifetimes, 768 fully checked GL/screen frames, recursive locking,
resizing and guard bytes. Shared bitmap areas return to zero after every
cycle. The candidate also boots in QEMU and passes the softpipe and
missing-renderer checks there. QEMU supplies no V3D evidence.

The new library is installed at `/boot/system/non-packaged/lib/libGL.so.1`
with the checksum above; the verified original is retained at
`/boot/home/performance-20261008/rollback/libGL.so.1`. Installation uses
a synced rename under the hardware lease. Without a library override,
the fixture reports that system library path and the transparent default,
passes pixel/fallback/OpenGL Kit checks, and completes 2,000 1280x720 frames
at 145.61 draws/s. app_server uses 6.131 ms/frame and the client 2.209.
All twenty health checks pass. GLTeapot renders normally on the installed
libraries, its native screenshot is inspected, and it closes cleanly. No
kernel change or reboot was needed.

Evidence: `glview-clear/`, including `abba.txt`, `abba-summary.json`,
`qemu-results.txt`, `candidate-qemu-results.txt`, `candidate.txt`, `stress.txt`,
`fresh-build.log`, `installed.txt`, `installed-result.json` and
`installed-health.json`.

A subsequent whole-system 2 ms profile of 2,500 completed BGLView frames
attributes 13.728 of 14.472 sampled seconds (94.86%) in its app_server window
thread to `memcpy`. Background filling is no longer a significant sampled
cost. The rendering worker has only two memcpy samples. This identifies the remaining
copy stages as the next presentation limit; it does not imply that 95% of
total system CPU is used by copies. Evidence: `glview-clear/profile.txt`,
`profile-run.txt`, `teapot.png`, `teapot-images.json` and `teapot-result.json`.

## V3D cache-maintenance loop bounds

A kernel-inclusive profile locates the next client CPU cost in
`sync_buffer_cache()`. The normal profiler reads only the loaded kernel
module's dynamic symbols, hiding this static function. A private copy of
the profiler accepts an explicit symbol-file override. The installed,
unstripped V3D file exactly matches the local build (32,035 bytes, SHA-256
`ea99935389c9be18f943b58db5943fc6aa5ae5057b058a2d657371b7b069cfb5`),
so its full symbol table supplies the missing names without changing the
running driver. The diagnostic override passes a QEMU smoke check first.

Over 3,000 completed 1280x720 BGLView frames, the original cache walk
accounts for 3.174 of 6.200 sampled seconds in the rendering worker, 51.19%.
Native disassembly shows that the inline assembly's memory clobber causes
the compiler to reload `buffer->address` and `buffer->size`, and recompute
the end address, after every cache-line operation. Those fields remain
fixed while the caller holds the device mutex and an allocation reference.

The driver now saves the start, end and line size in local constants before
the walk. CPU pinning, the CTR-derived line size, `DC IVAC` for CPU reads,
`DC CIVAC` for GPU ownership and the final full barrier are unchanged.
The generated inner loop contains no BO metadata loads. This changes the
CPU maintenance cost, not GPU clocks, memory types or ownership rules.

`jam rpi4_cache_loop_probe` builds a reproducible native loop comparison.
It pins its thread to CPU 0 and operates only on its own aligned memory,
checking every byte afterwards. It compares the original loop, fixed bounds,
and eight-line unrolling in forward/reverse order, with cold, read-warmed
and dirty buffers from 4 KiB through 8 MiB. The user-space fixture exercises
`DC CIVAC`; the actual driver tests below cover `DC IVAC` and GPU ownership.
At 3,686,400 bytes, pair means of the per-run medians are:

| Buffer condition | Original us | Fixed bounds us | Unrolled us |
| --- | ---: | ---: | ---: |
| Cold | 993 | 128 | 128 |
| Read-warmed | 1001 | 162.5 | 163 |
| Dirty | 994 | 129 | 128 |

Unrolling adds no useful gain and was not adopted. `CACHE_BENCH_QUICK=1`
limits the fixture to its 4 KiB case for smoke checks. The driver and probe
build separately; no unrelated DSI source changes enter the build.

The candidate is installed at
`/boot/system/non-packaged/add-ons/kernel/drivers/graphics/v3d`, SHA-256
`c0253ad24a7c40fb7362a94f1c4f208faa3051a11ae5f186215d5bfd80e24589`.
The packaged original remains untouched and its verified extra backup is
`/boot/home/performance-20261008/rollback/v3d-cache-loop`. A serial-captured,
orderly reboot loads the candidate from its new path, confirmed by the
kernel image list. The core remains `hrev60206+750`; Ethernet stays `.213`.
The QEMU image also boots with the candidate staged and passes softpipe
BGLView pixels, but does not execute V3D hardware operations.

The first native screen check, started 21 seconds into the boot, passes
GLES rendering, 48 readback combinations and the actual GPU bitmaps, then
fails a screen pixel at x=1482 in the widest window. The notification
server reports a window beginning at x=1483, overlapping that screen area
including its border. This supports startup notification occlusion; the
failed run is retained and excluded. After startup settles, the full
nine-size, two-context bitmap/screen test passes with zero GPU buffer bytes
and shared bitmap areas after each lifetime.

Two 960-frame runs per size on the original driver precede the reboot;
two matching runs use the candidate. Means are below. These are sequential
before/after measurements, not an ABBA comparison across driver boots.

| View | Original draws/s | Candidate draws/s | Original client ms/frame | Candidate client ms/frame |
| --- | ---: | ---: | ---: | ---: |
| 640x480 | 358.97 | 381.68 | 1.390 | 1.154 |
| 1280x720 | 153.30 | 162.61 | 2.190 | 1.506 |
| 1281x721 | 130.69 | 139.19 | 2.341 | 1.520 |
| 1856x900 | 92.41 | 97.48 | 3.147 | 1.972 |

At 1280x720, client CPU falls 31.2% and completed draw rate rises 6.1%.
Whole-app_server CPU differs across these runs, increasing from 5.719 to
6.173 ms/frame; the client saving must not be presented as a whole-system
CPU percentage. Earlier profiled runs have a different baseline draw rate.
On the candidate's matched 3,000-frame profile, the cache function falls
to 0.884 sampled seconds, about 72% less absolute sampled time, while
the worker total falls to 3.750 seconds. The profiler reports no dropped
or unknown worker ticks, but sampling remains an estimate.

Native qualification passes 80 texture rounds at 1281x721 (partial writes,
repeated maps, cropped reads and PBOs), full-HD depth/triangle pixels,
48 format/MSAA/mip/crop/padding/guard combinations, the eight-context
OpenGL Kit fixture, cached TFU ownership checks, and 1,100 buffer-lifetime
rounds. Client buffers return to zero while the original 4 MiB shared GPU
page table remains. Two concurrent texture clients also pass 40 rounds
each at 1281x721 and 1920x908, followed by another 100 clean lifetime rounds.
All twenty device health checks pass.

Evidence: `cache-profile/` for the baseline symbols and profile;
`cache-loop/` for `user-bench.txt`, `user-bench-summary.json`, `v3d-bounded.dis`,
`driver-qemu-results.txt`, `candidate-serial-capture.log`, `candidate-images.json`,
`first-checks.txt`, `notification-frame.json`, `recheck.txt`, `control.txt`,
`bounded.txt`, `summary.json`, `bounded-profile.txt`, `qualification.txt`,
`concurrent-result.json`
and `qualified-health.json`.

## Kernel memory-fill investigation

A private native allocation benchmark repeatedly creates a 64 MiB area,
checks that every word is zero, dirties it and deletes it. All 32 resident
and 32 demand-paged rounds pass. A 1 ms kernel-inclusive profile attributes
1.954 of the worker's 11.909 sampled seconds to kernel `memset` (16.41%).
Median resident allocation is 59.597 ms; demand allocation plus the first
full zero read is about 142.320 ms. These synthetic timings include page
management costs and do not represent application launch times.

The installed kernel's scalar fill uses one eight-byte store per loop.
Its instruction words exactly match the local kernel object, although the
complete kernel files have different hashes. The private comparison links
that original object under a renamed symbol and adds a 64-byte C loop that
compiles to general-register paired stores. It also tries handling exactly
eight bytes in the aligned-word path. The candidate passes the QEMU smoke
benchmark, 497,300 native checks (ten values, 64 alignments and guarded page
boundaries), and 8,192 checks on write-combining/write-back GPU allocations.

An original/candidate/candidate/original native comparison nearly doubles
cached 4 KiB throughput, but eight-byte unaligned and 32-byte fills regress
by roughly 14–35%. Eight MiB fills change little. This candidate is rejected
as a general kernel replacement; neither the kernel source nor the running
kernel is changed. Page clearing remains a possible narrower target, but
the large-buffer results do not yet establish a useful allocation saving.
Evidence: `kernel-memory/{baseline-profile.txt,baseline-summary.json,
kernel-identity.json,installed-memset.dis,guards.txt,micro-summary.json}`.
The first 40-second QEMU window ended before buffered benchmark output was
saved; the 55-second rerun completed. A native launch before upload exited
127 and is excluded; the qualified native run exits zero.

## AirPins toolbar startup work

Warm application measurements put StyledEdit near 99 ms, AboutSystem near
88 ms and AirPins near 301 ms to its first visible window. A ten-launch,
kernel-inclusive profile shows remaining loader lookup work and app_server
round trips. AirPins constructs every toolbar icon twice: in the button
constructor, then again in `AttachedToWindow()`. Each call rasterizes a
vector icon and creates the button's normal, active and disabled bitmaps.

AirPins commit `d732522` remembers the rendered icon, size and colour and
keeps the constructor's bitmaps when attachment has not changed those inputs.
The constructor still supplies an icon for pre-attachment layout. A changed
icon, font size or colour causes a rebuild; failed icon creation is not cached.
The temporary rasterization/tint bitmap uses `B_BITMAP_NO_SERVER_LINK` because
only its CPU pixels are needed. The actual button state bitmaps retain their
server connections. This changes startup work, not GPIO ownership or polling.

The release baseline is rebuilt byte for byte against the installed app:
364,146 bytes, SHA-256
`ff372a5241e58b323ac1b2b451d1aa7fa47e71a232efb563af5281e08d6a0f61`.
The release's shared-unwinder link specification matters: the first isolated
builds used the standalone helper's static unwinder. Their initial comparison
is retained separately; the figures below use matching release link settings.
The candidate is 364,218 bytes, SHA-256
`bdb38b2fea16f15859a24edbc59b35862045bdde61ca181a666addbb7ae158e7`.

An original/candidate/candidate/original comparison runs eight launches per
group, omitting each group's first launch from the warm statistic. Fourteen
launches per variant give median first-window times of **301.25 vs 262.91 ms**,
12.7% less. Separate ten-launch profiles accumulate 2.340 vs 2.077 sampled
seconds in the app's threads and 1.434 vs 1.274 seconds in its app_server
threads, about 11% less CPU across those startup/quit cycles. This is sampled
CPU, not an idle-runtime or whole-system percentage.

Both builds boot and launch in QEMU. A private fixture compares 200 complete
bitmap hashes across all ten icons, four button states, 12/18/24-point font
sizes, reattachment and icon switching; QEMU and the Pi match exactly.
The candidate also retains its state bitmap objects on unchanged attachment.
Native screenshots of all three layouts are pixel-identical inside the
application window. UI tests use simulated pins and restore saved settings.

`airpins-1.0.0-2-arm64.hpkg` is installed; its application exactly matches the
tested candidate. Eight installed launches give a 264.67 ms warm median.
Launching by application signature resolves `/boot/system/apps/AirPins`.
All twenty health checks pass. The old package is verified in
`/boot/home/performance-20261008/rollback/airpins-1.0.0-1-arm64.hpkg`.
The local source package and both image staging directories select revision 2;
the image copies use the existing Zstandard policy. The live package retains
the release builder's original compression: packagefs rejects replacement of
the same version solely to change compression, leaving the verified installed
package intact. Local installation uses `pkgman install -R` because repository
refresh is unavailable; the reviewed plan changes only AirPins.

Evidence is under `app-startup/`, chiefly `airpins-profile.txt` and
`icon-reuse/{release-summary.json,cpu-summary.json,native-icon-baseline.txt,
native-icon-candidate.txt,screen-comparison.json,installed-final.json,
signature-launch.json,installed-health.json,image-package-manifest.json}`.

## Reuse the vector-icon gamma calculation

Each `IconRenderer` constructs an identical gamma-2.2 lookup table, evaluating
512 `pow` calls even for a 16-pixel toolbar icon. A function-local constant now
calculates that table once, with C++ thread-safe initialization. AGG's gamma
table gains a deep-copy constructor, allowing each renderer to retain its
original ownership, class layout and exported accessor. The table's two
allocations remain per renderer; the immutable source retains another 512
bytes per process after first use. This is a CPU optimization, not a heap
reduction. No public class layout or gamma rounding formula changes.

`rpi4_icon_bench` renders supplied HVIF files into local RGBA bitmaps, reports
loaded `libbe`, per-size CPU/wall time and complete visible-pixel hashes, and
can start up to sixteen independent rendering workers together. The native
comparison uses fourteen icons, including StyledEdit, Terminal and
Icon-O-Matic gradient artwork, with 100 rounds per size. Two runs per variant
in original/candidate/candidate/original order give these mean worker CPU
times, in milliseconds per 1,400 renders:

| Icon size | Original | Candidate | Less CPU |
| --- | ---: | ---: | ---: |
| 16x16 | 775.620 | 617.455 | 20.4% |
| 24x24 | 806.639 | 776.151 | 3.8% |
| 32x32 | 883.288 | 776.970 | 12.0% |
| 64x64 | 1132.493 | 1061.214 | 6.3% |
| 128x128 | 1819.444 | 1693.584 | 6.9% |
| 256x256 | 3821.026 | 3779.253 | 1.1% |

The complete mixed-size workload uses 9.239 vs 8.705 CPU seconds, 5.8% less.
A separate 1 ms profile records 265 ms in `pow` for the original renderer
worker and no samples there for the candidate; total sampled worker CPU is
9.341 vs 8.930 seconds. These are synthetic vector-rendering results, not
application-wide gains. Fourteen warm AirPins launches per variant give
264.53 vs 263.57 ms to a visible window, effectively unchanged at this scale.

Eight workers starting together pass in QEMU and on the Pi. All 672 hashes
(fourteen icons, six sizes, eight workers) exactly match the original library;
the installed default repeats the same hashes. Host ASan/UBSan checks cover
deep-copy independence, three table element/resolution combinations and five
gamma values. Both libbe and Icon-O-Matic build. Native editor screenshots
match pixel-for-pixel across the visible editor content, including its gradient
canvas and previews; the test restores the initial settings state. A QEMU
boot with the candidate as its default system library reaches the desktop
and passes the concurrent rendering fixture.

The installed override is `/boot/system/non-packaged/lib/libbe.so`,
4,606,534 bytes, SHA-256
`2febc7adce8b4b72a548963bf6e93e081fc92bd3a4f0e37ac141b3b86925dbd9`.
The packaged 4,606,238-byte library is untouched, SHA-256
`65e63aaad60e3013c49ea9ec9c1c84e524799fe97166554d7d90f6b5d6629d16`;
it is also backed up as
`/boot/home/performance-20261008/rollback/libbe-icon-gamma-original.so`.
No native reboot was needed: new applications use the override, while
existing teams retain their previous mapping. Removing this override under
the hardware lease restores the packaged choice for subsequent applications.
All twenty installed health checks pass. The native Icon-O-Matic package is
unchanged; its rebuilt internal renderer was tested privately and will enter
the next image build with the source change.

Evidence: `icon-gamma/{native-abba-summary.json,baseline-profile.txt,
candidate-profile.txt,baseline-native-check.txt,candidate-native-check.txt,
system-qemu.txt,ui-pixel-comparison.json,app-abba-summary.json,
installed-check.txt,installed-check.json,installed-health.json}`. The first
application timing invocation used a nonexistent helper path and exited 127;
the qualified comparison uses the existing private helper and absolute library
paths. The startup investigation also verifies that the installed GCC 13.3
libstdc++ has only a SysV hash table; no C++ runtime rebuild or loader change
was attempted in this step.

## Copy-mode solid scanlines

A thirty-second native desktop profile identifies
`blend_hline_copy_solid` as the largest individual cost in AirTop's offscreen
app_server drawing thread: 320 of 977 sampled milliseconds. This is a different
path from the shared `gfxset32` helper investigated earlier; that helper remains
unchanged. The candidate unrolls opaque scanlines in eight-pixel blocks, with
the original scalar tail, and combines red/blue arithmetic into separate
16-bit lanes for partial coverage. Green is calculated separately. The weighted
lane sums cannot exceed 255 × 256, preserving the original division by 256,
rounding and opaque destination alpha. Explicit byte-order conversions retain
BGRA memory order. There is no new allocation or graphics ownership change.

`rpi4_drawing_bench` renders rounded panels, filled/stroked curves, fractional
lines, ellipses, text, disjoint clipping and BPicture playback into six bitmap
sizes. Each bitmap has a distinct window title, allowing the benchmark to
measure its own app_server drawing thread. Four timed runs per variant are
split across two boots each, alternating original/candidate/original/candidate.
Each large scene has eight warmups and 800 synchronized timed frames. Medians:

| Canvas | Scene | Original server ms/frame | Candidate | Reduction |
| --- | --- | ---: | ---: | ---: |
| 640×480 | Shapes and text | 5.047 | 4.466 | 11.5% |
| 640×480 | Shapes | 4.679 | 4.079 | 12.8% |
| 640×480 | Clipping | 6.670 | 6.137 | 8.0% |
| 640×480 | Picture playback | 5.075 | 4.480 | 11.7% |
| 1281×721 | Shapes and text | 9.194 | 8.808 | 4.2% |
| 1281×721 | Shapes | 8.693 | 8.299 | 4.5% |
| 1281×721 | Clipping | 11.954 | 11.354 | 5.0% |
| 1281×721 | Picture playback | 11.998 | 11.014 | 8.2% |

These are offscreen drawing CPU measurements, not monitor refresh rates or
whole-system CPU reductions. All 192 full-bitmap hashes from the eight native
runs agree, including the small sizes and alpha bytes. Baseline and candidate
QEMU boots produce the same 24 hashes. Two thirty-second desktop profiles per
variant record 977/990 ms for AirTop's original offscreen thread and 885/936 ms
for the candidate; the scanline function accounts for 320/323 versus 225/257 ms.
The live graph contents vary, so these profiles corroborate the hotspot reduction
rather than replacing the deterministic fixture.

The isolated native comparison passes 243,200 pixel/canary/protected-row checks
across five implementations, all 256 coverages, five offsets and nineteen span
lengths. A further 16,777,216 source/destination/coverage triples with three RGB
permutations exactly match the original arithmetic. Separate GPU allocations
pass 544 comparisons each with write-back and write-combining mappings.
Large write-combining spans are approximately unchanged. Tiny opaque spans
can be slower: a three-pixel call costs roughly 23–24 ns versus 13–16 ns in
that mapping. The accepted change has a measured workload gain, not a claim
that every span size or mapping is faster.

The candidate builds as app_server and boots in QEMU before native installation.
Its final native path is
`/boot/home/performance-20261008/scanline/app_server`, 2,055,040 bytes, SHA-256
`83f3495035ecfe79e26a1e76789c58f5a1499b5cdb4370b9cb3b0257b81cf9df`.
A user launch-service amendment at
`/boot/home/config/settings/launch/app-server-performance` selects it at boot.
The packaged `/boot/system/servers/app_server` remains byte-for-byte intact,
2,055,216 bytes, SHA-256
`54137fe854cd0c04f4d0a6aa1a9426424ace2392f9133034005bc4aba08488bc`.
Removing only that amendment under the shared hardware lease, followed by an
orderly reboot, restores the packaged server; the rollback boot was tested.
All three native reboots had serial capture. The final candidate boot passes
all twenty health checks, and the core, Mesa, GL, V3D and DSI binaries are pinned.

The first temporary candidate filename was `app_server-scanline`; an exact-name
health check consequently reported the server absent despite its loaded image,
registered application and successful drawing checks. The final path preserves
the normal basename. Existing EGL/BGLView diagnostics also assumed a
`/servers/app_server` path. They now resolve the registered application signature
through BRoster. Their new lookup and software-rendered pixels pass in QEMU.
The original failures remain in the evidence instead of being counted as passes.
The corrected native fixture passes 1,152 EGL frames across 72 size runs,
eight context lifetimes, complete bitmap/screen pixels and zero remaining GPU
buffers/shared areas. BGLView passes empty drawing, 800 completed 720p draws,
resize gaps and all final pixels, with zero retired-area leaks. Its 163.52 draws/s
is consistent with the preceding graphics stack; no additional GL throughput
gain is claimed. Pbuffer rendering and all 48 texture readback checks also pass.

Evidence: `scanline-fill/{drawing-summary.json,drawing-native-*.txt,
native-v2-summary.json,native-v2.txt,graphics-native-summary.json,
graphics-native.txt,*idle-profile.txt,*boot*-drawing.json,*boot*-health.json,
window-roster-qemu.txt,glview-roster-qemu.txt,drawing-window-qualified.txt,
installed-health.json,installed-hashes.json,baseline/app_server}`.

## Whole-page clearing: measured and rolled back

The general kernel `memset` investigation above was followed by a narrower
candidate in ARM64 `PMAPPhysicalPageMapper::MemsetPhysical`. It used `DC ZVA`
only for one aligned, zero-filled page in the loaders' Normal Write-Back
physical-RAM mapping. Other lengths, nonzero fills and unsupported `DCZID_EL0`
values retained the original `memset`. Interrupts were preserved and disabled
across the feature check and zero loop, keeping the CPU fixed without requiring
a current thread during early boot. The implementation used no SIMD registers.
This candidate is **not retained or installed**: the native workload did not
show a useful gain.

The initial bare `DC ZVA` loop was itself unsuitable on this Cortex-A72. A
single hot page took about 209 ns versus 589 ns for the original kernel loop,
but a 64 MiB working set took 2,902–3,318 ns/page versus about 1,470 ns/page.
Ordinary GPR zero stores in the first and last cache lines, followed by `DC ZVA`
in the middle, removed that streaming regression. The resulting candidate
took about 214 ns for the hot page, 380–383 ns/page for a 1 MiB working set
versus 624–626 ns originally, and about 1,469 ns/page for 64 MiB. Sequential
and shuffled visits, forward/reverse variant order, 5,120 native guard checks
and forced feature-fallback cases passed. Microbenchmark gains alone were
insufficient to justify adoption.

Native allocation measurements exposed a confounder: Haiku's pre-cleared
free-page pool can make initial resident allocations much faster. One early
64 MiB run had a 17.6 ms median, while subsequent steady-state runs took about
58 ms. The qualified comparison first ran 64 rounds each of resident and
demand-paged 64 MiB allocations, checking every word for zero before dirtying
and freeing the area. Two measured 32-round runs followed that warmup on each
boot. The following values are the median of those two run medians, in CPU ms:

| Kernel boot | Resident allocation | Demand first read | Complete resident cycle | Complete demand cycle |
| --- | ---: | ---: | ---: | ---: |
| Original A | 58.464 | 140.288 | 158.157 | 221.396 |
| Candidate | 58.391 | 139.775 | 157.483 | 218.310 |
| Original B, restored | 56.673 | 139.717 | 155.521 | 218.731 |

The complete cycle includes allocation, zero verification, dirtying and release.
The candidate's allocation and first-read results fall within the original
boots' range. Smaller 4 KiB, 16 KiB and 1 MiB runs also show no consistent
whole-cycle advantage. The profiled 64 MiB worker takes 12.332 versus 12.312
sampled seconds; the original kernel `memset` and candidate page-clear path
account for 2.426 and 2.382 seconds respectively. This is an unchanged
streaming-memory workload, despite the hot-cache microbenchmark improvement.

Before native testing, the candidate kernel booted in QEMU and passed zero
allocation checks, all 24 drawing hashes, `resize_area_tests`, `cow_bug113_test`,
`mmap_cut_tests` and `mmap_invalid_tests`. Those bounded VM tests and all native
zero checks also passed. The new reusable `rpi4_vm_bench` records per-phase
wall and thread CPU time; its comment gives the required warmup sequence.
Its own QEMU and native smoke tests pass. Very small allocations include enough
timer/bookkeeping overhead that their microsecond differences are coarse.

The existing USB recovery image was booted through NanoKVM, read the SD FAT
read-only, and verified the original boot archive before returning to healthy
Haiku. Its saved SSH address was stale; serial identified `.213`, the expected
host key matched, and the corrected repeat passed. No recovery SD writes were
needed. Each native kernel switch held the shared hardware lease, verified
archive hashes, synced and unmounted FAT, then rebooted with serial capture.
Only `system/kernel_arm64` differed in the candidate archive; every other
member and both boot configuration files matched the original.

The original `airos-boot.tgz`, SHA-256
`0b5b3a793b7a2d7e3d73523d51875d4eb9727df7c628e564a37df47375d57a3d`,
is restored. Its kernel is
`572db7b66ae352a271941e743549a63ed290bab7b88109b3ddaddfcb526cca80`.
The candidate archive and kernel were `57250e42...` and `6038cc97...`;
serial identified its kernel as `hrev60206+759+dirty`. Packaged kernel files
and `uname` metadata remained at the original revision even during that test,
so the profiler explicitly used the matching candidate symbols. Serial and
archive identity, rather than `uname` alone, distinguished the boots.
Original PMAP source and local kernel code are restored; the default host
boot archive remains original. The rejected source, binaries and full hashes
are preserved as local evidence.

Evidence: `page-clear/{native-micro-v2-summary.json,kernel-comparison-summary.json,
warm-*-summary.json,warm-*-profile.txt,qemu-kernel.txt,qemu-vm-tests.txt,
recovery-read-only-check.txt,*boot*-serial.log,*boot*-after-archive.json,
candidate-manifest.json,final-native.txt,final-installed-hashes.json,
final-health.json}`. A final graphics invocation initially used an incorrect
private drawing-probe path; that exited 127 and is retained separately from
the corrected passing run.

## GNU symbol hashes in the C++ runtime

Startup profiles identified remaining SysV symbol-hash work in the GCC C++
runtime, after the earlier dual-hash change to Haiku's own libraries. The
installed GCC 13.3 library still had only a SysV table. A rebuild adds a GNU
table while retaining SysV compatibility. This is a loader/startup improvement;
it does not speed up C++ code after its symbols are resolved.

Two libraries were linked from exactly the same objects, differing only in
hash style. Alternating native runs used isolated `LIBRARY_PATH` directories,
verified the actual loaded image, and restored AirPins settings afterward.
Each result below contains 30 warm launches per variant, from two groups of
16 with the first launch in each group excluded. Values are median ms:

| Application | Registration, SysV → both | First window, SysV → both | Client CPU, SysV → both |
| --- | ---: | ---: | ---: |
| StyledEdit | 38.042 → 33.583 | 99.734 → 95.345 | 90.715 → 86.836 |
| AirPins | 42.494 → 38.464 | 261.158 → 256.625 | 171.473 → 167.521 |

The isolated CPU savings are 3.879 ms (4.3%) and 3.952 ms (2.3%). An earlier
22-launch-per-variant repetition also saves about 4 ms of first-window time.
The installed original and rebuilt SysV runtime have comparable startup time:
the AirPins parity comparison gives 261.420 and 260.916 ms. A separate original
versus candidate comparison also improves, but the same-object comparison
above isolates the hash-table effect more precisely.

`rpi4_app_bench` now optionally appends `client_cpu_ms` when `APP_BENCH_CPU=1`.
It sums the application's user and kernel CPU across its threads immediately
after the application looper replies. Work performed by app_server and other
teams is excluded. Without the option, the existing six-column CSV is unchanged;
both output formats pass native and QEMU checks.

The GNU hash table occupies 44,520 bytes and increases the read-only executable
mapping by 65,536 bytes after alignment. Writable segment size is unchanged.
The comparison library grows by 65,648 file bytes, including ELF metadata.
This is a mapped-size cost, not a measured per-application private-RAM increase.

`build-cxx-runtime.sh` pins buildtools revision
`8375c2dbeaf109c520798cb234d57f0895463201` and GCC 13.3.0. It preserves the
installed `c++config.h` byte-for-byte and checks all 6,003 exported/versioned
symbol names and types, all 174 imported names and types, the SONAME and the
sole direct `libroot.so` dependency. Linking uses the native GCC support
archive to retain the original shared-unwinder imports. Three optional clock
APIs exposed by newer headers remain disabled, matching the installed runtime.
Only verified builds are published into the image stage. The image command
dry run confirms the staged runtime is included in `system/non-packaged/lib`.

The canonical build's 26 loaded ELF sections match the measured candidate's
addresses, sizes and contents exactly; only nonloaded debug metadata differs.
Its installed path is `/boot/system/non-packaged/lib/libstdc++.so.6`, SHA-256
`f073678d2aa5f25c6e54fdb86e2eb89aad113c0c0a7262e20c25dfc0a2433f9c`.
The packaged original remains
`f3a7df4507cf976d45daa8af613958e9151a1cef5d80e492d7ea922d3796f871`, with a
verified backup at `rollback/libstdc++-original.so.6` beneath the session's
native evidence directory. Installation used a synced rename. Removing the
override restored the original provider and passed both ABI probes; the
candidate was then reinstated and the machine rebooted normally.

Both old and new libstdc++ ABIs pass bounded probes covering eight threads and
8,000 shared-ownership/string operations, condition variables, asynchronous
exceptions, containers, locale/streams, regex, path normalization, and 128
cross-library exception/ownership/RTTI rounds. The original, rebuilt SysV and
dual-hash libraries all pass in QEMU and natively. The canonical default also
passes both ABIs, all 24 drawing hashes, EGL and BGLView after reboot. The pinned
libraries, driver, servers and kernel retain their expected checksums; the original kernel
still boots, and all 20 health checks pass. Screenshots show the expected desktop.

Provider inspection found app_server, registrar and AirTop using the override.
Tracker and Deskbar deliberately search their executable-adjacent
`/boot/system/lib` first, so they keep the packaged C++ runtime and libbe;
their lookup policy is unchanged and no gain is claimed for them. An initial
post-boot assertion incorrectly expected the override in Tracker; the corrected
check records each actual provider. The first pre-reboot health check also
found the idle screen blanker; it was stopped before the passing checks.

The initial standalone build missed the POSIX threading header, and two early
link attempts introduced a self-dependency or a private unwinder; those local
candidates were rejected before native use. The first test fixture also linked
the cross toolchain's private unwinder and failed against the original runtime.
The reusable `build-cxx-runtime-tests.sh` explicitly uses the native shared
unwinder, and its corrected probes pass all three runtime variants.

Evidence: `cxx-hash/{startup-a-summary.json,startup-cpu-summary.json,
canonical-allocated-sections.json,qemu-canonical.txt,native-probe.txt,
canonical-native-private.json,canonical-native-rollback.json,
default-verified-providers.json,default-verified-regression.txt,
default-verified-installed-hashes.json,default-verified-health.json,
default-boot-serial.log}` and `/mnt/HaikuWork/rpi4/cxx-runtime/manifest.json`.

## Gradient endpoint hoisting: rejected after measurement

The icon profile still spends time generating gradient palettes. A small
candidate moved invariant endpoint loads/conversions and the interpolation-mode
read outside the pixel loop. Its first build changed GCC's choice of fused
multiply/add term. A channel with endpoints 1 and 22, denominator 6 and numerator
1 then rounded to 19 instead of the original 18. That build was rejected before
native use. A second candidate explicitly preserved the ARM64 fused operation.

The corrected candidate matches the baseline across 114,688 native guarded
gradient calls: both interpolation modes, 14 output lengths from zero to 1,024,
and 4,096 deterministic cases including empty, duplicate and unsorted stops.
All 672 icon hashes also match, with a smaller corresponding QEMU pass. Its
11,787 exported symbols and 479 imports match the original libbe; file size is
unchanged. These checks establish correctness for the tested cases, not a speedup.

In an alternating original/candidate/candidate/original comparison, the isolated
256-entry linear palette uses 8.4% less CPU, while the default smooth palette
saves only 0.7%. Whole icon rendering across 14 icons and six sizes takes
8,724.049 versus 8,775.551 mean CPU ms for 8,400 renders, about 0.6% slower.
The first candidate run is effectively unchanged and the second is slower;
there is no repeatable full-rendering benefit to adopt.

The candidate remained in private library directories. Installed libbe stayed
at `2febc7ad...`, all 20 health checks passed, and the source was restored.
The rejected source, builds, rounding counterexample and measurements remain
under `icon-gradient/`, including `fma-order.txt`, `candidate-v2.patch`,
`native-abba-summary.json`, `*-gradient-check.txt`, `*-icons-check.txt`,
`*-qemu.txt`, `manifest.json` and `private-health.json`.

## Rectangle-fill batching: rejected after native boot comparison

The rectangle-fill helper writes two pixels per loop. Private eight- and
sixteen-pixel batches pass 81,504 guarded native cases, including zero-length
fills and page boundaries, plus 1,350 checks each on heap, write-back GPU and
write-combining GPU mappings. An eight-pixel batch is about 26% faster for a
cached 640-pixel row. Larger streaming fills and uncached GPU writes are
essentially unchanged; tiny fills can cost roughly one extra ns.

That microbenchmark gain does not translate into a repeatable full drawing
benefit. The eight-pixel candidate was built into app_server, passed the QEMU
desktop and all 24 bitmap hashes, and was compared over original/candidate/
restored-original native boots. Each boot ran the same drawing benchmark twice,
with 800 frames for each larger scene. Most timings overlap. The apparently
best result, the 1281×721 recorded-picture scene, takes 11.579 / 11.050 / 11.008
median app_server CPU ms per frame across those boots. The restored original
therefore reproduces the apparent improvement without the code change.

The original accepted scanline server at `scanline/app_server`, SHA-256
`83f3495035ecfe79e26a1e76789c58f5a1499b5cdb4370b9cb3b0257b81cf9df`, is restored
through the existing launch amendment. The packaged server, original kernel
and eight accepted improvements remain intact. Native drawing and all 20
health checks pass after rollback. The source change was removed; the
candidate `f78b7025...` remains private evidence.

Evidence: `rect-fill/{micro-summary.json,drawing-per-boot.json,
drawing-*-*.txt,qemu-server.txt,candidate-a-*.json,baseline-b-*.json,
candidate.patch,manifest.json}`. The conclusion uses the full native boot
comparison, not the faster cached-row loop alone.

## AirTop fixed panel geometry

AirTop repainted the full auxiliary-panel background and three rounded panel
frames on every update. Its fixed geometry is now rendered once at the actual
display scale, copied into a plain BBitmap, and copied back at device-pixel
resolution before drawing each frame's text, values and graphs. It reuses the
panel's existing drawing window during setup, so the cache requires no extra
window or drawing thread. Sampling and five-frame-per-second presentation are
unchanged. Failed cache creation and diagnostic skip modes use direct drawing.

The source tree at `/mnt/HaikuWork/apps/AirTop` was preserved before editing.
The installed 1.2.0-1 binary matches its local package, and a fresh baseline
build matches all loaded ELF sections. Original/candidate comparisons therefore
isolate the geometry cache. The accepted source is recorded in
`tools/rpi4/airtop-panel-cache.patch` because this application directory is not
a Git repository; `airtop-panel-cache-source.json` pins both source versions,
toolchain and package. Applying the patch to the preserved baseline reproduces
all 23 loaded sections of the accepted application.

The deterministic native drawing comparison uses the actual renderer with only
its header clock and uptime held constant. Four alternating runs of the final
candidate and original, 400 frames per 640×480 scene, match all 144 bitmap
hashes. Populated scenes use 16–19% less app_server CPU. The application test
`make check-panel` independently compares the cached path with direct drawing
in 288 cases: six sizes, fractional scaling, letterboxing, changing values,
core counts, sensor presence, network labels and diagnostic modes. QEMU and
native results agree; cache-failure fallback also matches the original.

The real DSI comparison warms each variant for 125 seconds to fill its two-minute
history, then measures 45 seconds, in original/candidate/candidate/original
order. Every run presents exactly 225 frames, collects 180 samples and retains
489 history entries. Mean CPU time for those 45-second intervals is:

| Measured work | Original | Cached geometry | Reduction |
| --- | ---: | ---: | ---: |
| app_server threads serving the panel | 1,426.713 ms | 1,014.952 ms | 28.9% |
| AirTop client, including sampling | 818.408 ms | 739.912 ms | 9.6% |
| Combined | 2,245.120 ms | 1,754.864 ms | 21.8% |

These are panel-workload CPU reductions, not percentages of total machine CPU.
The cache adds 1,228,800 bytes of pixel storage (1.17 MiB), matching the measured
increase in client mapped bytes. The same pages are shared with app_server.
Stopping the panel deletes its cache object, but the shared bitmap allocator
can retain the enlarged pool for reuse. Five start/stop cycles per variant show
constant mapped sizes, one drawing window while active and none after stopping.
There is no claim of immediate physical-memory return when the panel is hidden.

`airtop-1.2.0-2-arm64.hpkg` is installed. Its binary is 351,408 bytes, SHA-256
`d19a6f016c4ace1be0d67224af13e1284fb0c8bb6478fcd735ed30336b6a8e24`.
Local package plans change only AirTop. Downgrading to the backed-up original
package restores its exact binary hash, and reinstallation restores the tested
candidate. All 20 health checks and the earlier eight system hashes pass.
The full installed window renders and samples; its original settings and
panel-only service are restored afterward. The canonical source builds the
same application bytes. Both existing AirTop image staging locations now select
revision 2, with their original packages preserved and the DSI image copy using
the existing Zstandard policy.

The final normal native boot loads the package in panel-only mode with exactly
one app_server drawing thread. Its libbe and C++ providers are the accepted
non-packaged libraries. Nine pinned binary hashes, all 24 drawing hashes,
EGL/BGLView probes and 20 health checks pass. Serial confirms the original
`hrev60206+750` kernel; no boot configuration or firmware was changed.

Evidence: `airtop-panel/{source-manifest.json,baseline-allocated-sections.json,
reproduction-sections.json,panel-cache-test-qemu.txt,live-v1-summary.json,
v2/drawing-summary.json,v2/live-summary.json,v2/*-lifecycle.txt,install/,boot/,
image-package-manifest.json}`. An earlier prototype retained an unnecessary
offscreen window; the accepted version retains only the bitmap pixels. A UI
query encountered a transient TCP connection timeout; repetition completed,
with the panel and health checks restored after each attempt.

## Duplicate right-aligned font setup: not adopted

AirTop's right-aligned label helper installs its font, measures the string,
then calls a text helper that installs the same font again. A private candidate
keeps the first setup and draws the label directly. QEMU's 36 hashes and all
324 native hashes, including the direct-rendering fallback, match exactly.
Eight alternating native runs use 600 frames per populated 640×480 scene.
Combined client/server CPU improves only 0.27–0.69% in populated scenes, with
no consistent client improvement; the empty scene is about 0.8% slower.
This is too small to establish a useful workload improvement, so the source
and installed 1.2.0-2 package remain unchanged.

The benchmark controller encountered a TCP connect timeout during polling.
The native job completed normally (209.8 seconds, exit 0); its recorded output
was recovered and all 20 health checks passed. The private lab client now uses
a bounded connection timeout and retries connection establishment before sending
any request bytes. It does not retry a mutation after transmission. This is a
controller resilience change, not a claimed native networking performance fix.
Evidence: `airtop-font/{drawing-summary.json,*-?.txt,qemu-pass.txt,
probes-result.json,probes-health.json,recover-jobs.json}`.

## Bitmap pool growth: accepted

`ClientMemoryAllocator` previously added the full requested allocation to an
area even when a smaller free block already occupied its end. The allocator
now extends that final free block by only the missing page-rounded bytes.
Live blocks retain their addresses, failed growth leaves the existing block
unchanged, and allocation falls back to other chunks as before. This reduces
unnecessary pool growth; it does not shrink freed storage or change the policy
of retaining partly occupied pools.

Original/candidate/original/candidate native server states were compared, with
three fresh-process runs per state and 100 allocate/fill/free cycles at each
of 1, 4, 16 and 64 MiB. A small bitmap remains live throughout. Where the area
can grow in place, the largest sequence retains 85.16 MiB with the original
and 64.04 MiB with the candidate: **21.12 MiB less, or 24.8%**. The anchor area's
resident-byte measurement shows the same reduction. These are shared pages;
the client and server mappings must not be added together.

A separate gradual-resize fixture grows 125 RGB32 bitmaps from 64×48 to
2048×1536, for eight cycles (1,000 bitmaps, 4.35 GB written). It tests both
freeing the old bitmap before creating its replacement and keeping the old
bitmap until the replacement is ready:

| Deletion order | Original retained mapping | Candidate retained mapping |
| --- | ---: | ---: |
| Free before replacement | 17.33 MiB | 12.04 MiB |
| Free after replacement | 36.27 MiB | 34.93 MiB |

This is a memory optimization. Total CPU in the gradual fixture is effectively
flat at about 1.7 seconds. More frequent, smaller growth calls add about
2.4 ms of server CPU per 1,000 free-before-replacement allocations; the
standalone gradual-growth test makes 62 resize attempts instead of five.
Thirty warm launches per variant of both AirPins and StyledEdit show no
material startup change. Large repeated bitmap fills are also essentially
unchanged when using an existing area.

There is an existing address-layout limit: another mapping can prevent area
expansion. Both variants hit this in one of six 64 MiB runs. Repeated creation
of separate areas then takes about 17 seconds per 100 fills, versus 2.6 seconds
with in-place growth. Both variants also show the slower path in QEMU. These
runs are recorded separately, not counted as a new speedup or silently dropped.
The optimization does not solve that fragmentation behavior.

The direct allocator fixture exercises live clone growth, odd byte sizes,
whole-chunk release, detached allocators and 30,000 mixed allocations with
normal operation, every-other resize failure and every resize failing.
Byte and lifetime checks pass against both source versions in QEMU and
on the Pi. All 96 full drawing hashes across the four native states agree.
EGL/BGLView checks, AirTop's 288 cached/direct drawing comparisons, 20 health
checks and ten pinned installed hashes pass. The repository-built
`rpi4_bitmap_pool_bench` repeats all three workloads in QEMU and natively.
Its `shared` column reports whether every temporary bitmap used the anchor's
area; use it when interpreting CPU and retention results.

The installed server is 2,055,104 bytes, SHA-256
`5905594114002e34e2384cfa1bd9e5bb540b820b18faf828387503a28b07f823`, at
`/boot/home/performance-20261008/bitmap-pool/app_server`. The existing
`app-server-performance` launch amendment selects it. The preceding accepted
scanline server remains byte-for-byte intact at its original path; selecting
that path and rebooting was verified during the comparison. The packaged
server, original kernel, boot archive and the other accepted libraries and
applications are unchanged. Both candidate boots pass native checks.

Evidence: `bitmap-pool/{manifest.json,candidate.patch,comparison.json,
*-pool-*.json,*-resize-*.json,allocator-*-qemu.txt,*-allocator-*.json,
canonical-*-qemu.txt,final-*.json}`. The fixture sources, build commands and
forced-failure harness are retained alongside those results. No memory saving
here is added to a different workload's saving to claim a whole-system total.

## Bitmap client reservations: accepted

The client bitmap allocator reserves 128 MiB of virtual address space around a
small cloned area so the server can grow it in place. Releasing the last bitmap
reference deleted the clone but left the unused reservation behind. Repeated
clone/release cycles therefore accumulated reserved ranges and made subsequent
address searches slower. This is **virtual address space**, not an equivalent
amount of physical RAM.

The library now records whether it owns a reservation and releases that range
on the last reference, allocator destruction, or failed cloning. Read-only and
large mappings, failed reservation attempts and unrelated adjacent reservations
are left alone. The record still occupies 24 bytes on ARM64, and all 11,787
exported dynamic symbol names/types remain unchanged.

Testing that change exposed a second defect: the kernel's unreserve loop began
with `Next()` on an iterator already positioned at the first matching area.
It skipped that first area, so a library-only change could not release a lone
reservation. The loop now processes the current area before advancing. It still
removes only wholly contained reservations and preserves live mappings.
`unreserve_area_tests` fails on the original kernel in QEMU and on the Pi, then
passes on the corrected kernel. Its six cases cover a lone reservation, a
reservation after a freed prefix, live mappings, boundaries, adjacent
reservations and 1,000 repeated reuse cycles.

Private original/candidate/candidate/original library comparisons on the
corrected native kernel explicitly verify the loaded library path. Median
client CPU for the same clone, reference-count and data checks is:

| Clone/release cycles | Original CPU | Candidate CPU |
| --- | ---: | ---: |
| 100 | 3.108 ms | 2.828 ms |
| 1,000 | 40.110 ms | 24.657 ms |
| 2,000 | 109.887 ms | 48.059 ms |

The 1,000-cycle result is about 38.5% less CPU; 2,000 cycles use about 56.3% less.
The original advances through roughly 125 GiB of address space per 1,000 cycles,
with occasional extra gaps from existing mappings. The candidate reuses its
first address on every cycle. These are allocator-workload results, not a
whole-application startup or physical-memory saving. The installed repository
benchmark repeats 2,000 cycles in 47.1 ms of CPU with all 1,999 reuse checks
passing. It supports exact provider and expected-reuse checks through
`EXPECT_BE_PROVIDER` and `EXPECT_RESERVATION_REUSE`.

Both original and candidate ownership fixtures pass their expected behavior
checks in QEMU and natively: last references, destruction with live references,
failed cloning, failed reservations, read-only/large mappings, neighboring
reservations and growth of a shared area. Existing bounded VM tests, 48 matched
original/candidate drawing hashes, both libraries' EGL/BGLView paths, bitmap
resize workloads and both sets of AirTop's 288 drawing cases pass. A QEMU boot
using the default replacement library also passes both C++ ABIs and GUI launch.

The installed libbe remains at `/boot/system/non-packaged/lib/libbe.so`, now
4,606,590 bytes with SHA-256
`11ce36661e8e7a43b13db2844cd8381386fc9eb58d41c49b6ce2cee7b541cf9f`.
The preceding accepted icon-gamma library is preserved as
`/boot/home/performance-20261008/rollback/libbe-bitmap-reservation-original.so`.
Restoring it reproduces reservation growth, and reinstalling the candidate
restores reuse. The original packaged library remains intact.

The native FAT boot archive is now SHA-256
`a6df141897ab625d645e50761f82e2bc47c7e8aeedc6c0f3d4bb727cbe29cfef`.
Its only changed archive member is `system/kernel_arm64`, SHA-256
`292006d5fdd6d6a0207624388fb78ab7249e7a205ce317cddabde52e3300a2ec`.
Serial identifies the running kernel as `hrev60206+766+dirty`; the packaged
kernel file and package-derived system metadata still identify the original
750 build. Use the saved matching candidate kernel for symbol lookup. The
original FAT archive, config and command line are backed up and verified;
booting the original archive restores the expected regression failure with
20 health checks passing. The recovery USB image's hash is unchanged.

After returning to the corrected kernel and installing the library, a final
normal native reboot verifies both. App_server, registrar and AirTop load the
new libbe; Tracker and Deskbar retain their adjacent packaged libraries, so the
library cleanup improvement is not claimed for those two processes. The final
boot passes the new regression, the canonical benchmark, four bounded VM tests,
24 drawing hashes, 288 AirTop cases, EGL/BGLView, both C++ ABIs, StyledEdit launches,
ten pinned installed-file hashes, FAT/archive/config checks and all 20 health
checks. The preexisting DSI source edits remain byte-for-byte unchanged.

Evidence: `bitmap-reservation/{manifest.json,kernel-manifest.json,kernel.patch,
library.patch,old-kernel-*,kernel-original-*,edge-*-qemu.txt,private-*.json,
native-regression-*.json,kernel-rollback-*,kernel-candidate-*,install-*.json,
default-*-qemu.txt,canonical-qemu.txt,default-boot/}`. One boot controller was
interrupted after the second candidate had booted; explicit recovery checks
confirmed the serial revision, archive hash, native regression and health
before proceeding. The final default boot completed normally.
