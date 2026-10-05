#include "host.h"
extern "C" void* load_driver_settings(const char*);
extern "C" bool get_driver_boolean_parameter(void*, const char*, bool, bool);
extern "C" int unload_driver_settings(void*);
