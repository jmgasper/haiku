# Radxa Cubie A7S device tree (air/OS)

`sun60i-a733.dtsi` and `sun60i-a733-cubie-a7s.dts` come from Radxa's U-Boot
(`radxa/u-boot`, branch `u-boot-dlan17`, commit 9da4b3e8), which boots the
board; the `include/dt-bindings` headers come from the same tree. All are
dual-licensed (GPL-2.0 or MIT/BSD).

air/OS adds what its drivers need below the "air/OS additions" marker of the
board file. Build with `tools/cubie-a7s/build-dtb.sh <out.dtb>`; U-Boot hands
`dtb/allwinner/sun60i-a733-cubie-a7s.dtb` on the ESP to the EFI loader.
