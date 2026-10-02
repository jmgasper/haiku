#pragma once

extern "C" {
#include "nvtypes.h"
}

#include "FileDesc.h"


class NvKmsApi {
private:
	FileDesc fFd;

public:
	NvKmsApi();

	int Fd() const {return fFd.Get();}
	int Control(NvU32 cmd, void *arg, NvU32 size);
};
