# ROCK 5 ITX driver settings for full images

Kernel driver settings (`~/config/settings/kernel/drivers/`) that full ROCK 5
images install. Each names the firmware profile under which the driver
attaches: without them the eMMC is not admitted before the boot volume is
mounted, the display driver refuses app_server's accelerant (Screen shows
"Framebuffer") and mali_csf falls back to software rendering.

These were kept in `/mnt/HaikuWork/rock5-image-extras/settings`; they are in the
tree so the air/OS CI images (`tools/airos/ci/UserBuildConfig`) use the same
files.
