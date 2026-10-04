// Only the architectural primitives are substituted. The real allocator and
// mapper decide the ranges, operation, barriers and interrupt ordering.
#pragma once
#include "../../host.h"
#include "../../../../../headers/private/kernel/arch/arm64/cache_line_size.h"
uint64_t arm64_current_data_cache_line_size();
void arm64_clean_data_cache_line_poc(uintptr_t);
void arm64_invalidate_data_cache_line_poc(uintptr_t);
void arm64_clean_invalidate_data_cache_line_poc(uintptr_t);
