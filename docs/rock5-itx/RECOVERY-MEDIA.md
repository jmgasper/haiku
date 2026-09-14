# Protecting recovery media during a USB image switch

Recovery now presents its Linux boot image read-only. The test guest may still
issue filesystem writes after the USB backing file changes and before reset
takes effect. Writable recovery media lets those requests alter a different
filesystem at the old sector addresses.

This failure appeared after the `+168` four-boot cached-packet-DMA comparison.
All 48 network streams and the Haiku-side checks completed, but Linux recovery
printed two `Segment violation` messages during initramfs startup. Its usual
eMMC and Ethernet module initialization was absent, and the root UUID was not
found. The automatic reset/power fallback and a subsequent captured reset did
not restore Linux. This iteration remains a failed automatic recovery.

The original 96 MiB recovery image had SHA-256
`211cf1c02a2bb2c23c368f5cf5d849b8ecc6d59886961f50d5a9887c71bf6d86`.
The altered copy had SHA-256
`8d2f014a7e2b476e434fe4b634a756149e9e87bffec59f04acb56447f031caa5`.
Comparison found 12,288 bytes of changed sectors. The initrd differed while
the EFI launcher, Linux kernel, device tree and boot options still matched.
At byte 37,749,248, the changed image contains the `Haiku` BFS superblock,
with valid BFS magic and 2,048-byte block geometry. That is exactly 512 bytes
after the test image's BFS partition start. The modification time was
07:59:26 UTC, during recovery media selection on 2026-09-14.

These observations strongly support late guest filesystem writes reaching
the replacement LUN. Individual USB write requests were not captured. This
recovery-media corruption is separate from the earlier cached-DMA attempt's
USB transaction-error storm, whose cause remains unresolved.

The altered image was archived intact. A fresh copy of the original image,
verified before deployment and attached with `ro=1`, restored Linux on reset.
Its complete hash remained unchanged after boot. Linux independently verified
the eMMC FAT partition, both fixture files, filesystem consistency and all
three reference regions. The SSD remained on `+156`.

The controller change makes `lab.recover()` call `attach(..., readonly=True)`.
A host regression injects old-guest writes during selection and immediately
before reset; the old implementation changes the recovery contents, while
the corrected path preserves them. All 116 host checks pass. A further native
Haiku-to-Linux recovery cycle is required to qualify the corrected transition.

Evidence beneath `/mnt/HaikuWork`:

- Original failed iteration: `artifacts/automated-cached-packet-dma/20260914T073934Z-fbbb69`.
- Subsequent failed reset: `artifacts/cached-dma-reset-recovery/20260914T081010Z-f07648`.
- Altered image, per-file comparisons and BFS metadata review:
  `artifacts/recovery-image-integrity/20260914T081542Z-10ae87`.
- Fresh read-only deployment, Linux boot and post-boot image hash:
  `artifacts/readonly-recovery-restore/20260914T081743Z-a85108`.
- Independent Linux readbacks: `artifacts/emmc-file-readback/20260914T082005Z-ea396f`
  and `artifacts/emmc-read-reference/20260914T082005Z-0f3f45`.
