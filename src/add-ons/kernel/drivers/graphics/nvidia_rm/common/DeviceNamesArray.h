#pragma once

#include <SupportDefs.h>


class DeviceNamesArray {
private:
	const char **fNames {};

	void Delete();

public:
	inline ~DeviceNamesArray();

	status_t Init(int32 count);

	const char **Names() {return fNames;}
	status_t SetName(int32 index, const char *name);
};


DeviceNamesArray::~DeviceNamesArray()
{
	if (fNames == nullptr) {
		return;
	}
	Delete();
}
