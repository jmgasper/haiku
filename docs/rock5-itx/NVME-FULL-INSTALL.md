# Full Haiku installation on the Rock 5 ITX NVMe drive

On 2026-09-22 the regular ARM64 build was installed on the replacement
256,060,514,304-byte SPCC M.2 PCIe SSD (PCI ID `1f99:6100`, serial
`TCSJA265JX00002`). This is the default boot drive. The board is Rock 5 ITX
v1.12 with EDK2 v1.1 in DT-only mode.

The existing PCIe firmware profile was tied to the former Samsung 950 Pro. The
segment-zero profile now recognizes the installed SPCC controller while still
accepting the captured Samsung ID. The endpoint ID, class, BAR, root window,
function and firmware resources remain checked before exposing the device.
`rk3588_pcie` selects the NVMe profile. The full-image artifact helper uses the
matching full-build record.

## Installation and verification

* Full image: `artifacts/images/haiku-arm64-ab9de23857fd1c9d.img`, SHA-256
  `ab9de23857fd1c9d5e7c5c926a3c7ee4bb51016c0dcef33ae9b6e17f03951124`
  (6,480,199,680 bytes). The QEMU smoke result is
  `artifacts/qemu/20260922T110037Z-c7f320/result.json`; its screen reached
  the Welcome app with no serial panic.
* Before using the eMMC as the installation source, its complete
  7,818,182,656-byte contents were backed up to
  `artifacts/emmc-before-nvme-install.img`. Local and independent device hashes
  matched: `9962cdc39ffe8b2c4885ec03d92b0ee8d05752189bb5c8a898d8e24905409b0e`.
  The recovery SD and NanoKVM recovery USB were retained.
* DriveSetup created GPT with a 512 MiB FAT32 EFI system partition and a
  237.97 GiB BFS partition. Installer copied the full system from eMMC to
  `Haiku NVMe`. All 21 source `.hpkg` files matched the target by SHA-256,
  including Amp, Kiri, Turbo Chook and GLInfo. The EFI loader on both drives
  matched SHA-256 `4e5dfb496e4c10483de7109a3b89fe39fa0c44b8da8aded6038621ecee811c31`.
* Native `checkfs -c` on the mounted NVMe BFS partition checked 400 nodes with
  zero allocation errors. Debian recovery independently verified the device
  identity, GPT (`sgdisk -v` found no problems), partition sizes and EFI loader.
  Evidence is in `artifacts/nvme-install-20260922/`.
* The one-time `BootNext` trial and a subsequent normal boot both reached the
  desktop with `Mounted boot partition: /dev/disk/nvme/0/1` and
  `bfs: mounted "Haiku NVMe"` in the serial log. There was no `BootNext` on the
  second boot. UEFI `Boot0010` is `Haiku NVMe`; BootOrder starts
  `0010,0004,0005,0003`, retaining NanoKVM USB, eMMC and SD alternatives.
  The default-boot desktop screenshot is
  `artifacts/nvme-install-20260922/default-boot-2.jpg`.

At installation the NVMe driver reported polling because its MSI provider
was unavailable in that boot. The later [NVMe support update](NVME-SUPPORT.md)
enabled ITS1 MSI-X and verified native filesystem TRIM on this drive.
