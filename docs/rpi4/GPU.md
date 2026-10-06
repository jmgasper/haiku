# GPU on the Raspberry Pi 4 (VideoCore VI, V3D 4.2)

State 2026-10-03: OpenGL runs on the GPU. GLTeapot renders in a window at
about 315 FPS (`evidence/teapot6.jpg`); the offscreen probe is pixel-exact
from 128x128 to 1920x1080 with and without a depth buffer. Vulkan (Mesa's
v3dv, 2026-10-04) renders without a window: `rpi4_vk_probe` reports "V3D
4.2.14.0, Vulkan 1.3.328" and reads back a clear and a triangle exactly.

## Pieces

| Piece | Where | Notes |
|---|---|---|
| Firmware property module | `src/add-ons/kernel/generic/rpi_firmware` | mailbox requests: clocks (the V3D clock is firmware-owned), VL805 reload |
| Kernel driver | `src/add-ons/kernel/drivers/graphics/v3d` | publishes `/dev/graphics/v3d/0` |
| Interface | `headers/private/graphics/v3d/v3d_drm.h` (Linux's uapi structures), `v3d_haiku.h` (request codes, handle/sync stand-ins) | |
| Mesa | `tools/rpi4/mesa` (build script, patch on the ROCK 5's Mesa 25.3.6 port, probe) | `v3d` Gallium driver behind libglvnd and Haiku's EGL driver |

## Kernel driver

- Power: the firmware V3D clock, then the GRAFX power domain's reset line and
  the two ASB bridges (`power_on`/`power_off`). A reset after a hung job is a
  full power cycle of the domain; touching the core any other way after a hang
  stalls the bus.
- Memory: the core sees memory only through its MMU. One flat page table
  (4 MB, uncached) covers the 4 GB GPU address space; a buffer object is a
  locked, write-combining area plus a first-fit range of GPU addresses and its
  page table entries. `MMAP_BO` clones the area into the caller's team and
  returns the address (there is no mmap on a device node).
- Lifetime: the fixed FDT driver node is retained. Its page table, scratch
  page, register mappings and executor are initialized at registration and
  survive the last application close. This avoids repeatedly finding a
  contiguous 4 MiB table after memory fragmentation. Client buffers and sync
  objects are still released per file. `rpi4_v3d_lifetime_probe` checks this
  distinction; native measurements are in `PERFORMANCE.md`.
- Jobs: bin/render lists, TFU and compute (CSD) jobs go into one queue and run
  in submission order on an executor thread; the interrupt handler reports
  frame/flush done, out-of-memory in the binner (the driver hands it another
  buffer) and MMU errors. A job that does not finish in 5 s is reported with
  the core's registers and a summary of its buffers, and the core is reset.
- Sync objects are job sequence numbers; a fence is a token for one. For
  Vulkan a sync object can also be reset or signalled by hand, take over
  another one's state, and be waited for before it has a job; the multi-sync
  extension of the submit requests names several to signal.

## Mesa

`tools/rpi4/mesa/build.sh` builds `libEGL_mesa.so.0` and stages it together
with the EGL vendor file in `/mnt/HaikuWork/rpi4/mesa/stage`; the
`rpi4-airos` image profile installs both (plus GLTeapot and GL Info).
`README.md` in that directory lists what the patch changes.

Presentation is the ROCK 5 port's: the frame is read back and copied into the
window's bitmap. That costs a copy per frame; it is not a scan-out path.

The probe:

    B=/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z
    /mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-g++ \
        --sysroot=$B/sysroot -std=c++17 -O2 \
        -I$B/glvnd-install/boot/system/develop/headers/os/opengl \
        -I$B/sysroot/boot/system/develop/headers/os/opengl \
        tools/rpi4/mesa/gl_probe.cpp -L$B/glvnd-install/boot/system/lib \
        -lEGL -lGLESv2 -o rpi4_gl_probe
    PROBE_DEPTH=1 PROBE_WIDTH=1920 PROBE_HEIGHT=1080 ./rpi4_gl_probe

## Traps met on the way

- **Depth format.** Haiku's Mesa frontend asked for `Z24_UNORM_S8_UINT`
  without asking the driver. V3D only has `S8_UINT_Z24_UNORM`; the store of
  the depth buffer then goes out with output format 0 (a colour format) and
  the render thread wedges after a tile and a half with no error bit set
  anywhere (render list consumed, no FRDONE, GMP status 0x30). The symptom
  looked like a kernel problem for a long time: colour-only frames were
  perfect. What found it: `V3D_DEBUG=cl` (Mesa dumps its command lists) and
  looking at what the core had written into each buffer.
- The core's identity register reads "V3D" in the low three bytes only.
- Mesa's buffer cache mutex must be initialized (Haiku asserts on a zeroed
  mutex).

## Open

- **Vulkan beyond the probe.** There is no Vulkan loader for arm64 Haiku
  (programs link `libvulkan_broadcom.so` and start from
  `vk_icdGetInstanceProcAddr`) and no window system layer, so nothing can
  present yet. Sync files, timeline semaphores, the CPU job queue (indirect
  compute, timestamp and performance queries) and PRIME are not there.
  Nothing beyond the probe's two frames has been run.
- Presentation without the read-back copy.
- 8 GB boards: buffers above 4 GB cannot be mapped by this MMU setup
  (28-bit page numbers are fine, but the driver has only been run on 4 GB).
