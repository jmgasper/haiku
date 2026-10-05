# ARM64 CPU details on the Rock 5 ITX

The ARM64 kernel now captures each processor's `MIDR_EL1` and matches its
`MPIDR_EL1` affinity with the `/cpus` node in the firmware device tree. The
topology reports the ARM vendor and raw model ID for each core. For a CPU with
an `operating-points-v2` table, it reports the highest enabled `opp-hz` as the
available maximum frequency; a `clock-frequency` property is a fallback.
There is no claim that this is a live clock measurement or that frequency
scaling is implemented.

The shared CPU model decoder names ARM Cortex-A55 and Cortex-A76 parts.
“About this system” groups cores with the same model and advertised maximum
frequency. On the Rock 5 ITX it displays four Cortex-A55 cores up to 1.80 GHz
and four Cortex-A76 cores up to 2.40 GHz. Other architectures retain their
previous display path.

## Verification on 2026-09-22

* The full `@rock5full-mmc` image built with SHA-256
  `32d23337e2000c4532220408f2dd0fe07d516d733195cd5882b735b83468b107`.
  QEMU reached the Welcome screen; the smoke result is
  `artifacts/qemu/20260922T120403Z-19bc63/result.json`.
* The updated Haiku package had SHA-256
  `85d064cdb0c145a8667a67ab8dcf8d02041c2fd9ceaeefff734dc7e5b858ca2f`.
  That hash matched after transfer to the NVMe installation. `pkgman install`
  upgraded `haiku` from `hrev60097_322_dirty` to `hrev60097_323_dirty` and
  requested a reboot.
* The updated system mounted `/dev/disk/nvme/0/1`. The native system probe
  printed `8 ARM Cortex-A76, revision 414fd0b0 running at 2400MHz` (its
  legacy single-model summary). The About window displayed both core groups
  and their speeds; see `artifacts/cpu-details-20260922/about-b.jpg` and
  `native-serial.log`.

The first warm restart stopped at the EDK2 splash. A subsequent NanoKVM reset
and power-control sequence led to a successful NVMe boot. This observation
does not establish why that firmware phase stalled; reboot reliability remains
to be checked separately.
