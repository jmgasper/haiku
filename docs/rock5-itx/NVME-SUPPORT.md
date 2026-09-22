# NVMe and TRIM on the replacement Rock 5 ITX SSD

The 256 GB SPCC M.2 PCIe SSD (`1f99:6100`) is the default Haiku boot drive.
The existing NVMe driver exposes its 512-byte namespace as
`/dev/disk/nvme/0/1`. BFS and `fstrim` support free-space TRIM. The full board
image now opts into the existing, firmware-checked GICv3 ITS1 profile, so
segment-zero NVMe uses MSI-X rather than the polling fallback. The PCIe and
ITS profiles are tied to the captured EDK2 v1.1 firmware and requester ID.

## Verification on 2026-09-22

* Before enabling ITS, the installed NVMe BFS volume reported 58,215,762 free
  4 KiB blocks. `fstrim -f -v /boot` completed 238,451,761,152 bytes,
  exactly that free-block count times 4096. The installed Haiku package still
  matched SHA-256 `85d064cdb0c145a8667a67ab8dcf8d02041c2fd9ceaeefff734dc7e5b858ca2f`.
  `checkfs -c /boot` found zero allocation errors. See
  `artifacts/nvme-support-20260922/trim-b.jpg`, `post-trim-b.jpg`, and
  `checkfs-b.jpg`.
* The full `@rock5full-mmc` image built with SHA-256
  `cd8c8c41f67bfc405739ea48d2ce20dbcab0d856f364aa2003b300b2539a736c`.
  QEMU reached the Welcome screen; the result is
  `artifacts/qemu/20260922T125245Z-d62627/result.json`. The NVMe trim-range
  and PCIe profile checks passed.
* The matching ITS setting was installed on the NVMe system. The subsequent
  boot logged ITS1 initialization, DeviceID `0x100`, vector 8192, MSI-X
  selection, delivered interrupts, and `/dev/disk/nvme/0/1` as the boot
  partition. See `artifacts/nvme-support-20260922/msi-native-serial.log`.
  The installed kernel still prints “Samsung DeviceID” in its generic
  segment-zero ITS startup line; the compiled full image changes this label
  to “NVMe DeviceID”. The attached controller was identified as the SPCC SSD.
* In MSI-X mode, a 16 MiB file written from `/dev/urandom` retained SHA-256
  `cf2b93a958ffb938b96139c92d98d98d9760a7c44e12714b88512249487a7849`
  across `sync` and another filesystem TRIM of 238,434,852,864 bytes.
  `checkfs -c /boot` checked 423 nodes with zero allocation errors. See
  `artifacts/nvme-support-20260922/msi-trim-b.jpg` and `msi-checkfs-b.jpg`.
* The next warm restart paused at the EDK2 splash before Haiku started. A
  NanoKVM reset then booted the default NVMe partition again with MSI-X.
  The 16 MiB file retained the same hash after this restart; see
  `artifacts/nvme-support-20260922/persistence-b.jpg`. The temporary file was
  removed after verification. The firmware warm-restart pause remains open.

These checks establish basic installed-drive I/O, filesystem TRIM, and MSI-X
delivery. They do not measure sustained throughput, controller-stall recovery,
or sudden power-loss durability.

## Large-read correction

The next cold GLInfo launch on the `+323` installation exposed a real NVMe
failure. A physically contiguous read of 20,472 sectors was handed to a
controller limited to 128 KiB per command; libnvme returned out of memory.
The error path then called `SetStatusAndNotify()` again after its completion
callback had freed the request, and the kernel panicked on the freed lock.
The failure trace is `artifacts/gpu-full-20260922/native-serial.log` and its
NanoKVM screenshot is `glinfo-b.jpg` in that directory. The earlier MSI-X
and TRIM passes did not cover this large-read path.

The driver now routes any physical vector larger than its maximum transfer
through the bounded DMA translation path. `do_io()` owns notification on
every error, and its caller does not inspect a request after completion may
have freed it. The corrected full image has SHA-256
`a2625db407ae5ae06fa2962110b35a38749c77deecb45733beddb6777435030b`;
QEMU reached Welcome in `artifacts/qemu/20260922T130837Z-80edba`.
The rebuilt Haiku package has SHA-256
`751243c7f975aa47e4ecbaa6fa0974ebe873ab71203f80ab7a766b99af17d956`;
the same hash was checked after transfer, and `pkgman` upgraded the NVMe
installation from `+323` to `+326`.

On the first `+326` native NVMe boot, ITS1 and MSI-X attached and GLInfo
reached its Mali-G610 (Panfrost) window. The prior large library read no
longer failed or panicked. Filesystem TRIM completed 238,387,552,256 bytes,
and `checkfs -c /boot` checked 427 nodes with zero allocation errors. The
screen is `artifacts/nvme-large-io-20260922/glinfo-c.jpg`, the TRIM/check
screen is `final-trim-b.jpg`, and the serial capture is `native-serial.log`.
This resolves the reproduced failure; broader stress and fault injection
remain to be tested.
