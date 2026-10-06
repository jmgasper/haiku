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
