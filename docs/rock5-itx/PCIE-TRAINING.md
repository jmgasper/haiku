# PCIe startup training

The `+156` USB candidate now handles the captured SATA startup condition on
this board. On a native reboot it observed Link Training, waited 2,587 us
over two polls for that flag to clear, and then initialized AHCI and all four
direct ports. This validates that specific transition; the SSD still runs
`+148`, and other startup failures and native SATA disk I/O remain open.

## Original diagnostic

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

## Bounded wait and native check

Source `26a109b5ef2d7138f4f512abb580e705dbeff423` (`hrev60097+156`)
adds a passive wait of at most one second, polling at one-millisecond intervals
while the otherwise valid root reports an active link and Link Training.
It revalidates the complete root profile on each sample and checks the root
identity before each full snapshot. Invalid configuration, an inactive link,
a failed sleep or timeout prevents downstream configuration access. It does
not request retraining or change PHY, clocks, resets or address translation.
Successful waits record both link values, elapsed time and poll count.

The actual production wait and snapshot routines pass sanitizer tests with a
fake clock and register bank. Cases include all four supported root profiles,
immediate readiness without further reads, training completion, invalid
resources, link loss, changed identity, permanent training, deadline boundaries
and an interrupted wait. All 111 host checks and the complete ARM64/QEMU
regression passed. The candidate image SHA-256 is
`fabc17620f00a19bca06ff7b1c2715d2fbaa24755cb9346d8b475f673e70563b`.
Only the PCI host contains changed executable code relative to `+154`;
kernel/libroot differences are confined to their revision sections, and all
other pinned components and five settings match.

The first trial passed inventory, memory, copy, both Ethernet links and AHCI
attachment on three boots. None encountered training. Its 1,800-second session
deadline returned the board to Linux before the final native eMMC file check
ran. That missing check is retained in `first-session-incomplete.json`; the
trial is not described as fully completed. Independent Linux readback passed.
One collection attempt correctly refused to read a mounted FAT partition:
Linux had automatically mounted `/boot/efi` while idle. Stopping that mount
without changing persistent configuration allowed a clean readback, with the
whole partition still matching its saved hash.

A fresh automated two-boot cycle completed every native check and recovery.
The first boot found the link ready. The second boot logged:

```
segment 1 waiting for firmware link training, link=b8230000
segment 1 firmware link training settled after 2587 us (2 polls), link=b8230000 -> b0230000
```

AHCI then used IRQ 287 and initialized ports 0..3. Both boots passed all 25
component and five setting hashes, memory and copy probes, 2.5/1 Gbit/s DHCP
links, inspected Tracker/Deskbar desktops, and read-only eMMC file/reference
checks. Normal reboot, Linux recovery, clean serial capture and watchdog
disarm passed without a NanoKVM restart. Independent Linux readback again
confirmed the entire 300 MiB FAT partition, both files, filesystem integrity
and all three reference regions unchanged.

The local automatic runner advances through checked shell commands, waits
for the new kernel's shell after reboot, and requests guarded recovery on
completion or error. Its source is retained with the cycle evidence. Desktop
inspection and independent Linux integrity remain explicit qualification gates.

Additional evidence beneath `/mnt/HaikuWork`:

- Candidate, first incomplete session and final qualification:
  `artifacts/pcie-training-image/20260913T224921Z-a4d43d`.
- Completed automatic runner and per-boot checks:
  `artifacts/automated-pcie-training/20260914T021553Z-7be5f9`.
- Completed native serial/transcripts and inspected frames 008/021:
  `artifacts/interactive/20260914T021638Z-a8c1ec`.
- Guard: `artifacts/controller-guarded-native/20260914T021638Z-3f3196`.
- QEMU: `artifacts/qemu-shell/20260913T225022Z-a67e77`.
- Final Linux FAT/reference checks:
  `artifacts/emmc-file-readback/20260914T022401Z-a33256` and
  `artifacts/emmc-read-reference/20260914T022401Z-73a8c0`.
- Summary: `state/native-pcie-training.json`.

The next step is integration into the SSD installation and broader boot/error
coverage. This test does not qualify inactive-link recovery, PHY initialization,
hotplug, SATA disk I/O or the unrelated earlier startup/USB failures.
