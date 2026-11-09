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

/* The SDMA0 queue reset of DxgkDdiResetFromTimeout (queue-reset.md section 13, node 1). Its steps go into the same
 * hang record field as the KIQ steps above, in their own range so that a log line cannot mix them up:
 * 0x10 + enum bc250_sdma_reset_stage of bc250_sdma.h for a failure inside bc250_sdma_reset_retained_instance, then
 * the KMD's own steps. */
#define BC250_SDMA_QRESET_STEP_BASE      0x10u
#define BC250_SDMA_QRESET_STEP_RING_TEST 0x17u /* the restored ring did not execute one fence write */
#define BC250_SDMA_QRESET_STEP_SEQUENCE  0x18u /* the gfx sequence refused a register: the shim's steps are not proven */
#define BC250_SDMA_QRESET_STEP_ARGS      0x19u /* no paging ring on this adapter, or a wrong IRQL: nothing changed */
#define BC250_SDMA_QRESET_STEP_DONE      0x1Fu

/* 1 when a failed SDMA queue reset may have left the engine in a state that nothing proves stopped. Stage 0 (NONE)
 * is the shim's argument check, before any register access. Every later stage has touched the engine: STOP has
 * written FREEZE and HALT, PULSE and later have reset it. A failed ring test leaves an engine that runs but did not
 * answer. The caller then sets GpuStopUnconfirmed (section 7), so that the next stop does not trust it either. */
static __inline int Bc250SdmaQueueResetUnconfirmed(unsigned Step)
{
    return Step > BC250_SDMA_QRESET_STEP_BASE && Step != BC250_SDMA_QRESET_STEP_ARGS &&
           Step != BC250_SDMA_QRESET_STEP_DONE;
}

#endif
