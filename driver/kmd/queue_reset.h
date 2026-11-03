/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Stage 2 of the hang recovery (docs/design/queue-reset.md): the pure decisions of the KMD half. The shim half,
 * which writes the KIQ packets, is bc250_gfx_reset_kcq/kgq (driver/shim/bc250_gfx.c). Nothing here touches the
 * hardware, so driver/kmd/test/queue_reset_test.c checks it on the host. */
#ifndef BC250_QUEUE_RESET_H
#define BC250_QUEUE_RESET_H

#include "wave_snapshot.h"
#include "gc_10_1_0_sh_mask.h"      /* SQ_WAVE_HW_ID2__VM_ID: AMD's field, not a typed mask */

/* The two steps of the KMD half that follow the shim's steps (enum bc250_qreset_step ends at
 * BC250_QRESET_STEP_DONE, 8). They go into LastSeq of the hang record with verdict QUEUE_RESET_FAILED. */
#define BC250_QRESET_STEP_WAVES     9u     /* the wave proof of section 5.4 step 7 failed */
#define BC250_QRESET_STEP_SEQUENCE 10u     /* the gfx sequence refused a register: the shim's steps are not proven */

/* Section 5.4 step 7. 0 when the capture is complete and holds no wave of Vmid; else BC250_QRESET_STEP_WAVES. A
 * capture that failed, ran out of budget or was truncated proves nothing, so it fails the proof. A wave of another
 * VMID does not: it belongs to another process, which the queue reset did not touch. */
static __inline unsigned Bc250QueueResetWaveProof(const BC250_WAVE_HEADER* Header, const BC250_WAVE_RECORD* Waves,
                                                  int CaptureOk, unsigned Vmid)
{
    unsigned i;
    if (!CaptureOk || Header == 0 || (Header->Flags & BC250_WAVE_COMPLETE) == 0 ||
        (Header->Flags & (BC250_WAVE_TRUNCATED | BC250_WAVE_READ_ERROR | BC250_WAVE_BUDGET_EXPIRED)) != 0 ||
        Header->WaveCount > BC250_WAVE_MAX_RECORDS)
        return BC250_QRESET_STEP_WAVES;
    if (Header->WaveCount != 0 && Waves == 0) return BC250_QRESET_STEP_WAVES;
    for (i = 0; i < Header->WaveCount; i++)
        if (((Waves[i].Raw[Bc250WaveHwId2] & SQ_WAVE_HW_ID2__VM_ID_MASK) >> SQ_WAVE_HW_ID2__VM_ID__SHIFT) == Vmid)
            return BC250_QRESET_STEP_WAVES;
    return 0;
}

/* The KIQ steps of the shim: a failure there means the KIQ itself did not answer, so the next stop must not trust
 * the engines (GpuStopUnconfirmed, section 7). The values are enum bc250_qreset_step of bc250_gfx.h. */
static __inline int Bc250QueueResetKiqStep(unsigned Step)
{
    return Step == 2u /* UNMAP */ || Step == 4u /* VMID_RESET */ || Step == 6u /* MAP */;
}

#endif
