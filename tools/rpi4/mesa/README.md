# Mesa's v3d driver on air/OS (Raspberry Pi 4)

`build.sh` builds Mesa 25.3.6 with the `v3d` Gallium driver (and softpipe) for
Haiku arm64. It sits on the ROCK 5's pinned Mesa port
(`tools/rock5-itx/mesa`): the same sysroot, cross file, libglvnd and patched
source, plus `mesa-haiku-v3d.patch` and `mesa-haiku-shader-cache.patch` from
this directory. The build script applies them to its copy of the source
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
