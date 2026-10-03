// Host test of driver/kmd/gpu_clock.h (BD-056): amdgpu's hi/lo/hi read of the SMUIO golden TSC across a carry of the
// low word, the narrowest-window pair, the refusals, and the QPC fallback's scaling.
//
// The fake device: one clock, in nanoseconds, that every register read and every CPU counter read advances. The TSC
// is 100 MHz of it plus a base, the CPU counter 10 MHz of it (the lab's QPC). A read returns the TSC half at the time
// of that read, so a read sequence that straddles a carry sees the carry where real hardware would.
#include <stdio.h>
#include <string.h>

#include "../gpu_clock.h"

#define UPPER 0x5AC14ul     // only distinct values matter here; the driver passes regs.generated.h's names
#define LOWER 0x5AC18ul

static int g_failures, g_checks;
#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

typedef struct {
    unsigned long long Ns;          // the one clock
    unsigned long long TscBase;     // TSC ticks at Ns 0
    unsigned long long ReadNs;      // a register read costs this much
    const unsigned long long* CpuSteps; // what each CPU counter read costs, in turn (0: 100 ns)
    unsigned int CpuCalls, Reads;
    int FailRead;                   // the read with this 1-based number fails
    int AllOnes;                    // every read returns all ones
    unsigned long long FirstTsc, LastTsc;   // the TSC at the first and the last register read
} FAKE;

static unsigned long long Tsc(const FAKE* F) { return F->TscBase + F->Ns / 10u; }

static int FakeRead(void* Context, unsigned long Offset, unsigned long* Value)
{
    FAKE* f = (FAKE*)Context;
    unsigned long long tsc;
    f->Ns += f->ReadNs;
    f->Reads++;
    if (f->FailRead && (int)f->Reads == f->FailRead) return 1;
    tsc = Tsc(f);
    if (f->Reads == 1) f->FirstTsc = tsc;
    f->LastTsc = tsc;
    *Value = f->AllOnes ? 0xFFFFFFFFul : Offset == UPPER ? (unsigned long)(tsc >> 32) : (unsigned long)tsc;
    return 0;
}

static unsigned long long FakeCpu(void* Context)
{
    FAKE* f = (FAKE*)Context;
    unsigned long long step = f->CpuSteps ? f->CpuSteps[f->CpuCalls] : 100u;
    f->CpuCalls++;
    f->Ns += step;
    return f->Ns / 100u;            // 10 MHz
}

static void Init(FAKE* F, unsigned long long TscBase)
{
    memset(F, 0, sizeof(*F));
    F->TscBase = TscBase;
    F->ReadNs = 400;
}

// What the naive read (upper, then lower, no check) returns, for the negative control.
static unsigned long long Naive(FAKE* F)
{
    unsigned long hi = 0, lo = 0;
    FakeRead(F, UPPER, &hi);
    FakeRead(F, LOWER, &lo);
    return (unsigned long long)hi << 32 | lo;
}

static void TestCarry(void)
{
    FAKE f;
    unsigned long hi = 0, lo = 0;
    unsigned long long start, value;
    int straddled = 0;

    // Every start from 300 ticks before a carry of the low word to just past it, in 1-tick steps: the result is
    // always a value the counter held during the reads (never 2^32 off), and some starts do straddle the carry.
    for (start = 0x43FFFFFF00ull - 300u; start <= 0x4400000000ull + 10u; start++) {
        Init(&f, start);
        CHECK(GpuClockRead(&f, FakeRead, UPPER, LOWER, &hi, &lo) == 0);
        value = (unsigned long long)hi << 32 | lo;
        CHECK(value >= f.FirstTsc && value <= f.LastTsc);
        if ((f.FirstTsc >> 32) != (f.LastTsc >> 32)) straddled++;
    }
    CHECK(straddled > 0);

    // The re-read is what makes it right: the naive read just before the carry comes back 2^32 low.
    Init(&f, 0x43FFFFFFFFull - 60u);    // upper read 20 ticks before the carry, lower 20 after
    value = Naive(&f);
    CHECK(value + 0x80000000ull < f.LastTsc);
    Init(&f, 0x43FFFFFFFFull - 60u);    // upper read 20 ticks before the carry, lower 20 after
    CHECK(GpuClockRead(&f, FakeRead, UPPER, LOWER, &hi, &lo) == 0);
    CHECK(hi == 0x44ul && ((unsigned long long)hi << 32 | lo) <= f.LastTsc);
    CHECK(f.Reads == 4);            // upper, lower, upper changed, lower again

    // No carry: three reads, the value of the lower read.
    Init(&f, 0x0000004312345678ull);
    CHECK(GpuClockRead(&f, FakeRead, UPPER, LOWER, &hi, &lo) == 0);
    CHECK(f.Reads == 3 && hi == 0x43ul && lo == 0x12345678ul + 80u);
}

static void TestRefusals(void)
{
    FAKE f;
    GPU_CLOCK_PAIR pair;
    unsigned long hi = 7, lo = 7;
    int n;

    for (n = 1; n <= 3; n++) {
        Init(&f, 0x100000000ull);
        f.FailRead = n;
        CHECK(GpuClockRead(&f, FakeRead, UPPER, LOWER, &hi, &lo) == 1);
        CHECK(hi == 7 && lo == 7);  // not written on failure
    }
    // The fourth read (the lower again after a carry) fails too.
    Init(&f, 0x43FFFFFFFFull - 60u);    // upper read 20 ticks before the carry, lower 20 after
    f.FailRead = 4;
    CHECK(GpuClockRead(&f, FakeRead, UPPER, LOWER, &hi, &lo) == 1);
    Init(&f, 0);
    f.AllOnes = 1;
    CHECK(GpuClockRead(&f, FakeRead, UPPER, LOWER, &hi, &lo) == 2);
    // A failure in any sample fails the calibration, and the pair is left alone.
    memset(&pair, 0xA5, sizeof(pair));
    Init(&f, 0x100000000ull);
    f.FailRead = 5;                 // second sample's second read
    CHECK(GpuClockCalibrate(&f, FakeRead, FakeCpu, UPPER, LOWER, &pair) == 1);
    Init(&f, 0);
    f.AllOnes = 1;
    CHECK(GpuClockCalibrate(&f, FakeRead, FakeCpu, UPPER, LOWER, &pair) == 2);
}

static void TestNarrowest(void)
{
    // CPU read costs per call: before/after of sample 0, 1, 2. Sample 1 is interrupted least.
    static const unsigned long long steps[] = { 100, 50000, 100, 100, 100, 9000 };
    FAKE f;
    GPU_CLOCK_PAIR pair;
    unsigned long long cpu[6], ns;

    Init(&f, 0x1234ull << 32);
    f.CpuSteps = steps;
    CHECK(GpuClockCalibrate(&f, FakeRead, FakeCpu, UPPER, LOWER, &pair) == 0);
    CHECK(f.CpuCalls == 2u * GPU_CLOCK_SAMPLES && f.Reads == 3u * GPU_CLOCK_SAMPLES);
    // Replay the clock to know each sample's CPU readings: sample i's reads sit between cpu[2i] and cpu[2i+1].
    ns = 0;
    {
        unsigned int i;
        for (i = 0; i < 6; i++) {
            ns += steps[i];
            cpu[i] = ns / 100u;
            if (i % 2 == 0) ns += 3u * f.ReadNs;
        }
    }
    CHECK(pair.Window == cpu[3] - cpu[2]);
    CHECK(pair.Cpu == cpu[2] + (cpu[3] - cpu[2]) / 2u);
    CHECK(pair.Window < cpu[1] - cpu[0] && pair.Window < cpu[5] - cpu[4]);
    // The kept GPU value belongs to sample 1: inside its reads, and the pair agrees with the fake's 10:1 ratio
    // to within the window.
    CHECK(pair.Gpu == ((unsigned long long)pair.Upper << 32 | pair.Lower));
    {
        unsigned long long cpuNs = pair.Cpu * 100u, gpuNs = (pair.Gpu - f.TscBase) * 10u;
        unsigned long long diff = cpuNs > gpuNs ? cpuNs - gpuNs : gpuNs - cpuNs;
        CHECK(diff <= (pair.Window + 1u) * 100u);
    }
}

static void TestRatio(void)
{
    // The lab's acceptance: across two calibrations the GPU counter advances by GpuFrequency x the elapsed CPU time.
    FAKE f;
    GPU_CLOCK_PAIR a, b;
    const unsigned long long gpuHz = 100000000ull, cpuHz = 10000000ull;
    unsigned long long dg, dc;

    Init(&f, 0x0000004300000000ull - 5000000ull);
    CHECK(GpuClockCalibrate(&f, FakeRead, FakeCpu, UPPER, LOWER, &a) == 0);
    f.Ns += 16666667ull;            // a frame later, across a carry
    CHECK(GpuClockCalibrate(&f, FakeRead, FakeCpu, UPPER, LOWER, &b) == 0);
    dg = b.Gpu - a.Gpu;
    dc = b.Cpu - a.Cpu;
    // dg / gpuHz == dc / cpuHz to within one CPU tick (100 ns) and the windows' halves.
    {
        unsigned long long lhs = dg * cpuHz, rhs = dc * gpuHz, diff = lhs > rhs ? lhs - rhs : rhs - lhs;
        CHECK(diff <= (2u + a.Window + b.Window) * gpuHz);
        CHECK(dg > 1600000ull && dg < 1700000ull);
    }
}

static void TestScale(void)
{
    CHECK(GpuClockScale(0, 10000000ull, 100000000ull) == 0);
    CHECK(GpuClockScale(1, 10000000ull, 100000000ull) == 10);
    CHECK(GpuClockScale(10000000ull, 10000000ull, 100000000ull) == 100000000ull);
    // 30 days of a 10 MHz QPC: no overflow, exact.
    CHECK(GpuClockScale(25920000000000ull, 10000000ull, 100000000ull) == 259200000000000ull);
    // A QPC frequency that does not divide: 3579545 Hz (the ACPI PM timer) into 100 MHz, exact floor.
    CHECK(GpuClockScale(3579545ull * 1000ull + 1u, 3579545ull, 100000000ull) == 100000000000ull + 100000000ull / 3579545ull);
    {
        unsigned long long t;
        for (t = 0; t < 2000000ull; t += 7919u)
            CHECK(GpuClockScale(t, 3579545ull, 100000000ull) == t * 100000000ull / 3579545ull);
    }
}

int main(void)
{
    TestCarry();
    TestRefusals();
    TestNarrowest();
    TestRatio();
    TestScale();
    printf("gpu_clock_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
