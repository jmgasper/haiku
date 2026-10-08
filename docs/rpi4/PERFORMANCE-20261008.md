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
The profiled run still verifies the final pixels. A caller-stack profile
is being collected to identify copies and background clearing before
choosing the next change; no presentation improvement is claimed yet.
Evidence: `window-profile.txt` and `window-profile-result.json`.
