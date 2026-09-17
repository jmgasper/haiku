#include "NvKmsSurface.h"

#include "NvKmsApi.h"
#include "NvKmsDevice.h"
#include "ErrorUtils.h"


void NvKmsSurface::Delete()
{
	NvKmsUnregisterSurfaceParams params {};
	params.request.deviceHandle = fKmsDev->Info().deviceHandle;
	params.request.surfaceHandle = fHandle;
	CheckErrno(fKmsDev->Kms().Control(NVKMS_IOCTL_UNREGISTER_SURFACE, &params, sizeof(params)));
}
