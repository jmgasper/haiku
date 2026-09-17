#include "DeviceNamesArray.h"

#include <malloc.h>
#include <string.h>
#include <new>


void DeviceNamesArray::Delete()
{
	for (int32 i = 0; fNames[i] != nullptr; i++) {
		free((void*)fNames[i]);
	}
	delete[] fNames;
}

status_t DeviceNamesArray::Init(int32 count)
{
	if (fNames != nullptr) {
		Delete();
	}
	fNames = new(std::nothrow) const char*[count + 1];
	if (fNames == nullptr) {
		return B_NO_MEMORY;
	}
	memset(fNames, 0, sizeof(char*)*(count + 1));
	return B_OK;
}

status_t DeviceNamesArray::SetName(int32 index, const char *name)
{
	char *newName = strdup(name);
	if (newName == nullptr) {
		return B_NO_MEMORY;
	}
	free((void*)fNames[index]);
	fNames[index] = newName;
	return B_OK;
}
