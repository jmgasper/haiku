# Raspberry Pi 4 release performance

Work started 2026-10-06 on `rpi4`, from `e762318b83`. Target: the lab's
4 GB Raspberry Pi 4B revision 1.5, four Cortex-A72 cores at 1.5 GHz, the
existing 8 GB SD card, and the full air/OS image. Improvements must retain
the custom applications, preferences and hardware support.

## Measurement

Raw evidence belongs outside Git in
`/mnt/HaikuWork/rpi4/evidence/performance-20261006`. That directory also
preserves the starting image and boot archive. The board initially ran
kernel `hrev60097+639` and Summit packages `1.10.0-10` / `git20261005-2`.
Ethernet and Alpine recovery currently use `192.168.1.214`; the older
recorded recovery address was `.209`. Check DHCP rather than assuming
either is permanent.

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
