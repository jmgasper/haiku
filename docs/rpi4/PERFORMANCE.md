# Raspberry Pi 4 release performance

Work started 2026-10-06 on `rpi4`, from `e762318b83`. Target: the lab's
4 GB Raspberry Pi 4B revision 1.5, four Cortex-A72 cores at 1.5 GHz, the
existing 8 GB SD card, and the full air/OS image. Improvements must retain
the custom applications, preferences and hardware support.

The installed result is summarized in [STATUS.md](STATUS.md). The sections
below are a chronological experiment record: statements about candidates,
uninstalled builds and pending checks describe that experiment's state at
the time. The final-image measurements at the end distinguish the shipped
combination from isolated A/B results.

## Measurement

Raw evidence belongs outside Git in
`/mnt/HaikuWork/rpi4/evidence/performance-20261006`. That directory also
preserves the starting image and boot archive. The board initially ran
kernel `hrev60097+639` and Summit packages `1.10.0-10` / `git20261005-2`.
Ethernet and Alpine recovery currently use `192.168.1.214`; the older
recorded recovery address was `.209`. Check DHCP rather than assuming
either is permanent.

Native hardware measurements here cover this one Pi. The VM, app_server
and TCP changes also affect other platforms, and the ARM64 library/link
changes affect other ARM64 builds. Performance comparisons on the other
workstation targets were not run during this work.

- `jam rpi4_app_bench` builds the GUI launch probe.
  `rpi4_app_bench 5 /boot/system/apps/StyledEdit` measures elapsed time
  through application registration, its first shown window (2 ms polling),
  and a synchronous reply from its application looper. It quits each app
  normally before the next run. An already running app is an error; it is
  never closed by the probe. Window visibility does not prove the app has
  finished painting all its content.
- `serial-timing.py 110 <output-prefix>` records raw serial and JSON lines
  with host monotonic receipt times. Start it before rebooting. Measure
  differences between markers in the same capture, not from invocation of
  the reboot command. UART and SSH buffering limit the timestamp precision.
  Hold the lab hardware lock, and run no other serial reader concurrently.
- Cold launch means the first launch after a reboot. Later launches on the
  same boot are warm; they must not be presented as cold measurements.
- Compare identical app packages, window sizes, display layouts, pages or
  media, and record failures as well as successful runs. Keep firmware
  boot delay separate from the loader/kernel/userspace interval.

## Starting observations

On the already running system, before changes, first shown window in ms:

| Application | First measured launch | Median of next four |
| --- | ---: | ---: |
| StyledEdit | 224.2 | 142.9 |
| About this system | 232.0 | 134.1 |
| AirPins | 1132.1 | 1099.2 |

These are **not cold-boot measurements**. Evidence: `baseline-apps.txt`.

The first timed warm reboot has 31.1 seconds between the EEPROM banner
and selection of SD boot. The USB-first lab configuration tries the
unbootable thumb drive and the NanoKVM's empty mass-storage device before
reporting a 25-second USB timeout. The air/OS loader starts 2.7 seconds
after SD selection, and enters the kernel 0.14 seconds later. Evidence:
`baseline-warm-1.jsonl`. USB-first boot preserves remote recovery; any
timeout change must be tested with the recovery disk attached as well.

Two further candidates need measurement: GNU symbol hash tables for the
system libraries (the runtime loader already supports them), and Mesa's
shader disk cache (disabled in the starting build). No improvement is
claimed until the changed binaries run on the board.

## Restoring the emulator regression check

The starting tree failed to mount its SD boot volume in QEMU 10.2.0 after
the high-speed card change of 5 October. The short CMD6 transfer leaves a
nonzero block count; writing SDMA_ADDRESS for the next PIO transfer makes
QEMU resume a DMA operation that the driver never intended. The driver now
writes that register only when using DMA. With the same minimum SD image,
the new boot archive reaches the desktop (serial and screenshot:
`pio-fix-qemu.*`). The real board uses DMA, so this does not change its
transfer path.

`qemu-run.sh` no longer forces `airos.debug`. Serial is still captured;
screen debugging can be requested explicitly. The screen debug pager was
stopping unattended runs while printing the boot-volume information.

## GNU hashes: isolated library comparison

Six rebuilt libraries (`libroot`, `libbe`, `libmedia`, `libtranslation`,
`libtracker`, `libnetwork`) have both GNU and SysV hash tables. Keeping the
SysV table preserves the symbol count used by the relocation cache. This
change applies to ARM64 userspace links, not kernel or boot-loader links.

The libraries were staged in a separate directory and selected only for
test processes with `LIBRARY_PATH`; `listimage` confirms the paths. Warm
StyledEdit registration fell from about 71 to 52 ms, and first window from
145 to 125 ms. About registration fell from 51 to 44 ms. AirPins remains
dominated by another cost. Evidence: `gnu-apps-verified.txt`; the earlier
`gnu-apps.txt` is an invalid trial because the board lacks `tar` and did not
unpack the candidate libraries. Full-system boot and broader applications
still need checking before this becomes the installed default.

AirPins' complete startup profile attributes 89% of its app_server window
thread's samples to `View::RebuildClipping()`. The window starts hidden,
but its server-side root view initially starts visible, so adding and
laying out children repeatedly rebuilds clipping that cannot be displayed.
A candidate synchronizes the root view's initial state with the window,
while preserving offscreen bitmap drawing. Native pixel checks and
measurements are described below.

## First installed candidate

The system package and matching FAT boot archive (`hrev60097+644+dirty`)
were installed and rebooted on the board. All four checks in
`rpi4_view_visibility` pass: the first show with a hidden child, changes and
resize while hidden followed by show, minimize/restore, and an offscreen
bitmap drawn without showing its window. Tracker, Deskbar and Terminal
also render and accept input. The test is a standalone Jam target, not an
application shipped in the release image.

Same applications, two 1920x1080 outputs side by side, scale 1:

| Application | Previous warm window, ms | Candidate warm window, ms |
| --- | ---: | ---: |
| StyledEdit | 145 | 119 |
| About this system | 134 | 122 |
| AirPins | 1096 | 325 |

These are medians of four launches after the first launch in each group.
Candidate registration medians are 45, 42 and 50 ms respectively. Evidence:
`baseline-cold-apps.txt`, `candidate1-usb-apps.txt`. The candidate's first
launches were 136, 207 and 403 ms, but occurred after KVM/debugger diagnosis
and are not a clean cold-boot comparison.

The candidate initially could not be reached at its Ethernet address. The
KVM's USB RNDIS link (`10.239.6.100` from the KVM) allowed the checks above;
Ethernet subsequently passed ping and telnet after an interface down/up.
There is no established cause yet. Repeated unattended boots and Wi-Fi
checks remain required. `shell.py` accepts `RPI4_TELNET_PORT` for an SSH
tunnel through the KVM. The first MJPEG frame can be stale, so the KVM
screenshot helper now waits for fresh frames before saving.

A subsequent clean reboot passed Ethernet ping and telnet without any
intervention, automatically joined Gaspers and acquired its Wi-Fi address,
and passed all four drawing checks again (`candidate1-clean-*`). The
earlier connection attempts overlapped boot and serial-debugger stops;
they do not establish an Ethernet regression.

The full image also reaches the branded desktop in QEMU
(`candidate2-qemu-software.*`). Its emmc2 node is redirected to the sole
emulated SDHCI controller, which overlaps the physical Pi's Wi-Fi SDIO
address. The Wi-Fi probe now refuses that conflict instead of resetting
the boot card's controller. The emulator runner removes the HVS and V3D
compatible properties from a temporary DTB: those GPU blocks are not
emulated. The full image uses the generic framebuffer for this check;
GPU and multi-display validation belongs on the real board.

## Persistent OpenGL shader cache

The Pi Mesa build now enables the disk shader cache with compression and a
64 MB limit. Haiku uses Mesa's existing `dladdr`/file timestamp identity
fallback, since it cannot inspect mapped ELF build IDs with
`dl_iterate_phdr`. Changing the driver invalidates the corresponding cache.
An isolated EGL vendor file selected the candidate on the board; the
installed system driver was left available for comparison.

The clear and triangle probes pass pixel-exact checks with seven cache
hits, with a truncated cache entry, in four concurrent processes, and with
an unwritable cache directory. The last case disables caching and still
renders correctly. Evidence: `mesa-cache-resilience.txt` and
`mesa-cache-cleanup-probe.txt`. The integrated build reproduces the tested
`libEGL_mesa.so.0` byte for byte (SHA-256
`bc6ddaae91af0111994aacc6663f3a20bba2bec3df755fc5ca651bf8ba3a34db`).

Summit's compositor shader creation takes about 3–10 ms on cache hits,
compared with 50–78 ms for compilation. Alternating warm launches against
the same local page yielded first tiles at 1.014 and 1.009 seconds with the
cache disabled, and 1.035, 0.973 and 0.847 seconds with it enabled. Which
programs are already cached affects each run, so these are observations,
not a claimed fixed percentage gain. Evidence:
`summit-cache-comparison.txt`. Initial cold application loading still takes
several seconds and remains a separate target for investigation.

## Loader regression coverage

`tools/rpi4/build-loader-tests.sh` cross-builds the existing runtime-loader
suite twice, with SysV-only and dual SysV/GNU hashes, into a package that
can be extracted privately on the target. All 50 checks pass before and
after explicitly masking the GNU Bloom filter's second bit index to the
machine word width. The previous unbounded C++ shift happened to work with
ARM64's masked shift instruction, but was undefined in the language.

The matching system package and boot archive (`hrev60097+648+dirty`) pass
the full-image QEMU desktop check and native reboot, all 50 loader checks,
and the four window drawing checks. Evidence: `loader-tests-before.txt`,
`candidate3-qemu.*`, `candidate3-native-checks.txt`. That native command's
last step names a nonexistent GL probe; the corrected GL and network
checks are recorded separately in `candidate3-network-gl.txt`.

## USB-first EEPROM boot delay

The lab EEPROM retains version 2022-01-25 and `BOOT_ORDER=0xf14`, with
`USB_MSD_DISCOVER_TIMEOUT=5000` and `USB_MSD_LUN_TIMEOUT=500`. Its update
verified and reset, then booted the attached NanoKVM recovery disk. Linux
reported the new configuration and unchanged firmware version. The SD FAT
partition was backed up before staging the update; all update files were
removed afterwards. Evidence: `recovery-before-timeout.*`,
`eeprom-after-reboot.txt`, `eeprom-cleanup.txt`.

With recovery detached and both the thumb drive and empty NanoKVM LUN
present, the first cold-power test selected SD 8.58 seconds after the
EEPROM banner, versus 31.07 seconds previously: **22.5 seconds saved**.
The display layout appeared at 23.86 seconds and Ethernet link at 26.66
seconds after the banner, versus 46.45 and 49.31 seconds on the preceding
system build. These are serial milestones, not measurements of the first
fully drawn desktop. Native telnet and automatic Wi-Fi joining also pass.
Evidence: `candidate3-clean-boot.*`, `eeprom-fast-sd-1.*`,
`eeprom-fast-sd-1-checks.txt`. Repeated warm and cold boots remain planned.

This change is stored in the lab board's EEPROM, separately from the SD
image. A user's board configured to try SD first does not incur this lab's
USB recovery timeout in the first place.

The second test, a warm reboot, selected SD 8.57 seconds after the EEPROM
banner and brought Ethernet link up at 26.89 seconds. The shorter wait is
repeatable across these cold and warm boots (`eeprom-fast-sd-2.*`).

## Locating cold-launch costs

`HAIKU_LOADER_TIMING=1` reports individual file mapping times and the
program's load, relocation, protection-remap and initialization phases.
It is off by default. The loader's private `printf` also now writes at the
current stderr position and bounds the formatted length: its previous
positioned writes overwrote redirected diagnostics at offset zero.

After a native reboot, Summit spends 4.137 seconds loading images,
1.369 seconds relocating, 0.011 seconds remapping protection and 0.106
seconds initializing. File mapping itself accounts for 3.649 seconds:
ICU's data segment 0.765, WebKit 0.684, JavaScriptCore 0.567, OpenSSL crypto
0.332 seconds. `vm_map_file()` requests up to 10 MB of prefetch per mapping;
packagefs services these requests synchronously. The recorded trace is
`loader-timing-cold-fixed.txt`. The earlier `loader-timing-cold.txt` is
corrupted by the diagnostic output bug and is not usable for phase timing.

The full image reaches the desktop in QEMU with this instrumentation.
After rebooting the installed `hrev60097+650+dirty` package and matching
archive, all 50 loader checks pass with timing enabled, and a redirected
`true` invocation retains all six map lines plus its phase summary.
Evidence: `loader-timing-fixed-qemu.*`, `loader-timing-cold-fixed.txt`.

## Bounded initial file prefetch

The initial mapping prefetch is now capped at 1 MiB instead of 10 MiB.
The first native cold Summit launch on `hrev60097+651+dirty` reaches the
browser-ready marker in 3.729 seconds, versus 5.655 seconds on the previous
build. First tiles arrive in 9.768 versus 11.675 seconds. These are one
installed-app cold launch per build; the separate engine comparison below
uses repeated boots. The smaller read shifts some work into page faults:
load time falls from 4.137 to 1.566 seconds while relocation rises from
1.369 to 1.934 seconds. Total startup improves. Evidence:
`prefetch-1m-cold.txt`.

Warm StyledEdit and AirPins window medians remain about 119 and 329 ms.
The texture regression probe passes 40 rounds on the existing uncached
driver. The memory-pressure check reaches 6 MB free with 181 MB swapped,
then verifies all data, releases it with `MADV_FREE`, and reports `OK`.
Login becomes unresponsive during the severe pressure, but recovers without
a reboot. The suspended-child cleanup check also completes. Evidence:
`prefetch-1m-regression.txt`, `prefetch-1m-memory-serial.*`.

## Restricting WebKit exports

The WebRTC engine in `summit-rtc/WebKitBuild` reproduces the installed
1.10.0-10 library byte for byte after stripping and setting its package
RPATH. The older `summit-gl` engine is not a suitable release replacement:
it omits newer features. A version script now exports the `WK*` C API,
native `BWebKit` classes, helper process entry points and Haiku ABI markers.
It reduces global/weak dynamic definitions from 206,091 to 1,804 and the
stripped library from 140,607,624 to 105,306,968 bytes.

Control and candidate bundles were installed in the same private hpkg,
with identical browsers and dependency libraries. Their relative RPATHs
select the intended engine, confirmed with `listimage`. Alternating four
fresh boots with the 1 MiB prefetch kernel yielded:

| Engine | Browser ready, seconds | First tiles, seconds |
| --- | ---: | ---: |
| Control, first boot | 3.881 | 10.518 |
| Restricted exports, first boot | 2.779 | 9.086 |
| Control, second boot | 3.881 | 9.953 |
| Restricted exports, second boot | 2.782 | 9.090 |

Relocation time falls from about 1.983 to 1.005 seconds. Every boot passes
the local JavaScript, WebAssembly, WebGL pixel and basic WebRTC/API smoke
checks. Evidence: `webkit-ab-cold-*.bench.txt`. The DOM workload's median
round times in a subsequent warm control/candidate/candidate/control
sequence are 1527, 1467, 1484 and 1548 ms. This is a small local workload,
not a general browser benchmark (`webkit-dom-bench.txt`). The earlier
10-second DOM polling in the cold test stopped before its result and is
not usable for DOM timing.

`check-exports.py` checks Summit, WebProcess, NetworkProcess and Natter's
dynamic requirements. Their native WebKit symbols remain exported;
ordinary C++ allocation operators resolve from the existing libstdc++
dependency. This static check supplements runtime coverage. The packaged
1.10.0-11 library reproduces the tested candidate exactly (SHA-256
`337b6eeeeaeb615552aae8eff9330b5109a33c1d108a89f6cee4b022cb31f751`).
Evidence: `webkit-exports-coverage.txt`, `webkit-exports-package.txt`.

Discarded comparisons are retained as evidence: `summit-exports-smoke.txt`
used a library path that the executable's absolute RPATH overrode, and
`summit-exports-preload-smoke.txt` placed the candidate outside the helper
process directory layout. Neither establishes candidate performance.

## Failed TCP child cleanup during reboot

A native reboot on `hrev60097+652+dirty` stopped in the kernel debugger
with `bound endpoint ... not in hash!` while net_server destroyed its
port-23 listener. The child was still SYN_RECEIVED with no initialized
send path; the listener was the only connection-table entry.
`spawn_pending_socket()` copies the listener's address before TCP binding,
but `_Spawn()` left failed children in the backlog and retained that
address. Destruction then mistook the never-bound child for a bound socket.

TCP now clears the inherited address before binding and aborts incomplete
children on open, bind, route preparation and SYN-ACK send failures. The
abort runs after releasing the child lock because removing the listener's
last reference can destroy the child immediately. The bound-table panic
remains in place.

`tcp_shell --spawn-failures` runs the real TCP endpoint and manager in
Haiku's kernel emulation harness. On the Pi, 64 repetitions of each of the
four injected failures and of a successful SYN/setup/cleanup pass (320
cycles), including immediate destruction and backlog reuse. Linking the
same harness against the original endpoint fails on the first open error
with an incomplete child left behind. This is a focused lifecycle test;
reboot stress on the installed kernel is a separate check. Evidence:
`v3d-cache2-native-boot.log`, `tcp-unbind-panic-inspect.*`,
`tcp-spawn-failures.txt`, `tcp-spawn-unfixed.txt`.

The full `rpi4-airos` image builds and reaches the branded desktop in QEMU
with the TCP change (`tcp-fix-profile-build.log`,
`tcp-fix-profile-qemu.*`). The final harness rerun also passes all 320
cases (`tcp-spawn-final.txt`).

The installed `hrev60097+654+dirty` kernel then completed a normal reboot
and a reboot under connection churn. Two host workers repeatedly opened
and closed TCP port 23 for 70 seconds, crossing shutdown and startup:
260 connections completed, with refusals/timeouts during the reboot and
no kernel panic. Evidence: `tcp-fix-654-first-boot.*`,
`tcp-fix-654-reboot-churn.*`, `tcp-reboot-churn-client.txt`.

## Cached tiled textures

An opt-in V3D buffer protocol adds explicit CPU/GPU ownership for Normal-WB
allocations. Mesa uses it only for tiled textures, excluding persistent and
coherent mappings; ordinary buffers and Vulkan retain their previous type.
Every CPU interval waits for the last GPU job. Read intervals invalidate,
write intervals additionally mark dirty, and submission cleans dirty lines.
All aliases share the memory type and whole-page allocations avoid sharing
cache lines with unrelated objects. Capability version 2 adds read-only
intervals; old kernels cause Mesa to keep its existing allocation path.

On native kernels +653/+654, the direct TFU probe passes 64 alternating
read/write/GPU cycles. The texture probe passes 200 small and 100 full-HD
rounds, checking every byte after uploads, repeated maps, overlapping
subimage updates, GPU draws, narrow-band reads, and PBO readback. Vulkan's
headless clear and triangle remain pixel-exact. The full-HD run took
73.673 seconds. Evidence: `v3d-cache2-native.txt`,
`v3d-cache2-full-size.txt`.

With the same isolated Mesa library and installed WebKit 1.10.0-11, a
1024x768 WebGL animation in a fixed 1150x940 window gave these 30-second
measurements after five seconds of warm-up:

| Texture mapping | FPS | 95th percentile frame interval, ms |
| --- | ---: | ---: |
| Existing, first run | 31.41 | 33 |
| Cached, first run | 49.08 | 22 |
| Cached, second run | 49.75 | 21 |
| Existing, second run | 31.39 | 33 |

`listimage` confirms the intended Mesa and WebKit libraries in the web
processes. This is a local workload, not a general WebGL score.
`v3d-cache2-renderbench-repeat.txt` is the valid alternating comparison;
the earlier `v3d-cache2-renderbench.txt` includes two failed launches caused
by a wrong executable path and is not the four-run comparison.

Full-HD client readback falls from a median 55.45 to 39.8 ms; read-only tracking
keeps the preceding GPU finish near 2 ms instead of the first experiment's
4.7 ms. Pbuffer readback falls from a median 59.9 to 43.85 ms. A 64-row band
is slower (about 1.7 to 3.4 ms) because preparing the texture invalidates
the whole allocation. PBO CPU-copy time stays near 19.6 ms because its
buffer remains write-combining. Evidence: `v3d-cache2-readback.txt`.

The build script reproduces the isolated tested Mesa library byte for byte
(SHA-256 `a89890d31d1dfd6f65d3650374247ec8e096519243198c608e9758e81b78a475`).
The driver and source patches are being retained with the feature default
off while scrolling and broader application checks are collected.

A content-heavy local page also improves. Four alternating existing/cached/
cached/existing browser runs each delivered four 80-notch wheel bursts
(25 ms spacing, alternating down/up) in the same window. All 1,280 events
were delivered with status zero. Native-view frame counts yield roughly
23.2–24.2 FPS with existing mapping versus 36.3–39.1 FPS with cached
textures. These use the frame-counter snapshot taken at completion of each
burst, excluding the later idle polling gap. The page was in manual mode;
no requestAnimationFrame auto-scroll ran alongside the wheel test. Evidence:
`v3d-cache2-scroll.txt`.

## ARM64 user memory copies and overlapping moves

The scalar AArch64 routine from [Arm optimized-routines, pinned revision
503fafe311c177de0e571c458c7c337b1ca5f522](https://github.com/ARM-software/optimized-routines/blob/503fafe311c177de0e571c458c7c337b1ca5f522/string/aarch64/memcpy.S)
outperformed its Advanced SIMD alternative on this Cortex-A72 in the
alignment sweep. The user libroot candidate uses the scalar assembly for
both memcpy and memmove, with Haiku symbol/CFI annotations and the upstream
MIT notice preserved. The runtime loader shares that architecture object;
its separate musl memmove input is omitted on ARM64. The kernel retains
its existing scalar C routine.

In an original/candidate/candidate/original comparison, `dladdr` verifies
that both symbols resolve to the selected libroot. Each run has three
rounds; these are medians in MB/s for warm buffers:

| Operation | Bytes | Destination offset / overlap | Original | Candidate |
| --- | ---: | --- | ---: | ---: |
| Copy | 128 | aligned destination | 5986 | 7983 |
| Copy | 4096 | destination +3 | 7748 | 11460 |
| Copy | 8388608 | aligned destination | 1976 | 1984 |
| Copy | 8388608 | destination +3 | 1114 | 1996 |
| Move backward | 128 | overlap by offset 1 | 652 | 7369 |
| Move backward | 4096 | overlap by offset 1 | 746 | 11407 |
| Move forward | 4096 | overlap by offset 1 | 746 | 11404 |
| Move backward | 8388608 | overlap by offset 64 | 1087 | 1076 |

This primarily fixes unaligned-copy and small/medium overlapping-move
costs. Large aligned copies and large aligned overlapping moves are
essentially unchanged; the table is not an application-level speedup.
`jam rpi4_memory_bench` builds the reproducible benchmark. Evidence:
`memory-copy-first.txt`, `memory-copy-alignment.txt`,
`arm-copy-libroot-bench.txt`. The first experiment used unaligned
destinations only; the later alignment sweep includes aligned cases.

The actual candidate libroot passes 51,301 memcpy alignment, canary and
protected/read-only-page cases, plus 549,027 memmove cases covering both
overlap directions, 32 alignments, canaries and protected-page edges.
`jam rpi4_memory_move_probe` builds the latter. All 50 runtime-loader
checks also pass with the candidate library path. These are isolated
user-process tests, before replacing the system library. Evidence:
`arm-copy-libroot-checks.txt`.

The four visibility/pixel checks pass with original and candidate libroot
in alternating runs. Warm StyledEdit windows remain near 119–121 ms and
AirPins near 325–330 ms; this isolated user-library change has not shown a
meaningful launch-time improvement in those two apps. App_server still
uses the installed library in these tests. Evidence:
`arm-copy-app-bench.txt`.

The complete ARM64 image with the new library and runtime-loader string
object builds and boots to the branded desktop in QEMU (`arm-copy-full-build.log`, `arm-copy-qemu.*`). This includes the staged WebKit 1.10.0-11
package and the cached-texture-capable Mesa, still default off in that
image.

After the scrolling and application checks, cached textures are enabled by
default. `V3D_HAIKU_CACHED_TEXTURES=0` remains available for comparison, and
an older kernel automatically uses write-combining buffers. The default-on
library (SHA-256
`ea84cc103cdfc2426cc3cd403cfa5be87b745271818556a3483110c079123aec`)
passes 100 texture-probe rounds and the full-HD clear/triangle pixel check
without environment overrides (`gpu-default-native.txt`). GLTeapot also
renders correctly (`glteapot-cached.jpg`); its displayed FPS is a spot
check, not an alternating comparison.

The full +656 core and matching boot archive were installed and rebooted
on the Pi. The installed libroot passes all 51,301 copy cases and 549,027
move cases; the installed runtime loader passes all 50 checks. All four
visibility tests pass. Warm StyledEdit and AirPins window times remain near
117–119 and 320–336 ms respectively. Evidence:
`arm-copy-656-install.txt`, `arm-copy-656-boot.*`,
`arm-copy-656-native.txt`. The core package SHA-256 is
`d47b8ce6107895de31f61135061fc494c14f57faca8c20fd9c62532707f54104`;
the boot archive is
`3e1a3b62758246f6fa60b9892af22b4a915d28ac8411573fd479565a5f81ce16`.

With the installed library and default GPU settings, Summit passes the 12
JavaScript/Wasm/WebRTC/WebGL smoke checks. The local WebGL animation runs
at 52.67 FPS with a 21 ms 95th-percentile interval in one 30-second check.
`listimage` confirms the system WebKit and Mesa libraries. This is a
combined-system check, not an isolated attribution of the difference from
the earlier cached-texture runs (`gpu-default-656-apps.txt`). The attempted
Natter launch benchmark in that file names its containing directory, so the
roster reports Tracker already running; it is not a Natter measurement.
Using the executable `/boot/system/apps/Natter/Natter`, Natter opens and
answers its looper in 939 ms (`utile/native-results.txt`). The benchmark now
rejects directory arguments before asking the roster to launch them.

## HEVC picture conversion

SAND conversion now processes eight rows of one column at a time, keeping
the column input and row output local. It retains the existing pixel
formats, crop behavior and NEON unpacking. An isolated sweep of tile heights
1–64 selected eight as a useful compromise across I420, NV12 and P010 at
1080p and 4K. A separate NEON table-shuffle experiment did not improve the
ten-bit path and was not adopted. Evidence: `sand/traversal-results.txt`,
`sand/table-results.txt`.

On the actual hardware decoder, original/candidate/candidate/original runs
give these ranges, with decoded pictures converted but not written to disk:

| Stream | Original FPS | Candidate FPS | Original plane conversion, ms | Candidate, ms |
| --- | ---: | ---: | ---: | ---: |
| 1080p eight-bit | 113.4 | 122.6–122.9 | 3.6 | 2.9 |
| 1080p ten-bit | 68.8 | 72.1–72.5 | 8.6 | 7.8–7.9 |
| 4K eight-bit | 27.6–27.7 | 30.4–30.5 | 16.1–16.2 | 12.7 |
| 4K ten-bit | 16.3 | 17.8 | 38.1 | 32.6–32.8 |

All 25 hardware-decoded conformance streams retain their host FFmpeg MD5,
including ten-bit, odd dimensions, weighted prediction and 4K. The new
`jam rpi4_sand_convert_test` checks 1,506 conversions against an independent
per-sample reference, covering random crops, independent plane strides,
unaligned source addresses, padding, canaries and unchanged source data.
It passes natively with NEON and on the host with ASan/UBSan. The complete
image builds and reaches the branded desktop in QEMU. Evidence:
`sand/conformance.txt`, `sand/conformance-verification.txt`,
`sand/native-converter-test.txt`, `sand/decoder-bench.txt`, `sand/qemu.*`.

The candidate Media Kit add-on is confirmed in airTime by `listimage`.
Both 1080p eight-bit and ten-bit playback remain at approximately 30 FPS
with no steady-state drops (`sand/airtime-candidate.txt`). A separately
encoded 30-second 4K eight-bit fixture also plays at 30 FPS with no drops
in all four original/candidate/candidate/original runs. The conversion
benchmark gain has **not** produced a playback-FPS gain on that already
real-time clip (`sand/airtime-4k-abba.txt`). The fixture is an x265 ultrafast
CRF-28 upscale of `multi.mkv`, 3840x2160 at 30 FPS, SHA-256
`5df0e26d72cbb002c1860f2bd803fc78724e58fb1755cb065f3267e615c87092`.
It is not evidence that arbitrary 4K content, or 4K ten-bit, plays smoothly.


## Reusing TCP connections

`EndpointManager::SetConnection()` changed the local/peer addresses before
removing the endpoint from its intrusive hash table. Removal therefore
searched the new bucket, leaving a stale entry in the old bucket. Reusing
the endpoint across different buckets could eventually create a cycle and
stop connection lookup. Removal now precedes the address change.

The native `tcp_shell --connection-reuse` regression uses 512 different
peer tuples, verifies that each previous tuple disappears, and checks the
final unbind. The original manager stops making progress and the test's
10-second watchdog fails it; the candidate completes all 512 changes.
All 320 `--spawn-failures` cases still pass. This is the actual TCP/manager
code running in the existing kernel emulation harness, not a model.
Evidence: `tcp-reuse/native2.txt` (control) and `tcp-reuse/native3.txt`
(candidate). The first fixture kept all tuples in the same hash bucket and
did not expose the defect. An incremental rebuild also reused the control
object within one timestamp tick; the final candidate was explicitly
rebuilt, SHA-256 `c2cc66226765509020b98a38de11b988104f0920dda566c7bd5ec377ca2504bd`.

The complete `hrev60097+659+dirty` image builds and reaches the branded
QEMU desktop (`tcp-reuse/full-build.log`, `tcp-reuse/qemu.*`). Native kernel
installation and final-image acceptance remain separate steps.


## GPU-assisted frame readback

Mesa now blits large, read-only color maps into a linear staging texture,
using the existing version-2 CPU/GPU ownership protocol. V3D stores the
pixels in CPU row order, avoiding CPU detiling and a temporary copy. Small
reads and unsupported formats/flags retain the original path. The driver
option `V3D_HAIKU_GPU_READBACK=0` restores the previous behavior; disabling
cached textures or using an older kernel also disables the new path.

The first prototype improved full-HD RGBA copies from roughly 39–40 ms to
10–11 ms, but **regressed Summit**. Tracing showed that Summit's previous
384 KiB readback bands became separate GPU jobs. The final driver keeps
maps below 128 Ki pixels on the CPU and exposes an optional current-context
performance hint. WebKit uses whole rectangles when that hint is present
and enabled, and retains bands with an older/disabled driver. Explicit
`SUMMIT_READBACK_BAND_KB` settings still override the hint.

With the same private Mesa and WebKit binaries, alternating off/on/on/off
runs give these ranges. The control uses the existing cached textures and
CPU bands; the candidate automatically uses GPU readback and whole reads.

| Window frame | Workload | Control FPS | Candidate FPS |
| --- | --- | ---: | ---: |
| (10,60)–(1160,1000) | WebGL requestAnimationFrame | 59.3–59.5 | 59.78–59.79 |
| (10,60)–(1160,1000) | Scrolling, native view frames | 46.7–49.2 | 53.3–57.8 |
| (0,25)–(1919,1075) | WebGL requestAnimationFrame | 40.7–40.9 | 59.78–59.79 |
| (0,25)–(1919,1075) | Scrolling, native view frames | 24.8–26.3 | 38.8–39.8 |

The local WebGL canvas is 1024x768; the larger window also changes the
compositor/readback area. Scrolling uses four 80-event bursts per run,
25 ms between events, alternating direction. Every burst delivers all
80 events with status 0. Rates use frame notifications delivered to the
native view at burst completion, not idle time afterward. Neither metric
is a physical display-refresh measurement. Evidence:
`gpu-readback/automatic-browser.txt`, `gpu-readback/fullscreen-browser.txt`.
`listimage` confirms the private libraries in the browser processes.

Native checks cover all 48 format/MSAA/mip/crop/padding combinations with
GPU readback on and off, and with cached textures disabled. The hint
returns -1 with no current context and in fallback configurations. The
expanded texture probe passes 100 small and 20 full-HD rounds with repeated
maps, overlapping writes, patterned crops, narrow bands and PBOs. The full-HD
depth/clear/triangle check validates all 2,073,600 pixels. Evidence:
`gpu-readback/repo-readback-results.txt`, `gpu-readback/release-default-native.txt`.
Earlier experimental positional `pipe_box` initialization was wrong and
caused GPU MMU faults; it was rejected. The final code uses named fields,
and the subsequent integrated serial capture has no such faults.

All 28 patched Mesa source files reproduce from the pinned base. Patch
application is checked on copies before touching source files, including
clean, partially applied, already applied and conflicting states. WebKit
retains its 1,804 exports and passes the consumer symbol checks. Its release
package is `summit_webkit-1.10.0-12-arm64.hpkg`, SHA-256
`d3818f4148e8300c9cb5418e6b7ed52733bffd597d51d5de623857f3d05b5aef`.
The packaged engine's SHA-256 is
`635874118ebcdba2af507c121f7e0d48e4ae4ede0eb3d6455f26f5c684380114`.
It differs from the private candidate only in 56 unused RPATH-padding bytes:
CMake writes zeros where patchelf wrote `X` (`engine-byte-comparison.txt`).
The final Mesa also checks resource-allocation failure before dereferencing
the new resource; its SHA-256 is
`e62fc032edb97ebca6479349803fe14c17aad274817452bedfc73498b7b7218b`.
It passes the 48-case readback probe and four patterned full-HD rounds
(`final-mesa-native.txt`). The complete `hrev60097+660+dirty` image reaches
the branded QEMU desktop (`final-full-build.log`, `qemu.png`).

## ARM64 user memory fills

The user library and runtime loader now use the Arm optimized `memset`
from the same pinned revision as the copy routines. The upstream instruction
sequence is unchanged; only symbol macros, unwind annotations and the stack
note are adapted. Advanced SIMD is already enabled for user threads. The
zero-fill path checks DCZID_EL0, including its prohibition bit, before using
64-byte DC ZVA. Kernel memory fills retain the separate scalar implementation.

On the Pi, `rpi4_memory_set_probe` passes 497,300 checks covering ten input
values (including integer truncation), 64 alignments, exact boundaries,
canaries and inaccessible guard pages. `--graphics` additionally passes
8,192 cases on real write-combining and write-back V3D allocations. The
isolated candidate libroot passes the same checks, all 50 loader cases and
all four view-visibility checks. The full image boots to the branded desktop
in QEMU, exercising the new runtime-loader implementation too.

The native ABBA microbenchmark calls the installed and candidate entry points
through function pointers. Representative mean throughput in MB/s:

| Bytes / value / alignment | Installed scalar | Candidate |
| --- | ---: | ---: |
| 32 / nonzero / +3 | 727 | 3,933 |
| 128 / nonzero / aligned | 3,804 | 10,644 |
| 1,024 / zero / aligned | 5,338 | 16,027 |
| 4,096 / nonzero / aligned | 5,815 | 11,640 |
| 1 MiB / nonzero / aligned | 5,149 | 6,344 |
| 8 MiB / nonzero / aligned | 2,688 | 2,724 |

These are memory-operation results, not whole-application speedups. Warm
StyledEdit launch remains around 119–124 ms. Evidence is in `memset/` under
the performance evidence directory: `native.txt`, `graphics.txt`,
`libroot-native.txt`, `full-build.log`, and `qemu.{log,png}`.

The preceding integrated native build (+660) also passes all 50 loader
cases, the 48 GPU readback combinations, full-HD depth/triangle pixels,
Vulkan clear/triangle pixels, 1,506 SAND conversions and both TCP regression
harnesses. Its global WebKit 12 engine passes the browser smoke fixture.
See `gpu-readback/native-660-{install,startup,acceptance}.txt`. The recorded
5.109-second first-tile launch used a fresh browser profile; it is not an
A/B comparison with the earlier persistent-profile startup measurements.

## Zstandard package compression

The arm64 bootstrap repository lacks Zstandard, leaving packagefs to inflate
zlib chunks during demand paging. A pinned cross-build of Zstandard 1.5.7
now enables Haiku's existing boot, kernel and user codec support. Application
staging recompresses copies while preserving package payload and metadata;
the source release packages remain unchanged. The runtime package itself
uses zlib for upgrade compatibility. See `BOOT.md` for the transition order.

Four cold boots compare identical private Summit/WebKit payloads in
zlib/Zstandard/Zstandard/zlib order, using the same persistent profile and
local fixture. All 116 regular files across both installations match their
host SHA-256 values; all four browser runs pass the 12-case smoke fixture.

| Phase, seconds after launch | zlib runs | Zstandard runs |
| --- | ---: | ---: |
| Browser ready | 2.905, 2.906 | 2.643, 2.642 |
| First frame with tiles | 9.372, 9.400 | 8.486, 8.500 |

First content improves by about 9.5%; readiness improves by about 9%.
The browser comparison package shrinks from 79,704,347 to 72,437,855 bytes.
The new core package shrinks from 36,229,313 to 32,587,810 bytes. These
measurements isolate compression, not changes to browser code or settings.

The new core boots in QEMU with Zstandard compression. On the Pi the
zlib-compressed transition core boots, passes all memory-fill checks,
50 loader cases and four view-visibility cases, and reads both comparison
packages. Zstandard's native fuzzer passes 1,000 randomized cases with seed 1
(`--no-big-tests --no-long-tests`). Evidence: `zstd/cold-*.txt`,
`native-payload-and-smoke.txt`, `native-fuzzer.txt`,
`native-662-acceptance.txt`; build/QEMU evidence is in `/rpi4/zstd/`.

## ARM64 string comparison

The user library and runtime loader now use Arm's scalar `strcmp` from
optimized-routines revision `503fafe311c177de0e571c458c7c337b1ca5f522`.
The instruction sequence is unchanged; symbol macros, unwind annotations
and the stack note are adapted for Haiku. Kernel and boot strings retain
their existing implementation.

The native probe checks 6,422,376 comparisons against a byte-wise oracle,
including all 16-by-16 alignment combinations, unsigned bytes, early and
late differences, and strings ending immediately before inaccessible pages.
It compares result signs, as required by ISO C, rather than assuming a
particular nonzero return value. A private candidate libroot passes the
same probe, all 50 loader cases, all four view-visibility checks and Summit's
12-case browser fixture. `listimage` confirms the candidate in the browser
and its child processes. The new runtime loader also boots the full image
to the branded desktop in QEMU.

Four alternating native benchmark rounds compare installed and candidate
entry points through function pointers. Mean nanoseconds per equal-string
comparison (the second string's offset is relative to aligned storage):

| Bytes / second-string alignment | Installed | Candidate |
| --- | ---: | ---: |
| 8 / aligned | 20.18 | 15.36 |
| 32 / +3 | 65.43 | 24.03 |
| 128 / aligned | 93.44 | 43.05 |
| 128 / +3 | 193.59 | 47.60 |
| 512 / aligned | 285.64 | 158.88 |
| 4,096 / +3 | 5,488.81 | 1,107.65 |

These are comparison microbenchmarks, not measured application speedups.
Evidence: `strcmp/native.txt`, `libroot-native.txt`, `summit-native.txt`,
`full-build.log` and `qemu.{log,png}` under the performance evidence directory.

## Integrated measurements and rejected follow-up experiments

The later integrated browser workload uses the global WebKit 12 package,
the same dual-1080p desktop, and a 1920x908 content area. Its WebGL fixture
runs at about 59 FPS, while the scrolling fixture reports about 29–31 FPS.
The earlier isolated readback comparison reached 38.8–39.8 scrolling FPS;
that absolute rate did not reproduce in the integrated sequence. Fresh
browser profiles, the earlier private WebKit binary, and the earlier Mesa
readback-hint binary all reproduce the later, lower scrolling rate. Do not
present the earlier isolated rate as a final-image result.

Whole-system sampling attributes most of the browser's app_server window
thread time to bitmap copies: roughly half drawing into the back buffer
and half copying back to front. The browser's short `draw(app_server)`
timer omits synchronization inside `DrawBitmap()` and is not a measure of
all server presentation work. Evidence: `arm-boost/scroll-*.txt` and
`scroll-{all,full}.profile`.

An additional copy microbenchmark checks real V3D write-combining and
write-back allocations, row-sized and frame-sized transfers, and four
alignments. The installed Arm GPR copy routine does not regress relative
to the original C routine. An alternative SIMD routine is worse for some
misalignments, so it is not installed. All copied bytes match. Evidence:
`arm-boost/graphics-copy-{bench.cpp,native.txt}`.

Five additional HEVC SAND prefetch variants pass 7,500 conversion and
canary cases, but none consistently improves the existing eight-row
converter. They are rejected; the shipping converter is unchanged.
Evidence: `sand-prefetch/decision.txt` and the accompanying raw results.

A combined stress pilot with three concurrent Zstandard basic-test
workers passes 15 complete graphics/Vulkan/HEVC rounds, then cannot
allocate a 1,020-page contiguous HEVC picture in round 16. The board
remains responsive and the same stream decodes to the reference checksum
after the workers exit. This is an outstanding allocation-pressure limit,
not a passed endurance test. A separate clock endurance run skips the
large compression setup tests and uses the randomized workloads directly.
Evidence: `arm-boost/soak-monotonic-memory-pilot.txt` and `serial.log`.

## ARM64 string length

The user library and runtime loader also use Arm's `strlen` from the same
optimized-routines revision. Its 304 instruction bytes exactly match the
upstream build, including the backward-compatible BTI entry instruction.
It uses SIMD for longer strings and checks page boundaries before unaligned
loads. Kernel and boot implementations are unchanged; this routine does not
support memory tagging, which the current ARM64 port does not enable.

The standalone comparison passes 2,212,800 constructed-length checks across
the installed, candidate and forced-page-boundary implementations. The public
probe supplies 737,600 checks against the installed library. A private libroot
passes the string and memory guards, 50 loader cases, four view checks and
the 12-case browser fixture. The full image boots to the desktop in QEMU.
An initial visible-window test ran with the screen blanker active and failed;
after waking the display, all four view checks pass.

Four alternating benchmark rounds at 1.8 GHz give these median nanoseconds:

| Bytes / alignment | Installed | Candidate |
| --- | ---: | ---: |
| 3 / aligned | 11.72 | 7.82 |
| 8 / +3 | 23.12 | 7.82 |
| 128 / aligned | 30.63 | 21.44 |
| 512 / +3 | 113.40 | 62.11 |
| 4,096 / aligned | 636.20 | 427.00 |
| 32,768 / +3 | 5,167.19 | 3,314.94 |

These are string microbenchmarks, not whole-application improvements.
Evidence: `strlen/benchmark-native.txt`, `correctness-under-load.txt`,
`private-library-native-trace.txt`, `instruction-verification.txt`, and
`qemu.{log,png}`.

## Native EGL window bitmap reuse

Window swaps previously allocated a BBitmap, cleared it, copied the frame,
published it, and deleted the retired bitmap on every frame. Mesa now keeps
that retired bitmap for the next swap and clears only row padding. The
displayed bitmap remains untouched until the view retires it under its draw
lock. Size changes discard mismatched retired storage; destruction releases
both bitmaps. This keeps one additional frame-sized bitmap per active window.

The native probe checks published pixels, alpha and padding immediately,
then independently waits for matching screen pixels. Two create/destroy
cycles each alternate six sizes from 63x65 to 1920x908. Both the old and new
libraries pass. The probe explicitly hides the pointer and keeps its window
within HDMI0; earlier harness attempts crossed the display boundary or
included the software pointer in the screenshot and are excluded.

An idle 1.8 GHz ABBA comparison waits for every BView draw, after four warmup
frames. Rates are completed window draws per second, not physical refresh:

| Window size | Old, two runs | Reuse, two runs |
| --- | ---: | ---: |
| 640x480 | 205.47, 204.80 | 226.65, 229.02 |
| 1280x720 | 81.12, 81.06 | 91.88, 92.04 |
| 1920x908 | 45.54, 45.47 | 51.83, 51.92 |

This improves completed native-window drawing by about 11–14%. Summit uses
a separate pbuffer path, so these figures do not claim a browser speedup.
The ordered Mesa patch series passes clean, four-patch-prefix, already-applied
and conflicting-source checks; the conflict leaves source files unchanged.
Evidence: `egl-window/{control-correctness-7,candidate-correctness}.txt`,
`bench-*.txt`, `test-notes.txt`, and `patch-tests/results.txt`.

## Firmware-supported CPU boost

`arm_boost=1` lets Raspberry Pi firmware expose its supported turbo limit.
On the lab revision 1.5 Pi 4 this is 1.8 GHz, up from 1.5 GHz. The image does
not set an explicit `arm_freq`, `force_turbo`, voltage override or GPU overclock.
V3D and the core remain at 500 MHz. Older revisions retain the limit chosen
by their firmware. The board revision and supported behavior are checked
against the [official Raspberry Pi `arm_boost` documentation](https://www.raspberrypi.com/documentation/computers/config_txt.html#arm_boost).

Four actual cold-power boots compare 1.5 / 1.8 / 1.8 / 1.5 GHz with identical
core packages, global Mesa/WebKit and persistent browser profile. Readiness
and first content are separate browser milestones; window visibility is not
proof that an application has finished all painting.

These clock-comparison runs hash the engine before launch, warming its file
cache. They compare the first browser process after each cold boot under
that same preparation; they are not measurements of an untouched cold file
cache. The final-image harness moves byte verification after the first launch.

| Measurement | 1.5 GHz, two runs | 1.8 GHz, two runs |
| --- | ---: | ---: |
| First Summit ready, seconds | 2.295, 2.296 | 2.202, 2.201 |
| First browser tiles, seconds | 4.817, 4.829 | 4.681, 4.609 |
| Warm StyledEdit window, ms | 118.055, 118.371 | 105.171, 104.758 |
| Warm About window, ms | 124.532, 123.681 | 108.850, 107.781 |
| Warm AirPins window, ms | 325.770, 325.689 | 280.317, 279.177 |
| Warm Natter window, ms | 372.292, 375.006 | 332.715, 336.881 |
| DOM fixture median, ms | 1513, 1485 | 1270, 1260 |

Warm values are medians of launches two through five. The synthetic CPU
loop gains about 20%; the DOM workload takes about 15.6% less time. Full-window
WebGL remains at its roughly 59 FPS refresh ceiling. HEVC decode improves
modestly because CPU work is only part of the decode pipeline. The later
integrated scrolling rate remains around 29–31 FPS.

The serial display-layout marker moves from 22.77–22.88 seconds after the
EEPROM banner to 22.04–22.14 seconds. This is not first desktop paint. The
EEPROM timeout improvement is separate and is not encoded in the SD image.

A fresh-boot endurance run passes 173 graphics/Vulkan rounds over 3,659
seconds, alongside three randomized compression workers completing 31,033
checks. A persistent HEVC decoder produces 39,300 frames, with every 60-frame
round matching the known FNV hash. Peak sampled temperature is 73.489 C;
all firmware throttle flags are zero. Swap remains unused.

This run deliberately holds one GPU descriptor open and reuses the decoder's
buffers. It validates the supported clock under sustained CPU/GPU/playback
load, not repeated full GPU teardown or HEVC allocation. The final image's
GPU lifetime test and subsequent endurance run are recorded separately.

The earlier repeated-allocation stress run is a failure, not a passed soak:
with three bounded Zstandard workers, round 109 fails to allocate a 1,020-page
HEVC picture after about 36 minutes. Subsequent GPU initialization can also
fail to allocate its 1,024-page MMU table and select software rendering. The
machine stays responsive, and memory is free after workers exit; the issue
is the need for large physically contiguous runs. The persistent decoder
run tests playback stability separately from repeated allocation behavior.

Evidence: `arm-boost/clock{1500,1800}-run{1,2}.txt`, `abba-verification.txt`,
`abba-serial-markers.json`, `soak-bounded-native.txt`,
`soak-persistent-fragmented-pilot.txt`, `soak-persistent-native.txt`, and
`soak-serial.{log,jsonl}`. The previously documented large-dictionary pilot
is another failed run, not part of the endurance pass.

## GPU hardware lifetime

The fixed V3D device now initializes its shared hardware state when its FDT
node is registered and retains that driver node. It keeps the 4 MiB MMU page
table, scratch page, register mappings and sleeping executor between clients.
The previous last-close path freed the table, forcing the next application
to find another contiguous 4 MiB allocation. Under the fragmentation stress
that allocation failed despite several gigabytes of free memory.

Buffers and synchronization objects still belong to each client and are
released by the file cleanup path. Initialization failure unwinds only the
resources that were successfully created. This change reserves about 4 MiB
of shared GPU state for the machine's lifetime; it does not retain client
rendering allocations or fix the HEVC driver's separate contiguous-buffer
constraint.
The shared hardware remains initialized between clients; idle power was not
compared in this work.

On the old driver, the lifetime probe fails after the last close because
no page table remains. Over 100 open/ioctl/close cycles its median is
9.281 ms. The retained driver measures 0.070 ms, and a 1,000-round client
allocation/mapping/close test retains the same table with zero client buffers
remaining. These are device-open costs, not whole-application launch times.

The native candidate passes 48 GLES readback checks, ten full-HD texture
rounds, a full-HD depth scene, exact Vulkan clear/triangle checks, and twenty
additional cached-buffer probe processes. Another 100 lifetime rounds still
retain the original table and release all client buffers. Firmware throttle
flags remain zero. The loaded kernel image path confirms the staged candidate.

Two initial test harness runs stopped because diagnostic executables were
at different paths in the old installation; their output is retained. The
complete corrected run passes. One control cold boot also missed Wi-Fi
autojoin; the subsequent candidate cold boot joins successfully. The old
installation still contained the experimental scan-listener helper, so this
is recorded as a pre-flash failure without attributing a cause.

The full image also passes the QEMU desktop smoke check. QEMU does not
emulate V3D, so GPU claims rely on the native results above. Evidence:
`v3d-resident/control-opens.txt`, `control-lifetime-expected-failure.txt`,
`candidate-opens.txt`, `candidate-lifetime.txt`, `candidate-pixels.txt`,
`full-build-2.log` and `qemu-2.{log,png}`.

## Final flashed image, 2026-10-06

The complete image built from clean source `63f5e1e916c41369d9464a062f20a23912fe8ee6`
is 4,030,726,144 bytes, SHA-256
`dc071043870ab5e704c5b6cc76a5db8f9f307a49849a70a0f786f1c719fe2d32`.
It reaches the branded desktop in QEMU, was written to the lab SD card,
and the full readback matches before boot. The installed kernel reports
`hrev60097+672`. FAT configuration, loader and boot archive, packaged core
libraries, WebKit, Mesa, V3D and the Wi-Fi helper match the build manifest.
No production V3D override remains. Later documentation commits do not
change the image. Evidence below is under `release/`.

### Boot and application launch

The first observed fresh HDMI desktop is 34.407 seconds after the EEPROM
banner. MJPEG stream gaps cover the display reset; this is an observed upper
bound, not exact first paint. Serial display layout appears at 22.493 seconds
and Ethernet link at 25.982 seconds. The EEPROM USB timeout reduction saves
about 22.5 seconds before SD selection in its separate controlled comparison;
that EEPROM setting is on this board, not in the image file.

All three final launch runs leave the WebKit library unread until after
the first browser process. Boot 1 starts with an empty persistent shader
cache; boots 2 and 3 retain it across cold power cycles. The local start page,
application versions and dual-display layout stay fixed. Boot 3 follows a
Wi-Fi startup failure and unsuccessful manual retry; Ethernet remains up.

| Measurement | Fresh image / boot 1 | Cold boot 2 | Cold boot 3 |
| --- | ---: | ---: | ---: |
| Summit ready, seconds | 2.557 | 2.593 | 2.544 |
| First content tiles, seconds | 8.636 | 8.388 | 8.422 |
| Warm StyledEdit window, ms | 102.910 | 103.680 | 120.151 |
| Warm About window, ms | 107.823 | 107.445 | 107.658 |
| Warm AirPins window, ms | 279.176 | 280.831 | 276.669 |
| Warm Natter window, ms | 335.662 | 335.272 | 329.523 |
| DOM fixture median, ms | 1307 | 1296 | 1243 |
| Full-window WebGL fixture, FPS | 58.37 | 59.00 | 59.08 |
| Active scrolling, FPS range | 30.24–31.68 | 30.68–31.68 | 30.13–31.58 |

Window values are medians of launches two through five. StyledEdit's third
boot has two slower samples (132.5 and 147.2 ms); these are retained, not
discarded. The initial warm medians were 142.9 ms for StyledEdit, 134.1 ms
for About and 1099.2 ms for AirPins. Those initial observations and the final
combination show the overall change; the earlier isolated comparisons
establish which changes helped. Each final boot passes the 12-case WebKit
fixture. No final scrolling result reproduces the earlier isolated 39 FPS
result, so the release measurement remains approximately 30–32 FPS.

Evidence: `boot-1-observed.json`, `boot-1-video/`,
`benchmark-boot-{1,2,3}.{txt,json}`, `hid-boot-{1,2,3}.txt` and
`gpu-after-boot-{2,3}.txt`. All three HID checks deliver 20 keyboard,
20 relative-mouse and 20 absolute-tablet events. The native 1,000-round
GPU lifetime test and later 100-round checks retain the shared page table
with zero client buffers remaining.

### Browser and media workloads

The WebGL Aquarium source is pinned at
`425fa919b1abf5ba0824bcffa6d711ce6c6f3d9e`. Both local variants keep 500 fish,
a 1024x1024 canvas and the same 1125x688 window, with automatic quality
increase disabled. After 40 seconds of warm-up, 20 samples give 36 FPS
median (33–37) without normal maps/reflections and 32 FPS (30–33) with both.
The older 21 FPS screenshot did not control those settings and is not a
valid percentage comparison. The final scene is visually checked.

This WebKit revision hardcodes the JavaScript debug-renderer string to
`Apple GPU` in `WebGLRenderingContextBase.cpp`; that string is not hardware
identification. Native EGL and pixel probes identify V3D. Evidence:
`aquarium-check.txt`, `aquarium-summary.json` and `aquarium.jpg`.

| airTime fixture | Shown FPS | Dropped during measurement | Actual interval |
| --- | ---: | ---: | ---: |
| H.264 1080p30 | 29.6 | 9 | 20.3 s |
| HEVC 1080p30, 8-bit | 30.1 | 0 | 14.1 s |
| HEVC 1080p30, 10-bit | 30.1 | 0 | 15.8 s |
| HEVC 4K30, 8-bit | 30.0 | 0 | 20.4 s |

The short 1080p HEVC clips end before the requested 20-second interval;
the table reports their actual samples. Hardware decoder selection is
confirmed. H.264 dropped another five frames before its measurement began.
Across the three benchmark boots, raw 4K10 decoding remains 18.4–19.6 FPS,
below real time. These short playback tests do not establish performance
on all content or prove physical audio output. Evidence: `airtime-*.txt`
and the visually checked `airtime-playback.jpg`.

### Correctness and device coverage

The installed libraries pass 737,600 strlen cases, 6,422,376 strcmp cases,
549,027 memmove cases, 497,300 memset guards and 8,192 graphics-buffer cases.
All 50 loader cases, four view checks, 48 GLES readback checks, full-HD
texture/depth checks, native EGL window pixel/resize/retirement checks,
exact Vulkan clear/triangle checks, 1,506 SAND conversion cases, 512 TCP
rekeys and 320 failed-child cleanup cases pass (`correctness.txt`).

All 25 HEVC streams match their FFmpeg references. The H.264 harness first
omitted required width/height arguments, then used a stale expected digest;
both failures are retained. A fresh host FFmpeg decode and both native input
modes agree on 90 frames with MD5 `2ceb043d50df9e2292ac39efaeac639b`.
The corrected fixture bundle's 479-file manifest passes. Evidence:
`decoder-check.txt`, `h264-host-reference.txt`, `h264-corrected-reference.txt`
and `bundle-final-check.txt`.

airShot, Amp, Burrow, Kiri, Natter, TurboChook and AirPins pass repeated launch
checks; Clipper's history window opens; LCDMonitor renders a checked JPEG.
The custom Screen, Wi-Fi and Bluetooth preferences open and are visually
checked. BLE scanning finds 18 nearby devices. The audio API accepts a
three-second 48 kHz tone, but nobody has heard the physical output. The
second HDMI display is logically present at 1920x1080; only HDMI0 is observed.

With the other physical network interface administratively down, a 32 MiB
download to SD matches its SHA-256 over each interface. Three downloads to
`/dev/null` measure approximately 72–74 Mbit/s over Wi-Fi and 639–746 Mbit/s
over Ethernet; the server verifies the source address. Both interfaces are
restored afterward. Wi-Fi is joined and remembered through the custom GUI.
It autojoins on boot 2 but times out on boot 3, including two manual recovery
attempts. This is a production-image failure, not evidence that the earlier
experimental helper caused it. See `WIFI.md`.

Linux recovery and air/OS enumerate the Verbatim thumb drive and NanoKVM.
The first 8 MiB of the thumb drive hash identically on both, without any
write. The separate physical mouse appears in neither enumeration; KVM
input passes. Evidence: `apps-check.txt`, preferences screenshots,
`network-{wifi,ethernet}.txt`, `wifi-ui-joined.jpg`, and `usb-*.txt`.

### Final-image endurance

The installed production build passes 168 combined GLES/Vulkan rounds over
3,659 seconds, alongside three Zstandard workers completing 10,775, 10,395
and 10,510 randomized checks (31,680 total). A persistent 1080p10 HEVC
decoder completes 645 rounds / 38,700 frames over 3,661.079 seconds;
every 60-frame round has the expected FNV hash `f4b657343670d5f6`. All workers
exit successfully. This is a combined-load correctness test, not a claim of
30 FPS playback while three CPU stress workers and graphics probes run.

Peak sampled temperature is 74.950 C. Every sampled firmware throttle value
is zero. Swap is entirely free at every progress sample and after the run.
The final free-memory reading is 3,806,687,232 bytes. The serial interval
contains no kernel panic, GPU timeout or HEVC allocation failure.

Unlike the earlier clock test, this harness holds no extra GPU descriptor:
each graphics process closes its contexts normally, leaving hardware
retention to the packaged driver. A further 100 lifetime rounds after the
test retain the original page table and leave zero client buffers. The
decoder deliberately reuses its picture allocations. This does not fix or
retest the separate repeated-HEVC-allocation failure under fragmentation.

Firmware telemetry uses a temporary lab-only `rpi_property` module, which
is intentionally excluded from the image. No production driver or library
override is used. Its cleanup and the final clean boot are recorded in
`STATUS.md`. Evidence: `final-soak-summary.json`, `final-soak-complete.txt`,
`final-decoder-complete.txt`, `final-soak-cpu-{7,101,1009}.txt`,
`final-soak-memory.txt`, `soak-progress-*.txt` and `serial-{1,2}.{log,jsonl}`.

After the load ends, another five launches per app give warm-window medians
of 105.769 ms for StyledEdit, 278.711 ms for AirPins and 332.284 ms for Natter.
The GPU again retains its original table with zero client buffers after
100 further lifetime rounds (`post-soak-apps.{txt,json}`).

Clicking Cancel on the Wi-Fi error dialog releases the queued requests;
the saved network is associated with DHCP by the second five-second status
sample, without changing credentials or issuing another join. The subsequent
clean cold boot nevertheless reproduces the startup failure. This confirms
the dialog recovery obstacle and leaves the initial handshake timeout open.
