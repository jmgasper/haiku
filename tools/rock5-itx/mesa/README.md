# Experimental Haiku Mesa package

This owner-fork port connects Mesa 25.3.6/Panfrost to the qualified Haiku Mali
CSF kernel ABI. It has passed offscreen GLES rendering on two native ROCK boots;
[MESA.md](../../../docs/rock5-itx/MESA.md) records exact evidence and limits.
It is not an upstream Mesa/Haiku release or a system package replacement.

`sources.json` pins both original archives, six SDK packages, seven kernel ABI
headers, host compiler helpers and every patch/probe/validator. The complete
Mesa patch includes the pinned HaikuPorts changes; do not apply those twice.
For libglvnd, apply the pinned HaikuPorts patchset, then the sysroot and mutex
patches. Original source licenses/notices remain in place. This fork's original
bridge and test code is MIT licensed and AI-assisted at the owner's request.

## Reconstruct locally

Use the existing ARM64 compiler/package tool described in the lab README.
Host prerequisites are the pinned Mesa compiler helpers in
`/mnt/HaikuWork/toolchains/mesa-host/bin`, Meson/Mako/Python in `mesa-python`,
Ninja, and the extracted LLVM 18 host dependencies in `mesa-native-deps`.
These compile shader sources for the GPU; they are not Haiku shared libraries.
Source archives and SDK packages are local inputs. No downloads, host package
installation, deployment or board reboot occur in this script.

Place the two archives named in `sources.json` in one directory, and the six
SDK packages in another. The +207 SDK snapshot is retained locally at
`artifacts/mali-haiku-mesa/20260915T100752Z-80955e/packages`; the archives are
in that stage's `reconstruction-inputs` directory. Run from the Haiku checkout:

```sh
source tools/rock5-itx/env.sh
python3 tools/rock5-itx/mesa/build.py \
  --archives /mnt/HaikuWork/artifacts/mali-haiku-mesa/20260915T100752Z-80955e/reconstruction-inputs \
  --packages /mnt/HaikuWork/artifacts/mali-haiku-mesa/20260915T100752Z-80955e/packages \
  --output /mnt/HaikuWork/artifacts/mesa-reconstruction/NEW_BUILD
```

The output must be new. The script checks inputs, applies patches, verifies all
changed Mesa files and ABI headers, constructs a private SDK, cross-builds
GLVND/Mesa/the probe and emits `package/manifest.json`. `--prepare-only` stops
after patched-source and SDK preparation. Logs, commands, input receipts and
failures remain in the output directory. This reconstructs pinned sources and
configuration; build-path/debug information can change binary hashes.

The package manifest lists nine assets for `/boot/home/mesa-trial`. The `run`
launcher uses Haiku's `LIBRARY_PATH` and a private GLVND vendor file. Its probe
accepts `--native`, `--software` and `--absent-device`. Native mode expects
`/dev/graphics/mali_csf/0` and the separately supplied, licensed firmware at
`/boot/home/mali_csffw.bin`. Software mode is the explicit QEMU fixture.
Neither a build result nor software rendering qualifies a native candidate:
run the full Haiku build, both QEMU modes, native cycles and recovery checks.

`render_validation.py` independently compares complete readbacks with integer
pixel-region oracles and can write actual RGBA/PNG artifacts.
`native_render_validation.py` also requires cached native GPU properties,
accepted/completed queues, expected runtime joins and allocation baselines.
Frozen build, controller, host-fixture and qualification scripts for the first
passing image remain with the evidence linked in MESA.md.
