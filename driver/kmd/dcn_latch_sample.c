#include "dcn_latch_sample.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"

long Bc250LatchSampleRead(BC250_DISP_IO* Io, BC250_LATCH_SAMPLE* Sample)
{
    static const unsigned long offsets[Bc250LatchWordCount] = {
        BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT,
        BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0,
        BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
        BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,
        BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0,
        BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT
    };
    const unsigned long lockMask =
        OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
        OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
    const unsigned long selectMask = OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK;
    const unsigned long highMask =
        HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK;
    BC250_LATCH_SAMPLE zero = {0};
    unsigned long i, value;
    long status;
    if (!Sample) return BC250_DISP_STATUS_INVALID;
    *Sample = zero;
    if (!Io || !Io->Read) return BC250_DISP_STATUS_INVALID;
    for (i = 0; i < Bc250LatchWordCount; ++i) {
        value = 0;
        status = Bc250DispRead(Io, offsets[i], &value);
        if (status < 0) return status;
        Sample->Word[i] = value;
        Sample->ValidMask |= 1ul << i;
    }
    // Pipe 0 only. Compare complete frame words conservatively; do not infer
    // elapsed frames from a counter whose wrap interval is not measured here.
    if (Sample->Word[Bc250LatchFrameBefore] != Sample->Word[Bc250LatchFrameAfter] ||
        (Sample->Word[Bc250LatchSelectBefore] & selectMask) != 0 ||
        (Sample->Word[Bc250LatchSelectAfter] & selectMask) != 0 ||
        (Sample->Word[Bc250LatchLockBefore] & lockMask) !=
            (Sample->Word[Bc250LatchLockAfter] & lockMask) ||
        (Sample->Word[Bc250LatchHighBefore] & highMask) !=
            (Sample->Word[Bc250LatchHighAfter] & highMask)) return BC250_DISP_STATUS_BUSY;
    Sample->BracketsMatch = 1;
    Sample->LockHeld = (Sample->Word[Bc250LatchLockBefore] & lockMask) == lockMask;
    Sample->LockClear = (Sample->Word[Bc250LatchLockBefore] & lockMask) == 0;
    Sample->Physical = ((unsigned long long)(Sample->Word[Bc250LatchHighBefore] & highMask) << 32) |
        Sample->Word[Bc250LatchLow];
    return BC250_DISP_STATUS_SUCCESS;
}
