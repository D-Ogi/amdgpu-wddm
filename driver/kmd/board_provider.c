// Board identity gates platform accesses; chip discovery remains independent.
#include "bc250kmd.h"
#include "bc250_board_envelope.h"
#include "bc250_oc.h"

C_ASSERT(sizeof(BC250_ESCAPE_BOARD_CAPS) == 768);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_BOARD_CAPS, GpuPoints) == 152);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_BOARD_CAPS, FanPresetCounts) == 344);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_BOARD_CAPS, FanPresets) == 356);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_BOARD_CAPS, OcTier) == 556);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_BOARD_CAPS, OcTiers) == 576);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_BOARD_CAPS, OcPoints) == 624);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_BOARD_CAPS, OcCpuMinUvSteps) == 660);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_BOARD_CAPS, Reserved) == 684);
C_ASSERT(BC250_OC_TIER_COUNT == 3 && BC250_CLOCK_OC_LEVELS == 3);

void BoardProviderBind(BC250_DEVICE* device)
{
    BOOLEAN allowed = (BOOLEAN)bc250_board_supported((unsigned)InterlockedCompareExchange(
        &device->BoardMemoryProviderId, 0, 0));
    device->Hwmon.BoardAllowed = allowed;
    device->Smu.BoardAllowed = allowed;
}
void BoardProviderUnbind(BC250_DEVICE* device)
{
    // Only after every fan hand-back and mailbox teardown has completed.
    device->Hwmon.BoardAllowed = FALSE;
    device->Smu.BoardAllowed = FALSE;
    // A stopped device runs no overclock tier; the next start decides it again (oc_tier.c).
    InterlockedExchange(&device->Smu.OcTier, 0);
    device->OcTier = 0;
}
void BoardProviderCaps(BC250_DEVICE* device, BC250_ESCAPE_BOARD_CAPS* data, ULONG flags)
{
    BC250_ESCAPE_BOARD_CAPS request = *data, canonical = {0};
    D3DDDI_ESCAPEFLAGS wanted = {0};
    ULONG i, j;
    canonical.Magic = BC250_ESCAPE_MAGIC;
    canonical.Command = BC250_ESCAPE_RUN_BOARD_CAPS;
    canonical.Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    // ABI 1 readers (an older control application) get the ABI 1 reply: the overclock words stay zero.
    canonical.AbiVersion = request.AbiVersion == BC250_BOARD_CAPS_ABI_V1 ? BC250_BOARD_CAPS_ABI_V1 :
                                                                          BC250_BOARD_CAPS_ABI;
    *data = canonical;
    data->Status = BC250_ESCAPE_STATUS_REFUSED;
    data->NtStatus = (ULONG)STATUS_INVALID_PARAMETER;
    wanted.NoAdapterSynchronization = 1;
    if (flags != wanted.Value || RtlCompareMemory(&request, &canonical, sizeof(request)) != sizeof(request)) return;
    data->Status = BC250_ESCAPE_STATUS_DONE;
    data->NtStatus = STATUS_SUCCESS;
    data->Reason = BC250_BOARD_CAPS_REASON_BOARD;
    if (!bc250_board_supported((unsigned)InterlockedCompareExchange(&device->BoardMemoryProviderId, 0, 0))) return;
    data->ProviderId = BC250_BOARD_MEMORY_PROVIDER_BC250_ABL;
    data->Flags = BC250_BOARD_CAPS_SUPPORTED;
    data->Reason = BC250_BOARD_CAPS_REASON_INACTIVE;
    if (device->Started && !InterlockedCompareExchange(&device->RetainedPowerPhase, 0, 0)) {
        data->Flags |= BC250_BOARD_CAPS_MEMORY;
        // Board support is independent of module readiness and enable settings.
        // Existing feature status queries govern operations on an offline module.
        data->Flags |= BC250_BOARD_CAPS_FAN | BC250_BOARD_CAPS_GPU | BC250_BOARD_CAPS_CPU;
        data->Reason = BC250_BOARD_CAPS_REASON_NONE;
    }
    data->GpuMinMHz=BC250_CLOCK_MIN_MHZ; data->GpuFloorMHz=BC250_CLOCK_FLOOR_MHZ;
    data->GpuMaxMHz=BC250_CLOCK_CEILING_MHZ; data->GpuStepMHz=BC250_CLOCK_STEP_MHZ;
    data->GpuPointCount=BC250_CLOCK_LEVELS; data->GpuMinMv=BC250_CLOCK_FLOOR_MV;
    data->GpuMaxMv=BC250_CLOCK_CEILING_MV; data->GpuUndervoltMv=BC250_CURVE_UNDERVOLT_MV;
    data->GpuHotMc=BC250_CLOCK_HOT_MC;
    data->GpuDefaultMaxMHz=BC250_DPM_DEFAULT_MAX_MHZ; data->CpuStockCoreCount=BC250_BOARD_CPU_STOCK_CORES;
    data->CpuMinMHz=BC250_CPU_MIN_MHZ; data->CpuMaxMHz=BC250_CPU_MAX_MHZ;
    data->CpuLabMaxMHz=BC250_CPU_MAX_MHZ_LAB; data->CpuStepMHz=1;
    data->CpuUvMaxSteps=BC250_CPU_UV_MAX_STEPS; data->CpuTempMinC=BC250_CPU_TEMP_MIN_C;
    data->CpuTempMaxC=BC250_CPU_TEMP_MAX_C; data->CpuRefuseMv=BC250_CPU_REFUSE_MV;
    data->CpuStockMask=BC250_CPU_MASK_STOCK; data->CpuFullMask=BC250_CPU_MASK_FULL; data->CpuCoreCount=BC250_CPU_CORES;
    data->FanMinC=BC250_FAN_TEMP_MIN_C; data->FanMaxC=BC250_FAN_TEMP_MAX_C;
    data->FanFloorPct=BC250_FAN_FLOOR_PCT; data->FanFullPct=BC250_FAN_FULL_PCT;
    data->FanEmergencyMc=BC250_FAN_EMERGENCY_ON_MC;
    data->FanPointMin=BC250_FAN_POINTS_MIN; data->FanPointMax=BC250_FAN_POINTS_MAX;
    data->FanLeaseMinMs=BC250_FAN_LEASE_MIN_MS; data->FanLeaseMaxMs=BC250_FAN_LEASE_MAX_MS;
    data->FanLeaseDefaultMs=BC250_FAN_LEASE_DEFAULT_MS;
    for(i=0;i<BC250_CLOCK_LEVELS;++i) {
        data->GpuPoints[i].MHz=bc250_clock_points[i].mhz;
        data->GpuPoints[i].Mv=bc250_clock_points[i].mv;
        data->GpuPoints[i].Vid=bc250_clock_points[i].vid;
    }
    if (canonical.AbiVersion == BC250_BOARD_CAPS_ABI) {
        data->OcTier=device->OcTier; data->OcStoredTier=device->OcStoredTier;
        data->OcReason=device->OcReason; data->OcBoard=device->OcBoard;
        data->OcTierCount=BC250_OC_TIER_COUNT;
        for(i=0;i<BC250_OC_TIER_COUNT;++i) {
            struct bc250_oc_envelope e;
            (void)bc250_oc_envelope_of(i,&e);
            data->OcTiers[i].GpuMaxMHz=e.gpu_max_mhz; data->OcTiers[i].GpuMaxMv=e.gpu_max_mv;
            data->OcTiers[i].CpuMaxMHz=e.cpu_max_mhz; data->OcTiers[i].CpuUvMaxSteps=e.cpu_uv_max_steps;
        }
        for(i=0;i<BC250_CLOCK_OC_LEVELS;++i) {
            data->OcPoints[i].MHz=bc250_clock_points[BC250_CLOCK_LEVELS+i].mhz;
            data->OcPoints[i].Mv=bc250_clock_points[BC250_CLOCK_LEVELS+i].mv;
            data->OcPoints[i].Vid=bc250_clock_points[BC250_CLOCK_LEVELS+i].vid;
        }
        for(i=0;i<ARRAYSIZE(data->OcCpuMinUvSteps);++i)
            data->OcCpuMinUvSteps[i]=bc250_oc_cpu_min_uv_steps(BC250_OC_TIER_AIO,3600u+100u*i);
    }
    for(i=0;i<3;++i) {
        struct bc250_fan_curve curve;
        if (bc250_fan_profile_curve(i+1,&curve) != BC250_FAN_ERROR_OK) continue;
        data->FanPresetCounts[i]=curve.points;
        for(j=0;j<curve.points;++j) {
            data->FanPresets[i][j].TempC=curve.p[j].c;
            data->FanPresets[i][j].DutyPct=curve.p[j].pct;
        }
    }
}
