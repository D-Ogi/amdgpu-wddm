/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_SDMA_PAGING_H
#define BC250_SDMA_PAGING_H

#include "amdgpu.h"

/*
 * ADR 0008 stage D / ADR 0013 (docs/design/paging-node.md section 4a): the portable half of
 * BuildPagingBuffer's DXGK_OPERATION_VIRTUAL_TRANSFER/VIRTUAL_FILL packet building. Physical (MC)
 * addresses in, SDMA0 packets out - no DXGK type, no VidMmTranslate() and no hSystemContext lookup
 * in reach of this file, which is what makes it host-testable the way bc250_sdma_copy.c already is
 * (driver/shim/test/sdma_copy_packets.c, M95). GfxPagingBuild() (driver/kmd/gfx.c) resolves the
 * operation's virtual addresses with VidMmTranslate() first, then calls one of these two with the
 * result.
 *
 * Both wrap a throwaway struct amdgpu_ring around the caller's buffer - the same trick
 * sdma_copy_packets.c's ring_init() uses - and call amdgpu_ring_alloc() for the room check before
 * the existing, measured bc250_sdma_emit_copy_linear()/emit_fill() write the packet: no packet
 * layout is duplicated here, only the "does it fit" decision. amdgpu_ring_commit() is never called:
 * the buffer is dxgkrnl's own paging buffer (or its shadow copy, design note section 4a), not the
 * live SDMA0 ring, and there is no doorbell to ring on it.
 *
 * adev is the caller's live device, and it is a parameter rather than a local for one measured
 * reason (facts M104): the throwaway ring needs a non-NULL ring->adev (bc250_sdma_copy.c refuses
 * without one, bc250_ring.c logs through it), and struct amdgpu_device is 0x5B00 bytes - a copy on
 * the stack overflows a 24 KB kernel stack the moment dxgmms2's own frames are already on it, which
 * is bugcheck 0x50 in nt!_chkstk and is what E24 run 004 crashed on. The ring only ever reads
 * through this pointer here; nothing in this file writes to the device.
 */

enum
{
	BC250_SDMA_PAGING_OK = 0,		/* built: *DwordsWritten dwords now sit at buffer[0] */
	BC250_SDMA_PAGING_INSUFFICIENT = 1,	/* nothing written; *DwordsWritten is what the operation needs */
	BC250_SDMA_PAGING_EINVAL = 2,		/* bad argument; nothing written */
};

/* Buffer: the first dword of the room BuildPagingBuffer has left to write into (dxgkrnl's
 * pDmaBuffer, or the driver's shadow copy at the same offset) - always written starting at
 * buffer[0], never at an internal offset; the caller advances its own pointer by *DwordsWritten.
 * buffer_dwords: how much room is there. An operation is answered whole or not at all: on
 * BC250_SDMA_PAGING_INSUFFICIENT nothing is written and *DwordsWritten carries the (SDMA-aligned)
 * dword count the operation needs. These single-range emitters do not compute MultipassOffset:
 * the page-wise caller tracks cumulative DATA bytes separately from the COMMAND byte offset
 * DmaBufferWriteOffset. Adding these two coordinate spaces would lose transfer progress. */
int bc250_sdma_paging_copy(struct amdgpu_device *adev, u32 *buffer, unsigned int buffer_dwords,
                           u64 src_mc, u64 dst_mc, unsigned int bytes, unsigned int *dwords_written);
int bc250_sdma_paging_fill(struct amdgpu_device *adev, u32 *buffer, unsigned int buffer_dwords,
                           u64 dst_mc, u32 pattern, unsigned int bytes, unsigned int *dwords_written);

/* Explicit page-entry values for scattered system pages or zero entries for unmap.
 * table_mc is an aligned GPU MC address of the destination table, not a CPU pointer.
 * Pure command construction. Caller owns mapping lifetime and GPU TLB ordering. */
int bc250_sdma_paging_write_ptes(struct amdgpu_device *adev, u32 *buffer,
                                unsigned int buffer_dwords, u64 table_mc,
                                const u64 *ptes, unsigned int count,
                                unsigned int *dwords_written);

/* GFXHUB only, VMID0..15. Uses the same reserved SDMA0 invalidate engine.
 * Does not choose a process/VMID, change a root, or synchronize other engines.
 * Caller must keep the target VMID binding valid through execution/completion. */
int bc250_sdma_paging_invalidate_vmid(struct amdgpu_device *adev, u32 *buffer,
                                     unsigned int buffer_dwords, unsigned int vmid,
                                     unsigned int *dwords_written);
/* VMID0 GART invalidate on GFXHUB engine0, reserved for SDMA0 paging.
 * Uses initialized AMD hub offsets/request encoder. No root change or CPU MMIO.
 * Submit only within an ordered PTE-write/copy/unmap sequence, not independently. */
int bc250_sdma_paging_invalidate_gart(struct amdgpu_device *adev, u32 *buffer,
                                     unsigned int buffer_dwords, unsigned int *dwords_written);

struct bc250_sdma_paging_mapping {
    u64 table_mc;             /* caller-reserved contiguous PTE slots */
    const u64 *ptes;          /* arbitrary page values, not CPU pointers on GPU */
    unsigned int page_count;
    u64 scratch_mc;           /* retained marker slot, outside mapping window */
    u32 first_sequence;       /* three fresh consecutive values, no wrap */
    u64 src_mc, dst_mc;       /* caller-resolved MC/window addresses */
    unsigned int bytes;
    unsigned int fill;
    u32 pattern;
};
/* All-or-nothing map/sync/invalidate/copy/sync/unmap/sync/invalidate construction.
 * Caller owns window bounds, page pinning, scratch freshness and final OS fence.
 * No hardware submission, OS completion or resource reservation is performed here. */
int bc250_sdma_paging_mapped_transfer(struct amdgpu_device *adev, u32 *buffer,
                                 unsigned int buffer_dwords,
                                 const struct bc250_sdma_paging_mapping *map,
                                 unsigned int *dwords_written);

#endif /* BC250_SDMA_PAGING_H */
