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
