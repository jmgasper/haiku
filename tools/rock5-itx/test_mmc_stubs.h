/* Minimal host types; no MMC command or register logic is reimplemented here. */
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int32 = int32_t;
using status_t = int32;
using bigtime_t = int64_t;
using sem_id = int32;
using thread_id = int32;
using area_id = int32;
using generic_size_t = uint64;
using phys_addr_t = uint64;
struct driver_module_info {};
struct device_node {};
struct generic_io_vec { uint64 base, length; };
constexpr status_t B_OK = 0, B_ERROR = -1, B_BAD_VALUE = -2, B_BAD_DATA = -3,
    B_IO_ERROR = -4, B_TIMED_OUT = -5, B_INTERRUPTED = -6, B_BUSY = -7,
    B_SHUTTING_DOWN = -8, B_NOT_SUPPORTED = -9, B_NO_MEMORY = -10, B_READ_ONLY_DEVICE = -11;
constexpr int B_ABSOLUTE_TIMEOUT = 1, B_NORMAL_PRIORITY = 10,
    B_UNHANDLED_INTERRUPT = 0, B_HANDLED_INTERRUPT = 1, B_DO_NOT_RESCHEDULE = 1;
#define TRACE(...) do {} while (false)
#define TRACE_ALWAYS(...) do {} while (false)
#define ERROR(...) do {} while (false)
#define CALLED(...) do {} while (false)
#define panic(...) assert(false)

#include "mmc.h"
