# Regular ARM64 image for the ROCK 5 ITX

The `@rock5full-mmc` target produces a bootable ARM64 EFI image with a 32 MiB
ESP and 6144 MiB BFS partition. It selects Haiku's regular image definition,
the in-tree applications and demos, the fork's Mesa/OpenGL stack, and the
owner's Amp, Kiri and Turbo Chook packages. `tools/rock5-itx/UserBuildConfig`
names the image `build/arm64/haiku-rock5full-mmc.image`.

The ARM64 HaikuPorts repository is still a bootstrap set. WebPositive and other
HaikuPorts desktop packages are not part of this image. That is a remaining
package-porting task if "full" includes third-party desktop applications.

## Build inputs and commands

Source `tools/rock5-itx/env.sh` first. The pinned Mesa/GLVND and GLU artifacts
must be prepared using `tools/rock5-itx/mesa/`; their inputs are recorded in
`tools/rock5-itx/mesa/sources.json`. The three owner application binaries are
cross-built from the checkouts under `/mnt/HaikuWork/apps`; their package
builder uses the resulting `build-arm64` binaries. All generated assets live
under `/mnt/HaikuWork/rock5-image-extras` and stay out of Git.

```sh
tools/rock5-itx/build-fluidlite-arm64.sh
tools/rock5-itx/build-app-packages.sh
tools/rock5-itx/build-glinfo-package.sh
tools/rock5-itx/build-full.sh
```

The FluidLite builder pins commit `e64b4b3161cab212fffe7d1d3fb1c05750c363cc`
and its archive SHA-256; SF3 uses its bundled stb Vorbis decoder, avoiding an
ARM64 libvorbis package dependency. `build/jam/BuildFeatures` only enables this
local feature for the Rock 5 full profile. The resulting `libmidi.so` is in the
normal Haiku package; MidiPlayer, PatchBay and the Media preference are built.

GLInfo is cross-built from Haiku's own source, against the same ARM64 Mesa and
GLU artifacts as GLTeapot. Its package supplies its icon, app signature and
Deskbar entry. GLTeapot and Haiku3d are supplied under `system/non-packaged/demos`
because the normal `@mesa` feature is unavailable in the ARM64 package index.

`build-full.sh` holds the build lock and records source state, owner app
revisions, buildtools revision, the hashes of all image extras, image checksum
and EFI layout in `build/arm64/full-build-record.json`.

## Evidence, 2026-09-22

- Image SHA-256: `7bd339ee695281bec7e105f3b353c5e8e60252030e2c9086486f8f83da3de658`.
  It was built from `hrev60097+321+dirty`; the full build record preserves
  the source diff and untracked-file hashes.
- The Haiku package contains 36 in-tree applications, 22 preferences and
  11 demos. The image adds Amp, Kiri, Turbo Chook and GLInfo as packages, and
  GLTeapot and Haiku3d as non-packaged demos: 40 apps and 13 demos in total.
  The package listing is in `artifacts/rock5full-package-list-20260922.txt`.
- The exact recorded image passed the 65-second QEMU smoke test at
  `artifacts/qemu/20260922T104527Z-b828fc`. It reached the first-run Welcome
  window, framebuffer initialization completed and no kernel panic appeared.
  The earlier `artifacts/rock5full-midi-qemu-20260922` run also recorded first
  boot activation of `rock5_glinfo`.
- On the board's previous full eMMC install, Amp, Kiri and Turbo Chook appear
  in Deskbar with their own icons and launch successfully; see
  [EMMC-INSTALL.md](EMMC-INSTALL.md). GLInfo was subsequently installed with
  `pkgman` on that running install and launched. The NanoKVM capture at
  `artifacts/rock5-glinfo-20260922.jpg` shows **Mali-G610 (Panfrost)**,
  **Mesa 3.1 / Mesa 25.3.6** and GLU 1.3 on its window. This verifies the
  GLInfo package on the previous kernel/Mesa install, not a full native boot
  of the latest image or execution of the newly built MIDI apps.

Before calling the image a board-qualified release, boot its exact final
bytes on the board, check the complete application inventory and exercise the
new MIDI applications. The current eMMC remains on the previous full image;
the requested NVMe installation is the next deployment target.
