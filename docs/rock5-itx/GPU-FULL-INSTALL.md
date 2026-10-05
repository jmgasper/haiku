# Installed Mali 3D applications on the Rock 5 ITX NVMe system

The regular ARM64 image includes the existing Mesa/Panfrost OpenGL stack,
Mali firmware, GLInfo as a package, and GLTeapot under
`/boot/system/non-packaged/demos`. The board's `mali_csf` firmware profile is
enabled by default for this image. The underlying renderer, OpenGL Kit and
GLTeapot were previously qualified in the two-boot native graphics trials
described in [MESA-SYSTEM.md](MESA-SYSTEM.md) and [DISPLAY.md](DISPLAY.md).

## Verification on 2026-09-22

The corrected `hrev60097+326+dirty` Haiku package booted from
`/dev/disk/nvme/0/1` with the 256 GB SPCC SSD. The native log recorded the
Mali CSF resource validation and enabled identity profile. Starting GLInfo
from `/boot/system/apps/GLInfo` opened its window with renderer
**Mali-G610 (Panfrost)**, OpenGL **3.1 Mesa 25.3.6**, and GLU 1.3. See
`artifacts/nvme-large-io-20260922/glinfo-c.jpg` and `native-serial.log`.

Starting `/boot/system/non-packaged/demos/GLTeapot` from the normal shell,
without graphics environment overrides, displayed the 3D teapot at 59 FPS.
The changed teapot orientation in `glteapot-b.jpg` and `glteapot-c.jpg` in the
same artifact directory shows live rendering. GLInfo remained open beside it.
The image whose package was installed passed the QEMU Welcome-screen smoke;
QEMU has no Mali device and was not used as hardware acceleration evidence.

GLInfo printed a `GL_INVALID_OPERATION` diagnostic for its convolution
parameter query. Its renderer identification and window remained available.
These tests verify the installed 3D path and the two requested applications;
they do not establish general OpenGL conformance or performance across all
applications.
