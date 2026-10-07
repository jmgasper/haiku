# air/OS on Raspberry Pi 4: current status

The lab board is a Raspberry Pi 4 Model B revision 1.5, 4 GB (`c03115`),
with two HDMI displays, Ethernet, the NanoKVM's USB HID devices,
and a USB thumb drive. The fresh-install investigation on 2026-10-07 uses
a 128 GB SD card; the earlier performance run below used 8 GB. Current
reliability work is on `rpi-firstboot-reliability`, based on `master`.
Earlier bring-up notes are retained in `HISTORY-20261005.md`;
this page supersedes their older status statements.

## Fresh-install reliability investigation (2026-10-07)

The reported unbootable card has an intact FAT partition and a correctly
expanded 127,727,042,560-byte BFS volume. An allocated-data backup and its
extent checksums are preserved before changes. `bfs_shell checkfs` found
496 nodes and no missing, duplicate or reclaimable blocks. Its original
system boots when the NanoKVM presents a valid nonbootable USB disk instead
of an empty mass-storage LUN. This establishes a lab boot-device problem;
the observed card does not show corruption from filesystem expansion.

The serial cable is working: a known Linux UART message was received intact,
followed by a complete native AirOS boot log. `serial-capture.sh` previously
used a `pkill -f` pattern that could kill its own SSH shell before setting
the baud rate. It now uses a bounded reader and the shared serial lock.

The HTTPS failure also has a concrete certificate-store cause: the original
image has no `CARootCertificates.pem` at either curl's canonical path or the
legacy Summit path. Installing the official `ca_root_certificates` package
and the compatibility symlink restores certificate-validated HTTPS. The old
arm64 package reader cannot load the repository's zstd-compressed bundle;
CI now stages a zlib copy. A correct clock alone did not fix this image.

The candidate time/onboarding and USB HID lifetime changes build and boot in
QEMU. Both storage choices and the Hobart timezone persist across a guest
restart. The expanded virtual card passes an offline BFS check (434 nodes,
no allocation errors). Choosing a zone preserves UTC; a failed actual
`Time --update` returns status 1. Twenty keyboard and twenty tablet
attach/remove cycles completed with working input afterward. A networked
candidate synchronizes time automatically and Summit opens HTTPS.

The native SD now runs the candidate packages (`hrev60206+731+dirty`) and
matching loader/archive, installed in place with the old files retained for
rollback. It reaches the timezone step, NTP reports synchronization success,
and both curl and Summit load HTTPS with certificate validation enabled.
Seven native USB gadget disconnect/reconnect cycles complete without a panic.
Keyboard input works after restarting NanoKVM's HID service, whose stale Linux
handles otherwise produce `ENXIO` before reports can reach the Pi. The KVM's
RNDIS interface times out on reinitialization; this is not a successful USB
network hotplug qualification. Ethernet remains available.

HDMI now polls the BCM2711 HPD register, debounces edges, reads EDID using
firmware display IDs 2 and 7, restores a supported EDID timing through firmware
KMS, and notifies app_server through its existing display-change port. A locked
state snapshot prevents accelerants reading a partly updated EDID. Both physical
connectors retain stable IDs even if only one was present at boot. Three native
HDMI0 disconnect/reconnect cycles shrink the desktop to HDMI1 and restore the
3840x1080 layout with a visible KVM picture. A separate boot starts with HDMI0 absent;
with both firmware pipelines initialized in `config.txt`, a later connection
selects 1080p and produces a live picture. Without pipeline initialization it
remains black. HDMI1's physical reconnect and picture still need observation. Firmware EDID responses alone are unsuitable
for detection: they remain cached after HPD goes low.

Evidence: `/mnt/HaikuWork/rpi4/evidence/fresh-install-20261007`, including
`sd-allocated-manifest.json`, `bfs-check.log`, `no-boot-media-serial.log`,
`qemu/expanded-bfs-check.log`, `qemu-final/hotplug.log`,
`native-verify.log`, `native-summit-visible.jpg`, `native-hdmi-hotplug.log`,
`native-final-late-hdmi.log`, `native-final-late-hdmi.jpg` and
`native-usb-fixture-reset.log`. RPiInstaller 1.1.0 is staged in the build
server's ARM64 package pool; its old 1.0.0 package is retained in the task
backup directory. The complete CI image pipeline has not been run here.

## Tested image

Built on 2026-10-06 from clean source commit `63f5e1e916c41369d9464a062f20a23912fe8ee6`:

- Image: `/mnt/HaikuWork/rpi4/build/airos-rpi4.img`, 4,030,726,144 bytes.
- SHA-256: `dc071043870ab5e704c5b6cc76a5db8f9f307a49849a70a0f786f1c719fe2d32`.
- Emulator: the complete image reaches the branded desktop.
- Flashed to the lab SD card on 2026-10-06; a full 4,030,726,144-byte
  readback matched that checksum before boot. The installed kernel is
  `hrev60097+672`. FAT config, loader, boot archive, packaged libraries, Mesa,
  WebKit, V3D and Wi-Fi helper match the recorded build; UTC time is correct.

The checksum sidecar is `airos-rpi4.img.sha256`. The complete package/library
manifest and raw acceptance results are in
`/mnt/HaikuWork/rpi4/evidence/performance-20261006/release`.


The image contains the full branded air/OS desktop, custom Screen, Wi-Fi
and Bluetooth preferences/applets, Summit, airTime, and the other available
arm64 application packages. The lab build includes its private remote-test
overlay. That overlay is controlled by `/mnt/HaikuWork/rpi4/state/lab-image`;
it is not part of the generic `rpi4-airos` profile's application set.

Build instructions are in `BOOT.md`; the performance work, controlled
comparisons and rejected experiments are in `PERFORMANCE.md`. Firmware,
Mesa and Summit builds are pinned by the scripts in `tools/rpi4`.

## Hardware and applications

| Stage | Current result | Remaining limit |
| --- | --- | --- |
| SD and boot | Full air/OS boots from SD; four cores; high-speed SD with DMA requests up to 512 KiB; USB recovery retained | Only the 4 GB rev 1.5 board has been tested; no SD hot plug |
| Ethernet | GENET at 1000 Mbit/s full duplex, DHCP and data transfer | One queue and packet copies; no checksum offload |
| USB | VL805 storage and HID work after the descriptor-overfetch fix | 8 GB board DMA above 4 GB untested; no webcam connected |
| Graphics | Native OpenGL/GLES and Summit WebGL use V3D; pixel-exact GLES and Vulkan probes | Vulkan is headless: no arm64 loader or window-system presentation |
| Displays | Both outputs advertise 1920x1080; the desktop is 3840x1080; custom Screen preferences arrange, mirror and scale outputs | HDMI1's physical picture has not been observed; firmware scaling; HDMI0 hotplug tested; HDMI1 physical reconnect, DPMS and hardware cursor unqualified |
| Wi-Fi | CYW43455 scans and joins WPA2 through the custom preferences window, gets DHCP, and remembers the network; final independent traffic and reboot results below | Intermittent startup association timeout remains: Cold boots 3 and 4 fail to autojoin; dismissing the blocked supplicant dialog permits recovery; roaming and wider AP compatibility need more coverage |
| Bluetooth/BLE | Controller, custom preferences and nearby-device scanning work; earlier LE connection/service discovery passed | Pairing and application profiles untested; UART at 115200 |
| Media/airTime | Hardware H.264 and HEVC Main/Main10 decode; 25 HEVC fixtures and H.264 match FFmpeg; 1080p HEVC plays at 30 FPS | H.264 memory contention drops occasional frames; 4K10 remains below 30 FPS; sound has not been listened to physically |
| Summit | GPU compositing, WebGL, JIT and the WebRTC-enabled engine are included; local browser regression fixture passes | Full-window scrolling still has substantial readback and app_server copying costs |
| Other apps | airShot, Amp, Burrow, Kiri, LCDMonitor, Natter, TurboChook, Clipper and AirPins are included | Aurora needs an arm64 Haiku Node.js port; launch checks are not comprehensive app testing |

The installed image passes the correctness, launch, playback and device checks
below. Four cold boots deliver all 20 keyboard, 20 relative-mouse and 20
absolute-tablet events from the KVM. Wi-Fi autojoins on boot 2 but fails on
boots 3 and 4. On boot 3, dismissing the supplicant's blocked password-error
dialog releases the queued retries and restores the link. On boot 4,
dismissing it and running the normal helper reconnects with the saved
credentials. Ethernet remains available throughout. Startup reliability is
still a release blocker; the generic password-error dialog does not prove
that the password is wrong.

The final combined-load run passes 168 graphics/Vulkan rounds over 3,659
seconds, 38,700 checksum-verified HEVC frames, and 31,680 randomized
compression checks. Peak sampled temperature is 74.95 C, all sampled
firmware throttle flags are zero, and swap remains unused. This run holds
no extra GPU descriptor; the packaged driver retains its own hardware state.
The decoder reuses picture buffers, so the separate repeated-allocation
failure remains open.

The temporary telemetry module is removed from the driver directories,
then a clean cold boot verifies that its device node is absent. Production
library/driver hashes still match the image, UTC is correct, Ethernet and
USB are up, both logical displays retain their layout, and the GPU lifetime
probe leaves zero client buffers. The device is left running with Ethernet
and the saved Wi-Fi network connected. Evidence: `release/clean-boot-checks.txt`,
`verify-installed-final.txt`, `hid-boot-4.txt` and `wifi-boot-4-recovery.txt`.
An additional Wi-Fi-only transfer after recovery matches its checksum and
runs at 73–75 Mbit/s, with no ping loss; Ethernet is restored afterward
(`final-network-wifi.txt`, `final-idle-2.txt`).

The graphics and memory suite passes on the installed libraries: 737,600
string-length cases, 6,422,376 string comparisons, 549,027 memory moves,
497,300 memory-set guards plus 8,192 graphics-buffer cases, 50 loader cases,
four view checks, GPU lifetime/pixel checks, and TCP regressions. All 25 HEVC
streams and the 90-frame H.264 fixture match FFmpeg. The H.264 harness's
initial expected digest was stale; a fresh host decode and both native input
modes agree on `2ceb043d50df9e2292ac39efaeac639b`.

All application launch checks pass; LCDMonitor also writes a visually checked
dashboard. BLE scanning finds nearby devices, and the audio API accepts a
48 kHz tone. These are not physical audio or external-LCD observations. Both
Linux recovery and air/OS enumerate the Verbatim thumb drive and NanoKVM;
the separate physical mouse is not present in either enumeration. An 8 MiB
read of the thumb drive matches between Linux and air/OS without any write.

First observed HDMI desktop is 34.4 seconds after the EEPROM banner on the
initial flashed boot. A KVM video gap covers the HDMI reset, so this is an
observed upper bound, not the exact first desktop paint. The serial display
layout marker is at 22.49 seconds and Ethernet link at 25.98 seconds.

The first fresh-image Summit launch, with no pre-launch engine hash read and
an empty persistent shader cache, is ready in 2.56 seconds and sends its
first content tiles in 8.64 seconds. Browser cache and later cold-boot results
are distinguished in `PERFORMANCE.md`. Across three runs, warm window
medians are 103–120 ms for StyledEdit, about 108 ms for About,
277–281 ms for AirPins and 330–336 ms
for Natter. The controlled Aquarium workload measures 32 FPS with normal
maps/reflections and 36 FPS with both disabled; full-window synthetic
WebGL runs at about 59 FPS and scrolling at 30–32 FPS.


## Release limits

Exhausting both RAM and swap can leave the system unresponsive without an
OOM victim being selected. Fragmentation during repeated allocations can
also prevent the HEVC driver from finding a contiguous picture allocation
despite ample free RAM; the test process fails and the board recovers after
memory is released. The sustained bounded-memory
stress test and the memory-pressure failure are reported separately.

The port still needs physical HDMI1 and audio verification, Bluetooth pairing
and profiles, wider board/AP coverage, and Vulkan presentation. No result here
claims those gaps are closed. See `DISPLAY.md`, `MEDIA.md`, `WIFI.md`,
`BLUETOOTH.md`, `GPU.md`, and `PERFORMANCE.md` for implementation details.
