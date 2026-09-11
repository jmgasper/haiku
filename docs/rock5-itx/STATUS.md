# Tested status

Updated 2026-09-11. This page distinguishes lab readiness from native Haiku
support. No hardware row in the roadmap is accepted merely because Linux or
firmware supports it.

| Area | Evidence and state |
| --- | --- |
| Repository | `jmgasper/haiku`, `rock5-itx` branch; upstream base `855b5d0e3126c86acc84d09f8e859272019bbbc2` |
| Build tools | Haiku GCC 13.3.0 cross-compiler and binutils built successfully; buildtools `8375c2dbeaf109c520798cb234d57f0895463201` |
| ARM64 image and QEMU | `@minimum-mmc` built successfully; 636 MiB raw disk booted to a visible Tracker/Deskbar desktop in QEMU ARM64, 4 virtual CPUs and 2 GiB RAM |
| NanoKVM | PCIe model, application 2.4.3 and base image v1.4.0; SSH and authenticated API tested |
| Remote controls | HDMI capture, keyboard, reset, full off/on and controller availability through target power-off tested |
| Virtual storage | Raw USB image verified byte-for-byte from ROOBI; selected image survives reset and target power cycle |
| Automated controls | Integrity, path isolation, locking and recovery-on-error regression checks pass; full image cycle validation in progress |
| Recovery OS | ROOBI / Debian 11, kernel `5.10.110-33-rockchip`; SSH works independently of virtual media |
| Boot firmware | Vendor U-Boot 2017.09; no EFI handoff verified and no firmware replacement performed |
| Native Haiku on ROCK | Not yet booted; firmware handoff and UART are the next gates |
| Serial | Cable ordered; no board serial connection verified |
| Power LED readback | Reports false while running; use boot IDs, video and actual reachability |

First QEMU desktop capture:
`/mnt/HaikuWork/artifacts/qemu/20260911T045107Z-071bbd/screen.png`.
The serial log records successful framebuffer initialization and first-login
processing. The minimum image lacks some optional libraries: screen-saver and
shortcut input filters and `desklink` report missing dependencies. These do
not prevent this basic desktop boot; broader ARM64 package completeness remains
part of phase 11. No GPU acceleration or networking was tested in QEMU.

Inventory observed from this unit: RK3588, 16 GiB RAM, approximately 7.3 GiB
onboard eMMC, 16 MiB SPI loader device, two `10ec:8125` rev 05 Ethernet devices,
ASM1164 `1b21:1164` SATA controller, ES8316 audio and HYM8563 RTC. Linux's running
device tree also identifies RK806/RK8602/RK8603 power devices, FUSB302 Type-C
control and PWM fan. Identification does not establish Haiku driver support.

No NVMe/SATA disk or Wi-Fi/Bluetooth module has been identified as installed.
Additional storage, network peers, audio loopback/receivers, displays and camera
fixtures are needed for the corresponding acceptance tests. Exact board PCB
revision still needs confirmation.

Local evidence and credentials are not published. Initial evaluation lives at
`/mnt/HaikuWork/nanokvm/EVALUATION.md`; raw inventory, screenshots and running
device tree are in its `evidence/` directory. New runs go to
`/mnt/HaikuWork/artifacts`. SPI and partial eMMC snapshots are not a validated
recovery image or complete backup.
