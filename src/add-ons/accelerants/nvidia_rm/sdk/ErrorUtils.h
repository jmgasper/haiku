#pragma once

#include <errno.h>
#include <system_error>

extern "C" {
#include "nvstatus.h"
}


class NvStatusErrorCategory: public std::error_category {
private:
	static NvStatusErrorCategory sInstance;

public:
	static const NvStatusErrorCategory &Instance() {return sInstance;}

    const char* name() const noexcept override;
    std::string message(int ev) const override;
};


void RaiseErrno(int err);
void RaiseNvStatus(NV_STATUS res);


inline void CheckErrno(int res)
{
	if (res < 0) {
		RaiseErrno(errno);
	}
}

inline void CheckNvStatus(NV_STATUS res)
{
	if (res != 0) {
		RaiseNvStatus(res);
	}
}
