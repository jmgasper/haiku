# PCIe startup training observation

The first updated `+148` SSD boot missed SATA attachment after rejecting the
root profile. A diagnostic-only `+154` image reproduced that failure on USB,
without changing admission checks, firmware settings or the SSD installation.

The rejected segment-1 snapshot was:

```
id=35881d87 command=00100007 class=06040001 header=00010000
buses=00010100 memory=f100f100 capability=1042b010 link=b8230000
```

Link Status is the upper halfword, `0xb823`. Its data-link-active bit is set,
negotiated width is x2 and speed encoding is 3 (8 GT/s), but Link Training
(`0x0800`) is also set. The identifiers, header, bus numbering, memory decode,
capability and forwarded memory window pass the current profile. A sanitizer
test against the production `RootMatches` header rejects this captured set of
fields and accepts it after clearing only the training bit in a local fixture.
That fixture does not write any hardware register. See the
[Linux v6.12 PCI register definitions](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/pci_regs.h)
for the status fields and
[Linux's bounded link-status polling](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/pci/pci.c)
for a primary implementation reference.

The first USB boot later published the segment-1 PCI host, showing that its
strict root check had passed by then. AHCI never attached during that boot.
After one normal Haiku reboot, the same image initialized IRQ 287 and direct
ports 0..3. This is a failure/pass comparison with no intervening driver or
setting changes. The late PCI appearance does not turn the first attachment
failure into a pass.

Both boots passed 25 component and five setting hashes, a 64 MiB/eight-worker
memory check, 51,301 copy cases, native USB-root verification, and DHCP with
2.5/1 Gbit/s Ethernet links. Tracker/Deskbar desktops were inspected. Read-only
eMMC file and three raw-reference hashes passed before and after reboot.
Recovery returned to Linux without serial errors or a controller restart;
the NanoKVM watchdog was disarmed. Independent Linux checks confirmed the
entire 300 MiB eMMC FAT partition, both fixture files, filesystem integrity
and all three reference regions unchanged.

The candidate was built from `a5f9d4173c23289d484f7cdaf08548ad76e5244f`
(`hrev60097+154`). Image SHA-256 is
`4076e5f91ca66274029f3350b53a08a23068ac5bd306c55da4640d08005eb570`.
All 110 host checks and the full ARM64/QEMU regression passed before native
deployment. Among the 25 pinned components, only the PCI host contains changed
code; the kernel and libroot differ only in their revision sections, with
identical `.text`. All five settings match the `+148` baseline.

Local evidence beneath `/mnt/HaikuWork`:

- Candidate, component comparison, references and final observation:
  `artifacts/pcie-profile-diagnostics-image/20260913T221820Z-da96de`.
- Native two-boot serial, shell transcripts and frames 010/068:
  `artifacts/interactive/20260913T222518Z-d0c6df`.
- Guard: `artifacts/controller-guarded-native/20260913T222518Z-361419`.
- QEMU: `artifacts/qemu-shell/20260913T221943Z-c0d03c`.
- Linux FAT readback: `artifacts/emmc-file-readback/20260913T224148Z-8a2889`.
- Linux references: `artifacts/emmc-read-reference/20260913T224148Z-ce0042`.
- Summary: `state/native-pcie-profile-diagnostics.json`.

Next is a bounded, passive wait for the observed training state, retaining
the complete profile and inactive-link rejection before downstream config
access. The current diagnostic has no retry implementation. A changed driver
needs fault tests, a build/QEMU pass and new native boot evidence. Native SATA
disk I/O remains untested because no SATA disk is attached.
