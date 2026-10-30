// Host shim: IOLog/IOSleep for tests. No kernel behaviour is emulated.
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include "IOTypes.h"
static inline void IOLog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
static inline void IOSleep(unsigned ms) { usleep(ms * 1000u); }
