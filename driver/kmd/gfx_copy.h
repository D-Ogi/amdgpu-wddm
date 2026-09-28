/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

/* GFX10 ME DMA_DATA copy, L2 path. No submission or allocation mapping.
 * Caller guarantees supported hardware/L2, residency, valid GPU addresses and
 * producer cache/shader ordering; then submits on its presenting GFX context.
 * CP_SYNC orders ME after the copy; it is not a WDDM completion notification.
 * Distinct, non-overlapping ranges only. Zero bytes is invalid, not an idle wait.
 * Returns the required dwords, or0 for invalid size. Emit returns0 on invalid
 * input/short capacity without writing the output; otherwise written dwords. */
unsigned int Bc250GfxCopyDwords(unsigned int Bytes);
unsigned int Bc250EmitGfxCopy(unsigned int* Buffer, unsigned int CapacityDwords,
    unsigned long long Source, unsigned long long Destination, unsigned int Bytes);

/* Compose a larger IB: WaitBefore orders earlier CP DMA and SyncAfter waits at
 * the end of this span. A caller batching spans supplies these only at IB edges. */
unsigned int Bc250GfxCopyMaxBytes(void);
unsigned int Bc250EmitGfxCopySpan(unsigned int* Buffer, unsigned int CapacityDwords,
    unsigned long long Source, unsigned long long Destination, unsigned int Bytes,
    int WaitBefore, int SyncAfter);

/* Single-word CP padding used for the GFX IB alignment tail. */
unsigned int Bc250GfxCopyNop(void);

/* Linux gfx_v10_0_emit_mem_sync sequence, eight DWORDs including header.
 * Makes prior producer writes visible to the L2 DMA read; it does not wait for
 * another queue or make an allocation resident. Caller supplies that ordering. */
#define BC250_GFX_ACQUIRE_DWORDS 8u
unsigned int Bc250EmitGfxAcquire(unsigned int* Buffer, unsigned int CapacityDwords);
