/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* PROVENANCE: Linux amdgpu gfx_v10_0.c, v6.18 7d0a66e4bb90, AMD MIT;
 * UMR scan_waves.c, a3d8666f936144ef497d93ea366070464b3a4d5b, MIT.
 * The leaf read, register order and selector are the upstream GFX10 methods.
 * Deviation: bounded live enumeration, backend failure checks and identity
 * rereads replace debugfs's caller-selected single-wave request. */
#include "bc250_wave.h"
#include "bc250_cu_mode.h"
#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "soc15_common.h"

#define WAVE_SES 2u
#define WAVE_SAS 2u
#define WAVE_WGPS 5u
#define WAVE_SIMDS 4u
/* UMR's GC10.1 bound, also LLVM AMDGPUBaseInfo::getMaxWavesPerEU.
 * GC10.3 has 16, but is not admitted by this reader. */
#define WAVE_SLOTS 20u

static const u32 wave_registers[Bc250WaveRegisterCount] = {
    ixSQ_WAVE_STATUS, ixSQ_WAVE_PC_LO, ixSQ_WAVE_PC_HI,
    ixSQ_WAVE_EXEC_LO, ixSQ_WAVE_EXEC_HI, ixSQ_WAVE_HW_ID1,
    ixSQ_WAVE_HW_ID2, ixSQ_WAVE_INST_DW0, ixSQ_WAVE_GPR_ALLOC,
    ixSQ_WAVE_LDS_ALLOC, ixSQ_WAVE_TRAPSTS, ixSQ_WAVE_IB_STS,
    ixSQ_WAVE_IB_STS2, ixSQ_WAVE_IB_DBG1, ixSQ_WAVE_M0, ixSQ_WAVE_MODE
};

unsigned int bc250_gfx_wave_broadcast_value(void)
{
    return GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK |
           GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK |
           GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK;
}

static void select_wave_bank(struct amdgpu_device *adev, u32 se, u32 sh, u32 instance)
{
    u32 data;
    if (instance == 0xffffffffu)
        data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES, 1);
    else
        data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_INDEX, instance);
    if (se == 0xffffffffu)
        data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_BROADCAST_WRITES, 1);
    else
        data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_INDEX, se);
    if (sh == 0xffffffffu)
        data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_BROADCAST_WRITES, 1);
    else
        data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_INDEX, sh);
    WREG32_SOC15(GC, 0, mmGRBM_GFX_INDEX, data);
}

static int stopped(BC250_WAVE_HEADER *header, bc250_wave_stop_fn stop, void *context)
{
    unsigned int reason = stop(context);
    if (!reason) return 0;
    /* An unknown stop reason is still failure, never a successful empty read. */
    if (reason & ~(BC250_WAVE_READ_ERROR | BC250_WAVE_BUDGET_EXPIRED))
        reason = BC250_WAVE_READ_ERROR;
    header->Flags |= reason;
    return 1;
}

static int wave_read(struct amdgpu_device *adev, BC250_WAVE_HEADER *header,
                     bc250_wave_stop_fn stop, void *context,
                     u32 wave, u32 address, u32 *value)
{
    if (stopped(header, stop, context)) return -1;
    /* gfx_v10_0.c:4477 wave_read_ind. SIMD is encoded in INSTANCE, not here. */
    WREG32_SOC15(GC, 0, mmSQ_IND_INDEX,
        (wave << SQ_IND_INDEX__WAVE_ID__SHIFT) |
        (address << SQ_IND_INDEX__INDEX__SHIFT));
    if (stopped(header, stop, context)) return -1;
    *value = RREG32_SOC15(GC, 0, mmSQ_IND_DATA);
    return stopped(header, stop, context) ? -1 : 0;
}

static int invalid_read(BC250_WAVE_HEADER *header)
{
    header->InvalidReads++;
    header->Flags |= BC250_WAVE_READ_ERROR;
    return -1;
}

int bc250_gfx_read_waves(struct amdgpu_device *adev, BC250_WAVE_SNAPSHOT *snapshot,
                        bc250_wave_stop_fn stop, void *context)
{
    BC250_WAVE_HEADER *h;
    u32 se, sh, wgp, simd, slot, cc, user, status, field;
    int result = -1;
    if (!snapshot) return -1;
    h = &snapshot->Header;
    h->Flags = h->WaveCount = h->SlotsVisited = h->WgpVisited = h->InvalidReads = 0;
    h->SeCount = h->ShPerSe = h->WgpPerSh = h->SimdPerWgp = h->WaveSlotsPerSimd = 0;
    memset(h->WgpMask, 0, sizeof(h->WgpMask));
    if (!adev || !stop || amdgpu_ip_version(adev, GC_HWIP, 0) != IP_VERSION(10, 1, 3) ||
        adev->gfx.config.max_shader_engines != WAVE_SES ||
        adev->gfx.config.max_sh_per_se != WAVE_SAS) {
        h->Flags = BC250_WAVE_READ_ERROR;
        return -1; /* No register access on another generation or unknown topology. */
    }
    h->SeCount = WAVE_SES;
    h->ShPerSe = WAVE_SAS;
    h->WgpPerSh = WAVE_WGPS;
    h->SimdPerWgp = WAVE_SIMDS;
    h->WaveSlotsPerSimd = WAVE_SLOTS;
    for (se = 0; se < WAVE_SES; se++) {
        for (sh = 0; sh < WAVE_SAS; sh++) {
            if (stopped(h, stop, context)) goto cleanup;
            select_wave_bank(adev, se, sh, 0xffffffffu);
            if (stopped(h, stop, context)) goto cleanup;
            cc = RREG32_SOC15(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG);
            if (stopped(h, stop, context)) goto cleanup;
            user = RREG32_SOC15(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG);
            if (stopped(h, stop, context)) goto cleanup;
            if (cc == 0xffffffffu || user == 0xffffffffu) {
                (void)invalid_read(h);
                goto cleanup;
            }
            h->WgpMask[se][sh] = bc250_cu_active_wgps_cc(cc, user, WAVE_WGPS);
            for (wgp = 0; wgp < WAVE_WGPS; wgp++) {
                if (!(h->WgpMask[se][sh] & (1u << wgp))) continue;
                h->WgpVisited++;
                for (simd = 0; simd < WAVE_SIMDS; simd++) {
                    if (stopped(h, stop, context)) goto cleanup;
                    select_wave_bank(adev, se, sh, (wgp << 2) | simd);
                    if (stopped(h, stop, context)) goto cleanup;
                    for (slot = 0; slot < WAVE_SLOTS; slot++) {
                        BC250_WAVE_RECORD *record;
                        if (wave_read(adev, h, stop, context, slot, ixSQ_WAVE_STATUS, &status))
                            goto cleanup;
                        h->SlotsVisited++;
                        if (status == 0xffffffffu) {
                            (void)invalid_read(h);
                            goto cleanup;
                        }
                        if (!REG_GET_FIELD(status, SQ_WAVE_STATUS, VALID)) continue;
                        if (h->WaveCount == BC250_WAVE_MAX_RECORDS) {
                            h->Flags |= BC250_WAVE_TRUNCATED;
                            goto cleanup;
                        }
                        record = &snapshot->Waves[h->WaveCount];
                        memset(record, 0, sizeof(*record));
                        record->Se = se; record->Sh = sh; record->Wgp = wgp;
                        record->Simd = simd; record->Slot = slot;
                        record->Raw[Bc250WaveStatus] = status;
                        for (field = 1; field < Bc250WaveRegisterCount; field++)
                            if (wave_read(adev, h, stop, context, slot, wave_registers[field],
                                          &record->Raw[field])) goto cleanup;
                        if (wave_read(adev, h, stop, context, slot, ixSQ_WAVE_HW_ID1, &record->HwId1After) ||
                            wave_read(adev, h, stop, context, slot, ixSQ_WAVE_HW_ID2, &record->HwId2After) ||
                            wave_read(adev, h, stop, context, slot, ixSQ_WAVE_STATUS, &record->StatusAfter))
                            goto cleanup;
                        if (record->StatusAfter == 0xffffffffu ||
                            record->Raw[Bc250WaveHwId1] == 0xffffffffu ||
                            record->Raw[Bc250WaveHwId2] == 0xffffffffu ||
                            record->HwId1After == 0xffffffffu || record->HwId2After == 0xffffffffu) {
                            (void)invalid_read(h);
                            goto cleanup;
                        }
                        /* Preserve an observed retirement or identity change. These raw
                         * rereads diagnose a live race; they do not make the read atomic. */
                        h->WaveCount++;
                    }
                }
            }
        }
    }
    /* All-zero topology is not a positive idle control: no wave slot was
     * observed. Keep the masks so the operator can distinguish that refusal. */
    if (!h->WgpVisited) {
        h->Flags |= BC250_WAVE_READ_ERROR;
        goto cleanup;
    }
    result = 0;
cleanup:
    select_wave_bank(adev, 0xffffffffu, 0xffffffffu, 0xffffffffu);
    if (stopped(h, stop, context)) result = -1;
    if (!result) h->Flags |= BC250_WAVE_COMPLETE;
    return result;
}
