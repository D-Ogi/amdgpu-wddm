// The adapter and node performance data of DxgkDdiQueryAdapterInfo (BD-404): DXGKQAITYPE_NODEPERFDATA (24),
// ADAPTERPERFDATA (25), ADAPTERPERFDATA_CAPS (26) and GPUVERSION (27). perf_data.c collects the sources and writes
// the WDK structures. This header holds the decisions: the input words, the units and which source stands behind
// which field. Pure: no WDK header, so driver/kmd/test/perf_data_test.c checks it on the host.
//
// The contract. The WDK 10.0.26100 d3dkmdt.h gives the four output structures under #pragma pack(1) (d3dkmdt.h:1493
// to 2500): DXGK_NODE_PERFDATA 44 bytes, DXGK_ADAPTER_PERFDATA 53, DXGK_ADAPTER_PERFDATACAPS 28 and DXGK_GPUVERSION
// 128; perf_data.c asserts the sizes. The units are those of the header and of the DDI reference pages: hertz,
// millivolts, RPM, tenths of a percent and tenths of a degree Celsius. The input is not documented anywhere. A
// static reading of dxgkrnl 26100.9974 (scratch/t39/bd404-re) shows it:
//   - NODEPERFDATA: DXGADAPTER::GetNodePerfData passes 4 bytes, NodeOrdinal | PhysicalAdapterIndex << 16, the
//     packing of DXGKNODEMETADATA_GETNODEORDINAL (d3dkmddi.h:8648), and an output of 0x2C bytes. It checks both
//     indexes against its own counts first, and asks the driver at most every 450 ms per node; between two asks
//     it returns its copy of the last answer.
//   - ADAPTERPERFDATA: GetAdapterPerfData passes 4 bytes, the PhysicalAdapterIndex, and an output of 0x35 bytes,
//     with the same 450 ms copy. The driver's status goes to the caller (BD-404 was STATUS_NOT_SUPPORTED there).
//   - ADAPTERPERFDATA_CAPS and GPUVERSION: asked once, when dxgkrnl creates the adapter and the driver reports a
//     WDDM version of 2.4 or later, with the PhysicalAdapterIndex as input; outputs of 0x1C and 0x80 bytes. A
//     refusal is only logged. A caller then reads dxgkrnl's copy, which stays zero.
// Zero is therefore what dxgkrnl itself holds for a driver that gives no answer, and it is the value of a field this
// driver has no honest source for. The contract names no other "not available" value.
#pragma once

#define BC250_PERF_NAME_CHARS 32u           // DXGK_MAX_GPUVERSION_NAME_LENGTH (d3dkmdt.h:2073), checked in perf_data.c
#define BC250_PERF_HZ_PER_MHZ 1000000ull

// What the driver knows at the time of one query. A Valid flag says that a fresh reading stands behind the value
// next to it; the value is ignored without it. The envelope is the limit set of a positively identified board.
typedef struct _BC250_PERF_SOURCES {
    int GfxClockValid;
    unsigned GfxMHz;                        // the GFX clock as the SMU reads it back (GetGfxFrequency)
    int GfxVoltageValid;
    unsigned GfxMv;                         // the VDDCR_GFX rail as the SMU metrics table reads it
    int TemperatureValid;
    long TemperatureMc;                     // millidegrees Celsius: the reading the clock governor throttles on
    int FanValid;
    unsigned FanRpm;                        // the main fan (Bc250PerfMainFan)
    int PoweredOn;                          // the adapter is in D0 and its power transaction is not in progress
    int EnvelopeValid;
    unsigned GfxMaxMHz, GfxMaxMv;           // the top of the qualified clock table
    unsigned GfxMaxOcMHz, GfxMaxOcMv;       // the top of the overclock tier in force (the table's top for none)
    long TemperatureWarningMc;              // where the governor's thermal cap starts to lower the clock
    long TemperatureMaxMc;                  // where the governor forces the thermal floor
} BC250_PERF_SOURCES;

// The fields of DXGK_NODE_PERFDATA, DXGK_ADAPTER_PERFDATA and DXGK_ADAPTER_PERFDATACAPS, in their units.
typedef struct _BC250_PERF_NODE {
    unsigned long long Frequency, MaxFrequency, MaxFrequencyOC;     // Hz
    unsigned Voltage, VoltageMax, VoltageMaxOC;                     // mV
    unsigned long long MaxTransitionLatency;                        // 100 ns
} BC250_PERF_NODE;

typedef struct _BC250_PERF_ADAPTER {
    unsigned long long MemoryFrequency, MaxMemoryFrequency, MaxMemoryFrequencyOC;   // Hz
    unsigned long long MemoryBandwidth, PCIEBandwidth;                              // bytes transferred
    unsigned FanRPM;
    unsigned Power;                                                 // 0.1 % of the adapter's power limit
    unsigned Temperature;                                           // 0.1 C
    unsigned char PowerStateOverride;                               // 1: powered on
} BC250_PERF_ADAPTER;

typedef struct _BC250_PERF_CAPS {
    unsigned long long MaxMemoryBandwidth, MaxPCIEBandwidth;        // bytes per second
    unsigned MaxFanRPM;
    unsigned TemperatureMax, TemperatureWarning;                    // 0.1 C
} BC250_PERF_CAPS;

// The verdict on an input word. perf_data.c answers every refusal with STATUS_INVALID_PARAMETER.
#define BC250_PERF_INPUT_OK 0
#define BC250_PERF_INPUT_MISSING 1          // no input buffer, or fewer than 4 bytes
#define BC250_PERF_INPUT_ADAPTER 2          // a physical adapter other than 0: this driver has one, no LDA
#define BC250_PERF_INPUT_NODE 3             // a node ordinal at or above the node count of this start

static __inline unsigned Bc250PerfInputWord(const void* Input) { return *(const unsigned*)Input; }

static __inline int Bc250PerfAdapterInput(const void* Input, unsigned InputSize)
{
    if (Input == 0 || InputSize < sizeof(unsigned)) return BC250_PERF_INPUT_MISSING;
    return Bc250PerfInputWord(Input) != 0u ? BC250_PERF_INPUT_ADAPTER : BC250_PERF_INPUT_OK;
}

// NodeOrdinal in the low half, PhysicalAdapterIndex in the high half. *Ordinal is written on OK only.
static __inline int Bc250PerfNodeInput(const void* Input, unsigned InputSize, unsigned NodeCount, unsigned* Ordinal)
{
    unsigned word;
    if (Input == 0 || InputSize < sizeof(unsigned)) return BC250_PERF_INPUT_MISSING;
    word = Bc250PerfInputWord(Input);
    if ((word >> 16) != 0u) return BC250_PERF_INPUT_ADAPTER;
    if ((word & 0xFFFFu) >= NodeCount) return BC250_PERF_INPUT_NODE;
    *Ordinal = word & 0xFFFFu;
    return BC250_PERF_INPUT_OK;
}

// Millidegrees to the contract's tenths of a degree. Truncated, so that a reading never shows above a limit it has
// not reached; the field is unsigned, so a reading below 0 C shows as 0.
static __inline unsigned Bc250PerfDeciCelsius(long Mc)
{
    return Mc <= 0 ? 0u : (unsigned)(Mc / 100);
}

// The main fan: "the current rpm of the main fan" (ns-d3dkmdt-_dxgk_adapter_perfdata.md). The board's monitor says
// which headers have a tachometer, not which fan cools the GPU, so the main fan is the fastest present channel that
// the sample accepted. Returns 0 and leaves *Rpm alone when no present channel was accepted.
static __inline int Bc250PerfMainFan(const unsigned* RpmPerChannel, unsigned Count, unsigned ValidMask,
                                     unsigned PresentMask, unsigned* Rpm)
{
    unsigned i, best = 0u;
    int any = 0;
    for (i = 0; i < Count && i < 32u; i++) {
        if ((ValidMask & PresentMask & (1u << i)) == 0u) continue;
        if (!any || RpmPerChannel[i] > best) best = RpmPerChannel[i];
        any = 1;
    }
    if (any) *Rpm = best;
    return any;
}

// GFX nodes (3D, compute) run on the GFX clock and rail. Another node (the copy engine) has no clock or voltage
// source in this driver, and gets zeros. No transition latency is measured: MaxTransitionLatency stays 0.
static __inline void Bc250PerfFillNode(const BC250_PERF_SOURCES* S, int GfxNode, BC250_PERF_NODE* Out)
{
    Out->Frequency = Out->MaxFrequency = Out->MaxFrequencyOC = 0ull;
    Out->Voltage = Out->VoltageMax = Out->VoltageMaxOC = 0u;
    Out->MaxTransitionLatency = 0ull;
    if (!GfxNode) return;
    if (S->GfxClockValid) Out->Frequency = (unsigned long long)S->GfxMHz * BC250_PERF_HZ_PER_MHZ;
    if (S->GfxVoltageValid) Out->Voltage = S->GfxMv;
    if (S->EnvelopeValid) {
        Out->MaxFrequency = (unsigned long long)S->GfxMaxMHz * BC250_PERF_HZ_PER_MHZ;
        Out->MaxFrequencyOC = (unsigned long long)S->GfxMaxOcMHz * BC250_PERF_HZ_PER_MHZ;
        Out->VoltageMax = S->GfxMaxMv;
        Out->VoltageMaxOC = S->GfxMaxOcMv;
    }
}

// The memory clock, the transfer counters and the power share have no source in this driver: the SMU metrics
// reader does not decode MemclkFrequency, nothing counts transferred bytes, and the driver knows no power limit to
// take a share of. All of them stay 0. The APU has no PCIe transfer of its own memory at all.
static __inline void Bc250PerfFillAdapter(const BC250_PERF_SOURCES* S, BC250_PERF_ADAPTER* Out)
{
    Out->MemoryFrequency = Out->MaxMemoryFrequency = Out->MaxMemoryFrequencyOC = 0ull;
    Out->MemoryBandwidth = Out->PCIEBandwidth = 0ull;
    Out->Power = 0u;
    Out->FanRPM = S->FanValid ? S->FanRpm : 0u;
    Out->Temperature = S->TemperatureValid ? Bc250PerfDeciCelsius(S->TemperatureMc) : 0u;
    Out->PowerStateOverride = S->PoweredOn ? 1u : 0u;
}

// TemperatureWarning is "the temperature level where temperature based throttling begins" and TemperatureMax "the
// maximum temperature before damage levels occur" (ns-d3dkmdt-_dxgk_adapter_perfdatacaps.md). No source gives the
// silicon's damage level; the envelope's TemperatureMaxMc is the driver's own emergency limit, which is lower, so the
// answer errs on the safe side. The fan's maximum speed depends on the fan the owner fitted: MaxFanRPM stays 0.
static __inline void Bc250PerfFillCaps(const BC250_PERF_SOURCES* S, BC250_PERF_CAPS* Out)
{
    Out->MaxMemoryBandwidth = Out->MaxPCIEBandwidth = 0ull;
    Out->MaxFanRPM = 0u;
    Out->TemperatureMax = Out->TemperatureWarning = 0u;
    if (!S->EnvelopeValid) return;
    Out->TemperatureMax = Bc250PerfDeciCelsius(S->TemperatureMaxMc);
    Out->TemperatureWarning = Bc250PerfDeciCelsius(S->TemperatureWarningMc);
}

static __inline unsigned Bc250PerfPutDecimal(unsigned short* Out, unsigned At, unsigned Value)
{
    unsigned short digits[10];
    unsigned n = 0u;
    do { digits[n++] = (unsigned short)('0' + Value % 10u); Value /= 10u; } while (Value != 0u && n < 10u);
    while (n != 0u && At < BC250_PERF_NAME_CHARS - 1u) Out[At++] = digits[--n];
    return At;
}

// "GC major.minor.revision" from amdgpu's IP_VERSION packing (asic_ip.h), the graphics IP of the part. 0, for a part
// without a row in the IP table, gives an empty name. The rest of the array is zero.
static __inline void Bc250PerfArchitecture(unsigned GcVersion, unsigned short Out[BC250_PERF_NAME_CHARS])
{
    unsigned i, at = 0u;
    for (i = 0; i < BC250_PERF_NAME_CHARS; i++) Out[i] = 0;
    if (GcVersion == 0u) return;
    Out[at++] = 'G';
    Out[at++] = 'C';
    Out[at++] = ' ';
    at = Bc250PerfPutDecimal(Out, at, (GcVersion >> 16) & 0xFFu);
    Out[at++] = '.';
    at = Bc250PerfPutDecimal(Out, at, (GcVersion >> 8) & 0xFFu);
    Out[at++] = '.';
    (void)Bc250PerfPutDecimal(Out, at, GcVersion & 0xFFu);
}

// A terminated copy of at most BC250_PERF_NAME_CHARS - 1 characters; the rest of the array is zero. NULL gives an
// empty name.
static __inline void Bc250PerfCopyName(unsigned short Out[BC250_PERF_NAME_CHARS], const unsigned short* Source)
{
    unsigned i, end = 0u;
    for (i = 0; i < BC250_PERF_NAME_CHARS; i++) Out[i] = 0;
    if (Source == 0) return;
    while (end < BC250_PERF_NAME_CHARS - 1u && Source[end] != 0) {
        Out[end] = Source[end];
        end++;
    }
}
