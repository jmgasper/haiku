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
