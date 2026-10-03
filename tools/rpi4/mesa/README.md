# Mesa's v3d driver on air/OS (Raspberry Pi 4)

`build.sh` builds Mesa 25.3.6 with the `v3d` Gallium driver (and softpipe) for
Haiku arm64. It sits on the ROCK 5's pinned Mesa port
(`tools/rock5-itx/mesa`): the same sysroot, cross file, libglvnd and patched
source, plus `mesa-haiku-v3d.patch` from this directory. The patch is applied
to the copy of the source under `/mnt/HaikuWork/rpi4/mesa/mesa-25.3.6` (it is
the difference between that copy and the ROCK 5 build's source).

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
- `egl_haiku.cpp`, `egl/meson.build`: the V3D screen is used whenever the
  device exists (`HAIKU_V3D_DEVICE` names another, `LIBGL_ALWAYS_SOFTWARE`
  skips it); presentation is the ROCK 5 port's copy into a window bitmap.

The result is `build/src/egl/libEGL_mesa.so.0.0.0`; stripped, it replaces
`/boot/system/non-packaged/lib/libEGL_mesa.so.0`. An EGL vendor file naming it
goes into `/boot/system/non-packaged/add-ons/opengl/egl_vendor.d/`.

`gl_probe.cpp` is an offscreen GLES 2 test with full pixel readback: a clear,
then a triangle. Build it with the cross compiler against the same sysroot
(see the command in docs/rpi4/GPU.md).
