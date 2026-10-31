// Host shim: IOLog/IOSleep for tests. No kernel behaviour is emulated.
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <mutex>
#include "IOTypes.h"
static inline void IOLog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
static inline void IOSleep(unsigned ms) { usleep(ms * 1000u); }
using IOLock = std::mutex;
static inline IOLock* IOLockAlloc() { return new IOLock; }
static inline void IOLockFree(IOLock* lock) { delete lock; }
static inline void IOLockLock(IOLock* lock) { lock->lock(); }
static inline void IOLockUnlock(IOLock* lock) { lock->unlock(); }
static inline void* IOMalloc(size_t size) { return malloc(size); }
static inline void IOFree(void* memory, size_t) { free(memory); }
