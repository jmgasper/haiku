# Mesa's v3d driver on air/OS (Raspberry Pi 4)

`build.sh` builds Mesa 25.3.6 with the `v3d` Gallium driver (and softpipe) for
Haiku arm64. It sits on the ROCK 5's pinned Mesa port
(`tools/rock5-itx/mesa`): the same sysroot, cross file, libglvnd and patched
source, plus the V3D, shader-cache, texture-cache and GPU-readback patches
from this directory. The build script applies them to its copy of the source
under `/mnt/HaikuWork/rpi4/mesa/mesa-25.3.6`, or checks that they are already
applied. It reconfigures existing builds to pick up changed options.

What the patch does:

- `include/xf86drm.h`, `include/libsync.h`: stand-ins for libdrm and libsync.
  The V3D render device (`/dev/graphics/v3d/0`, driver in
  `src/add-ons/kernel/drivers/graphics/v3d`) takes the `drm_v3d_*` structures
  as they are; the header translates the request codes and stands in for sync
  objects with job sequence numbers.
- `v3d_bufmgr.c`: buffers are mapped by the driver (MMAP_BO returns the
  address) and unmapped with `delete_area()`; the wait request's error codes.
- `v3d_fence.c`: a fence is a job token, not a file descriptor.
- `v3d_screen.c`: `get_system_info()` instead of `sysinfo()`; the buffer
  cache's mutex is initialized (Haiku asserts on a zeroed mutex).
- `v3d_drm_winsys.c`: `v3d_haiku_screen_create()` opens the device; stubs for
  the scan-out helpers that only exist with DRM.
- `frontends/hgl/hgl.c`: the depth/stencil buffer gets a format the driver
  supports. The frontend asked for `Z24_UNORM_S8_UINT` whatever the driver;
  V3D only has `S8_UINT_Z24_UNORM`, and with the unsupported one the depth
  store went out with a colour format and wedged the core's render thread.
- `egl_haiku.cpp`, `egl/meson.build`: the V3D screen is used whenever the
  device exists (`HAIKU_V3D_DEVICE` names another, `LIBGL_ALWAYS_SOFTWARE`
  skips it); presentation is the ROCK 5 port's copy into a window bitmap.

The result is `build/src/egl/libEGL_mesa.so.0.0.0`; stripped, it replaces
`/boot/system/non-packaged/lib/libEGL_mesa.so.0`. An EGL vendor file naming it
goes into `/boot/system/non-packaged/add-ons/opengl/egl_vendor.d/`.

`gl_probe.cpp` is an offscreen GLES 2 test with full pixel readback: a clear,
then a triangle. Build it with the cross compiler against the same sysroot
(see the command in docs/rpi4/GPU.md).

## Shader cache

OpenGL enables Mesa's disk shader cache with zlib compression and a 64 MB
limit. The supplemental patch uses Mesa's `dladdr`/file timestamp identity
on Haiku, where `dl_iterate_phdr` cannot provide the mapped ELF build ID.
The build supplies a corrected zlib pkg-config file for the pinned sysroot.

`MESA_SHADER_CACHE_DIR` selects a test directory;
`MESA_SHADER_CACHE_DISABLE=true` bypasses caching, and
`MESA_SHADER_CACHE_SHOW_STATS=1` reports hits when the EGL display is closed.
`gl_probe.cpp` now closes its context and display, checks every rendered
pixel, and prints elapsed rendering/validation time.

On the Pi, the repeated probe reports seven hits and no misses with exact
pixels. It also passes with a truncated cache entry, four concurrent
processes, and a read-only cache location (which disables caching).
Summit's later compositor shaders fall from roughly 50–78 ms each to
3–10 ms for cache hits. First page timing varies with which shaders have
been warmed; see `docs/rpi4/PERFORMANCE.md` and the evidence there.

## Cached textures

Tiled Gallium textures use Normal-WB memory with explicit CPU/GPU ownership
transfers by default. `V3D_HAIKU_CACHED_TEXTURES=0` restores write-combining
mapping for comparisons or troubleshooting. This requires driver capability
`V3D_HAIKU_PARAM_CACHEABLE_BO >= 2`; an older kernel falls back to the
existing write-combining allocation. Persistent/coherent resources, ordinary
buffers and Vulkan allocations keep their existing memory type.

Every CPU map prepares the buffer, even if its virtual address was cached.
The driver waits for prior GPU work and invalidates CPU lines; writable maps
also mark the buffer dirty. A job submission cleans dirty CPU lines before
handing ownership to the GPU. Read-only maps avoid an unnecessary subsequent
clean. Buffer-cache reuse matches both size and memory type. See the public
protocol comments in `headers/private/graphics/v3d/v3d_haiku.h`.

`jam rpi4_v3d_probe`, followed by `rpi4_v3d_probe --cached`, exercises the
capability, TFU transfer and 64 alternating read/write ownership cycles.
`texture_probe.cpp` checks every pixel after repeated maps, partial writes,
GPU rendering, narrow-band reads and PBO transfers. Its optional arguments
are round count, base width and base height (defaults: `40 257 131`).
`build-probes.sh` builds the three GLES probes into `mesa/probes` against the
same pinned EGL/GLESv2 libraries. Native pixel and
application measurements, including the narrow-band readback regression,
are in `docs/rpi4/PERFORMANCE.md`.

## GPU-assisted readback

Large, read-only RGBA8/BGRA8 (including opaque RGBX) texture maps use a
linear GPU staging target by default. V3D stores raster-order pixels there,
and the existing ownership API makes them safe for CPU access. This avoids
CPU detiling and its temporary copy. The minimum is 128 x 64 pixels and
128 Ki pixels in total; small reads keep their CPU path. Writes,
unsynchronized, direct, nonblocking, persistent/coherent, array and other
format mappings retain their existing path. `V3D_HAIKU_GPU_READBACK=0`
disables the optimization. It also falls back when cached textures or the
required kernel capability are unavailable.

The Haiku EGL library exports a private performance hint:
`int haiku_mesa_readback_band_bytes(void)`. It examines the current context,
changes no GL state, and returns -1 for no preference, 0 for whole readback
rectangles, or a positive preferred band size in bytes. Currently only
V3D with GPU readback enabled returns 0. With no current context it returns
-1. Consumers discover the optional symbol in the loaded EGL vendor
library, so older libraries remain usable.

`../summit/webkit-haiku-readback-hint.patch` uses that hint to avoid turning
Summit's former 384 KiB CPU bands into many small GPU jobs. An explicit
`SUMMIT_READBACK_BAND_KB` still takes precedence. `readback_probe.cpp`
checks 48 combinations of formats, MSAA resolve, mip levels, cropped
origins, padded rows and guards. Set `PROBE_READBACK_HINT=0` (or -1 for a
fallback configuration) to check the hint too. The texture probe additionally
checks cropped patterned reads; the depth/triangle probe remains unchanged.

The build validates the entire ordered patch series on temporary copies
before updating the source, recognizing both clean trees and already applied
prefixes. This matters because the readback patch overlaps the texture patch.
Measured application results and rejected experiments are recorded in
`docs/rpi4/PERFORMANCE.md`.

## Vulkan (v3dv)

The same script builds `libvulkan_broadcom.so` in a second build directory.
What the patch changes for it:

- `src/util/u_sync_provider_haiku.c`: Mesa's Vulkan runtime reaches DRM sync
  objects through a small provider interface; this one talks to the render
  device (create, wait, reset, signal, transfer). No sync files, no timelines.
- `src/vulkan/runtime/meson.build`, `src/util/meson.build`: build the DRM
  sync object type and that provider on Haiku.
- `v3dv_device.c`: the device is `/dev/graphics/v3d/0` (no DRM device list,
  no display device), the heap size comes from `get_system_info()`, and the
  cache UUIDs from the version and build time (there is no build-id note).
- `v3dv_bo.c`: buffers are mapped by the driver, as in the GL driver; the
  buffer cache's mutex is initialized.
- `v3dv_wsi.c`, `v3dv_image.c`: nothing to present on; no DRM format
  modifiers.

`vk_probe.c` links the driver directly (there is no Vulkan loader for arm64
Haiku) and renders into an image without a window: a clear, and a triangle
with two shaders written out as SPIR-V words by hand. Build it like the GL
probe, with `-I<mesa>/include -L<build-vk>/src/broadcom/vulkan
-lvulkan_broadcom`, and run it with the library in `LIBRARY_PATH`.
