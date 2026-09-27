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
