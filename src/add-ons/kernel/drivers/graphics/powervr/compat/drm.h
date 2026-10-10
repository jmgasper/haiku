/* Stand-in for Linux's uapi <drm/drm.h>, which pvr_drm.h includes: its
   types come from linux_compat.h. The DRM_IOCTL_PVR_* request codes are
   not usable here (no DRM_IOWR); the driver's interface is pvr_haiku.h. */
#include <linux_compat.h>

#define DRM_COMMAND_BASE	0x40
