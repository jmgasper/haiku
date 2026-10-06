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
Ethernet currently uses `192.168.1.214`; recovery's recorded address is
`192.168.1.209`. Check DHCP rather than assuming either is permanent.

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
