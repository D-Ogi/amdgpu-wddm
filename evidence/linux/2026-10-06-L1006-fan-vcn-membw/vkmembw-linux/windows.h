/* Linux stand-in for the four Windows calls vkmembw.c uses (L39: the same C file and shaders under RADV). */
#pragma once
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>
typedef void *HMODULE;
typedef union { struct { uint32_t LowPart; int32_t HighPart; } u; long long QuadPart; } LARGE_INTEGER;
static inline int QueryPerformanceCounter(LARGE_INTEGER *c)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    c->QuadPart = (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
    return 1;
}
static inline int QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return 1; }
static inline HMODULE LoadLibraryA(const char *name) { (void)name; return dlopen("libvulkan.so.1", RTLD_NOW); }
static inline void *GetProcAddress(HMODULE m, const char *n) { return dlsym(m, n); }
