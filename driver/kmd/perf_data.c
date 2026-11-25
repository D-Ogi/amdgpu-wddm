// DxgkDdiQueryAdapterInfo for the performance data types (BD-404): DXGKQAITYPE_NODEPERFDATA, ADAPTERPERFDATA,
// ADAPTERPERFDATA_CAPS and GPUVERSION. The contract, the input words and the rule for a field without a source are in
// perf_data.h; this file collects the sources and writes the WDK structures.
//
// Every source is a copy that another owner published: the DPM governor's snapshot (the SMU's clock readback and
// the THM temperature it throttles on), the SMU metrics table (the GFX rail voltage), the board monitor's sample
// (the fan) and the VBIOS part number of this start (hwinfo.c). This DDI sends no SMU message and touches no port,
// like DXGKQAITYPE_UMDRIVERPRIVATE. The limits (clock table, overclock tier, thermal rules) are BC-250 board facts
// and are given only for a positively identified board; any other AMD part gets its readings, if an owner publishes
// them, and zero limits.
#include "bc250kmd.h"
#include "asic_ip.h"
#include "bc250_oc.h"
#include "perf_data.h"

// The sizes dxgkrnl 26100.9974 passes as OutputDataSize (perf_data.h). d3dkmdt.h packs these structures to 1 byte.
C_ASSERT(sizeof(DXGK_NODE_PERFDATA) == 0x2C);
C_ASSERT(sizeof(DXGK_ADAPTER_PERFDATA) == 0x35);
C_ASSERT(sizeof(DXGK_ADAPTER_PERFDATACAPS) == 0x1C);
C_ASSERT(sizeof(DXGK_GPUVERSION) == 0x80);
C_ASSERT(DXGK_MAX_GPUVERSION_NAME_LENGTH == BC250_PERF_NAME_CHARS);
C_ASSERT(sizeof(WCHAR) == sizeof(unsigned short));
C_ASSERT(RTL_FIELD_SIZE(BC250_DEVICE, VbiosPartNumber) == RTL_FIELD_SIZE(DXGK_GPUVERSION, BiosVersion));

static NTSTATUS PerfInputStatus(int Verdict)
{
    return Verdict == BC250_PERF_INPUT_OK ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static void PerfSources(BC250_DEVICE* Device, BC250_PERF_SOURCES* S)
{
    BC250_DPM_SNAP dpm;
    BC250_DPM_METRICS metrics;
    BC250_HWMON_SNAP hwmon;
    struct bc250_oc_envelope stock, tier;
    unsigned rpm = 0u;

    RtlZeroMemory(S, sizeof(*S));
    // The governor's flags say what its last tick read. A paused governor (a power transition) reads nothing, so its
    // flags are from before the pause and do not count.
    DpmReadSnapshot(Device, &dpm);
    if ((dpm.Flags & BC250_DPM_FLAG_RUNNING) != 0 && (dpm.Flags & BC250_DPM_FLAG_PAUSED) == 0) {
        if ((dpm.Flags & BC250_DPM_FLAG_CLOCK) != 0) {
            S->GfxClockValid = 1;
            S->GfxMHz = dpm.ObservedMHz;
        }
        if ((dpm.Flags & BC250_DPM_FLAG_TEMPERATURE) != 0) {
            S->TemperatureValid = 1;
            S->TemperatureMc = dpm.TemperatureMc;
        }
    }
    // ObservedVid is a VID code, not millivolts; the metrics table has the rail in mV. TRUE: a fresh table.
    if (SmuMetricsFill(Device, &metrics)) {
        S->GfxVoltageValid = 1;
        S->GfxMv = (unsigned)metrics.GfxMv;
    }
    if (HwmonReadFresh(Device, &hwmon) &&
        Bc250PerfMainFan((const unsigned*)hwmon.Rpm, BC250_HWMON_FAN_MAX, hwmon.RpmValidMask, hwmon.FanPresentMask,
                         &rpm)) {
        S->FanValid = 1;
        S->FanRpm = rpm;
    }
    S->PoweredOn = InterlockedCompareExchange(&Device->RetainedPowerPhase, 0, 0) == BC250_POWER_ACTIVE;
    if (bc250_board_supported((unsigned)InterlockedCompareExchange(&Device->BoardMemoryProviderId, 0, 0))) {
        // The qualified table's top, and the top of the tier this start latched (oc_tier.c; the table's top for none).
        (void)bc250_oc_envelope_of(BC250_OC_TIER_NONE, &stock);
        (void)bc250_oc_envelope_of(Device->OcTier, &tier);
        S->EnvelopeValid = 1;
        S->GfxMaxMHz = stock.gpu_max_mhz;
        S->GfxMaxMv = stock.gpu_max_mv;
        S->GfxMaxOcMHz = tier.gpu_max_mhz;
        S->GfxMaxOcMv = tier.gpu_max_mv;
        // The governor's thermal cap steps the clock down from the hot cap and forces the thermal floor at the
        // critical point, both on the reading above (bc250_board_envelope.h, docs/hardware.md).
        S->TemperatureWarningMc = BC250_DPM_HOT_MC;
        S->TemperatureMaxMc = BC250_DPM_CRITICAL_MC;
    }
}

static NTSTATUS PerfNode(BC250_DEVICE* Device, const DXGKARG_QUERYADAPTERINFO* Query, ULONG NodeCount,
                         ULONG GfxNodeMask)
{
    DXGK_NODE_PERFDATA* out = (DXGK_NODE_PERFDATA*)Query->pOutputData;
    BC250_PERF_SOURCES sources;
    BC250_PERF_NODE node;
    unsigned ordinal = 0u;
    NTSTATUS status = PerfInputStatus(Bc250PerfNodeInput(Query->pInputData, Query->InputDataSize, NodeCount, &ordinal));

    if (!NT_SUCCESS(status)) return status;
    if (out == NULL || Query->OutputDataSize < sizeof(*out)) return STATUS_BUFFER_TOO_SMALL;
    PerfSources(Device, &sources);
    Bc250PerfFillNode(&sources, ordinal < 32u && (GfxNodeMask & (1u << ordinal)) != 0u, &node);
    RtlZeroMemory(out, Query->OutputDataSize);
    out->Frequency = node.Frequency;
    out->MaxFrequency = node.MaxFrequency;
    out->MaxFrequencyOC = node.MaxFrequencyOC;
    out->Voltage = node.Voltage;
    out->VoltageMax = node.VoltageMax;
    out->VoltageMaxOC = node.VoltageMaxOC;
    out->MaxTransitionLatency = node.MaxTransitionLatency;
    return STATUS_SUCCESS;
}

static NTSTATUS PerfAdapter(BC250_DEVICE* Device, const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_ADAPTER_PERFDATA* out = (DXGK_ADAPTER_PERFDATA*)Query->pOutputData;
    BC250_PERF_SOURCES sources;
    BC250_PERF_ADAPTER adapter;
    NTSTATUS status = PerfInputStatus(Bc250PerfAdapterInput(Query->pInputData, Query->InputDataSize));

    if (!NT_SUCCESS(status)) return status;
    if (out == NULL || Query->OutputDataSize < sizeof(*out)) return STATUS_BUFFER_TOO_SMALL;
    PerfSources(Device, &sources);
    Bc250PerfFillAdapter(&sources, &adapter);
    RtlZeroMemory(out, Query->OutputDataSize);
    out->MemoryFrequency = adapter.MemoryFrequency;
    out->MaxMemoryFrequency = adapter.MaxMemoryFrequency;
    out->MaxMemoryFrequencyOC = adapter.MaxMemoryFrequencyOC;
    out->MemoryBandwidth = adapter.MemoryBandwidth;
    out->PCIEBandwidth = adapter.PCIEBandwidth;
    out->FanRPM = adapter.FanRPM;
    out->Power = adapter.Power;
    out->Temperature = adapter.Temperature;
    out->PowerStateOverride = adapter.PowerStateOverride;
    return STATUS_SUCCESS;
}

static NTSTATUS PerfCaps(BC250_DEVICE* Device, const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_ADAPTER_PERFDATACAPS* out = (DXGK_ADAPTER_PERFDATACAPS*)Query->pOutputData;
    BC250_PERF_SOURCES sources;
    BC250_PERF_CAPS caps;
    NTSTATUS status = PerfInputStatus(Bc250PerfAdapterInput(Query->pInputData, Query->InputDataSize));

    if (!NT_SUCCESS(status)) return status;
    if (out == NULL || Query->OutputDataSize < sizeof(*out)) return STATUS_BUFFER_TOO_SMALL;
    PerfSources(Device, &sources);
    Bc250PerfFillCaps(&sources, &caps);
    RtlZeroMemory(out, Query->OutputDataSize);
    out->MaxMemoryBandwidth = caps.MaxMemoryBandwidth;
    out->MaxPCIEBandwidth = caps.MaxPCIEBandwidth;
    out->MaxFanRPM = caps.MaxFanRPM;
    out->TemperatureMax = caps.TemperatureMax;
    out->TemperatureWarning = caps.TemperatureWarning;
    return STATUS_SUCCESS;
}

// BiosVersion: the VBIOS part number, which is what amdgpu reports as vbios_version. GpuArchitecture: the graphics
// IP version of the part's row in asic_ip.h, empty without a row.
static NTSTATUS PerfGpuVersion(BC250_DEVICE* Device, const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_GPUVERSION* out = (DXGK_GPUVERSION*)Query->pOutputData;
    BC250_ASIC_IP ip;
    NTSTATUS status = PerfInputStatus(Bc250PerfAdapterInput(Query->pInputData, Query->InputDataSize));

    if (!NT_SUCCESS(status)) return status;
    if (out == NULL || Query->OutputDataSize < sizeof(*out)) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(out, Query->OutputDataSize);
    Bc250PerfCopyName((unsigned short*)out->BiosVersion, (const unsigned short*)Device->VbiosPartNumber);
    RtlZeroMemory(&ip, sizeof(ip));         // the lookup writes only a row it found
    (void)Bc250AsicIpLookup(BC250_PCI_VENDOR(Device->PciId), BC250_PCI_DEVICE(Device->PciId), &ip);
    Bc250PerfArchitecture(ip.Gc, (unsigned short*)out->GpuArchitecture);
    return STATUS_SUCCESS;
}

NTSTATUS PerfDataQuery(BC250_DEVICE* Device, const DXGKARG_QUERYADAPTERINFO* Query, ULONG NodeCount, ULONG GfxNodeMask)
{
    // Four of the enumeration's values; wddm.c routes only these here, so no switch over the whole enumeration.
    if (Query->Type == DXGKQAITYPE_NODEPERFDATA) return PerfNode(Device, Query, NodeCount, GfxNodeMask);
    if (Query->Type == DXGKQAITYPE_ADAPTERPERFDATA) return PerfAdapter(Device, Query);
    if (Query->Type == DXGKQAITYPE_ADAPTERPERFDATA_CAPS) return PerfCaps(Device, Query);
    if (Query->Type == DXGKQAITYPE_GPUVERSION) return PerfGpuVersion(Device, Query);
    return STATUS_NOT_SUPPORTED;
}
