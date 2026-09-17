#include "NvKmsApi.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <assert.h>

extern "C" {
#include "nvUnixVersion.h"
#include "nvkms-ioctl.h"
#include "nvkms-api.h"
}
#ifdef __HAIKU__
#include "nvkms-haiku.h"
#endif


NvKmsApi::NvKmsApi()
{
	fFd = FileDesc(open("/dev/" NVIDIA_MODESET_DEVICE_NAME, O_RDWR | O_CLOEXEC));
	assert(fFd.IsSet());
}

int NvKmsApi::Control(NvU32 cmd, void *arg, NvU32 size)
{
#ifdef __HAIKU__
	return ioctl(fFd.Get(), NV_HAIKU_KMS_BASE + cmd, arg, size);
#else
	NvKmsIoctlParams params {};
	params.cmd = cmd;
	params.size = size;
	params.address = reinterpret_cast<NvU64>(arg);
	return ioctl(fFd.Get(), NVKMS_IOCTL_IOWR, &params);
#endif
}
