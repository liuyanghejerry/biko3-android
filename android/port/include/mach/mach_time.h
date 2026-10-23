/* mach_absolute_time -> CLOCK_MONOTONIC, with a 1:1 timebase. */
#pragma once
#include <stdint.h>

typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
uint64_t mach_absolute_time(void);
int mach_timebase_info(mach_timebase_info_data_t *info);
int mach_wait_until(uint64_t deadline);
