# X399 workstation port

Target: an Asus Prime X399-A workstation running this AI-assisted Haiku fork
natively from its NVMe drive. This work lives on the `x399-workstation` branch
and must not change ARM64/ROCK 5 behaviour (see `docs/rock5-itx`).

## Hardware inventory

Captured with SystemRescue 13.02 (Linux) on 2026-09-17. Raw captures are kept
outside the repository (`/mnt/HaikuWork/x399/evidence/inventory`).

| Function | Device | PCI ID | Haiku driver (upstream) |
| --- | --- | --- | --- |
| Board / firmware | Asus Prime X399-A, AMI UEFI 1601 (2023-04-14) | | |
| CPU | AMD Ryzen Threadripper 1950X, 16C/32T, 1 NUMA node | | x86_64 SMP (max 64) |
| Memory | 4 x 16 GiB DDR4-2133 | | |
| NVMe | Samsung SSD 950 PRO 256GB (bus 41) | 144d:a802 | nvme_disk |
| GPU | NVIDIA GeForce GTX 1080 Ti (GP102, Pascal, bus 42) | 10de:1b06 | framebuffer only |
| GPU audio | GP102 HDMI audio | 10de:10ef | hda |
| Ethernet | Intel I211 | 8086:1539 | ipro1000 |
| Audio | AMD Family 17h HDA, Realtek ALC1220 codec | 1022:1457 | hda |
| USB | X399 chipset xHCI (14 USB2 / 8 USB3 ports) | 1022:43ba | xhci |
| USB | AMD Family 17h xHCI (CPU, bus 0f) | 1022:145c | xhci |
| USB | AMD Family 17h xHCI (CPU, bus 43) | 1022:145c | xhci |
| USB | ASMedia ASM2142 USB 3.1 (NanoKVM attached here) | 1b21:2142 | xhci |
| USB / Thunderbolt | Intel Maple Ridge 4C TB4 add-in card: xHCI + NHI | 8086:1138, 8086:1137 | xhci / none |
| SATA | X399 chipset AHCI, 2 x FCH AHCI (no disks attached) | 1022:43b6, 1022:7901 | ahci |
| Platform | AMD IOMMU, PSP, SMBus, HPET, CRAT/CDIT/IVRS ACPI tables | | |
| Sleep | Linux reports `freeze mem disk`, `mem_sleep` = `deep` (S3) | | none |

## Graphics route

X547's `nvidia-haiku` pairs NVIDIA's open GPU kernel modules with GSP
firmware. That combination only supports Turing and newer GPUs. The GTX
1080 Ti is Pascal, so this port uses the RM core from the proprietary driver
of the same release (570.86.16), which still supports Pascal. The RM core
is combined with X547's Haiku OS interface layer, the open `nvidia-modeset`
sources, NVK on the NVRM interface (Mesa 25.3 lists Maxwell to Ada as
conformant) and Zink for OpenGL.

The proprietary `nv-kernel.o_binary` is non-PIC code for GCC's kernel code
model. Haiku kernel add-ons are position-independent shared objects, so the
kernel ELF loader can also map x86_64 `ET_EXEC` add-ons at their link
addresses above `KERNEL_FIXED_ADD_ON_BASE` (top 2 GiB). NVIDIA binaries
are never committed; they are downloaded at build time.

## Lab control

A NanoKVM PCIe card in the workstation provides HDMI capture, USB keyboard,
virtual USB storage and power control. Its "reset" line turns the machine
off; a 800 ms power pulse turns it on. Local tooling and credentials are
kept in `/mnt/HaikuWork/x399` and are not committed.
