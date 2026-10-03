// The GPU clock counter and its pairing with the CPU's, for DxgkDdiCalibrateGpuClock (wddm.c, BD-056).
//
// The counter is the SMUIO golden TSC, 100 MHz, the clock the CP writes RELEASE_MEM/EOP timestamps in. amdgpu reads
// it on GC 10.1.3 in gfx_v10_0_get_gpu_clock_counter() (driver/amdgpu-import/reference/gfx_v10_0.c:7694-7706, MIT):
// upper, lower, upper again, and if the upper half moved in between, lower once more under the new upper half (the
// low word carries over every ~42 s). This header does the same through a read callback, and brackets each read with
// the CPU counter so that the caller gets the pair with the smallest window, the CPU time taken at its middle.
// Pure: no WDK header, so driver/kmd/test/gpu_clock_test.c checks it on the host.
#pragma once

#define GPU_CLOCK_SAMPLES 3u        // pairs taken per calibration; the narrowest one is kept

// One register read: returns 0 and sets *Value, or nonzero when the read could not be made.
typedef int (*GPU_CLOCK_READ)(void* Context, unsigned long Offset, unsigned long* Value);
// The CPU counter (KeQueryPerformanceCounter in the driver).
typedef unsigned long long (*GPU_CLOCK_CPU)(void* Context);

typedef struct _GPU_CLOCK_PAIR {
    unsigned long long Gpu;         // the 64-bit counter
    unsigned long long Cpu;         // the CPU counter at the middle of the window
    unsigned long long Window;      // CPU ticks between the reads before and after the register reads
    unsigned long Upper, Lower;     // the raw halves, for the log
} GPU_CLOCK_PAIR;

// amdgpu's hi/lo/hi read. 0 on success; 1 when a read failed; 2 when both halves read all ones (a BAR that answers
// nothing: device powered down or gone), which is not a counter value.
static __inline int GpuClockRead(void* Context, GPU_CLOCK_READ Read, unsigned long UpperOffset, unsigned long LowerOffset,
                                 unsigned long* Upper, unsigned long* Lower)
{
    unsigned long hi = 0, lo = 0, check = 0;
    if (Read(Context, UpperOffset, &hi) || Read(Context, LowerOffset, &lo) || Read(Context, UpperOffset, &check)) return 1;
    if (check != hi) {
        if (Read(Context, LowerOffset, &lo)) return 1;
        hi = check;
    }
    if (hi == 0xFFFFFFFFul && lo == 0xFFFFFFFFul) return 2;
    *Upper = hi;
    *Lower = lo;
    return 0;
}

// GPU_CLOCK_SAMPLES pairs, the one with the smallest CPU window kept (the first of equals). Returns as GpuClockRead
// does, for the first failing read; Pair is filled only on 0.
static __inline int GpuClockCalibrate(void* Context, GPU_CLOCK_READ Read, GPU_CLOCK_CPU Cpu, unsigned long UpperOffset,
                                      unsigned long LowerOffset, GPU_CLOCK_PAIR* Pair)
{
    unsigned int i;
    int found = 0;
    for (i = 0; i < GPU_CLOCK_SAMPLES; i++) {
        unsigned long hi = 0, lo = 0;
        unsigned long long before = Cpu(Context), after, window;
        int error = GpuClockRead(Context, Read, UpperOffset, LowerOffset, &hi, &lo);
        after = Cpu(Context);
        if (error) return error;
        window = after >= before ? after - before : 0;
        if (!found || window < Pair->Window) {
            Pair->Gpu = (unsigned long long)hi << 32 | lo;
            Pair->Cpu = before + window / 2;
            Pair->Window = window;
            Pair->Upper = hi;
            Pair->Lower = lo;
            found = 1;
        }
    }
    return 0;
}

// Ticks of a From Hz counter in a To Hz counter (the fallback: the CPU counter at 10 MHz into 100 MHz ticks). Exact,
// and free of overflow while the result fits in 64 bits and the frequencies are below 2^32 (at 100 MHz: 5800 years).
static __inline unsigned long long GpuClockScale(unsigned long long Ticks, unsigned long long FromHz, unsigned long long ToHz)
{
    return Ticks / FromHz * ToHz + Ticks % FromHz * ToHz / FromHz;
}
