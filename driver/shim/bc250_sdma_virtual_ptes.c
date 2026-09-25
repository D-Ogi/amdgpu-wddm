/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "bc250_sdma_virtual_ptes.h"
#include "bc250_sdma.h"

int bc250_sdma_build_virtual_ptes(struct amdgpu_device *adev, void *buffer,
    unsigned int capacity_bytes, u64 dma_va, u64 source_va, u64 destination_va,
    unsigned int entries, struct bc250_sdma_virtual_ptes *built)
{
    const struct amdgpu_ring_funcs *funcs = bc250_sdma_ring_funcs();
    struct bc250_sdma_virtual_ptes layout;
    unsigned int reserved, written = 0;
    u64 end, table_bytes;
    u32 *ib;
    int result;
    if (!built) return BC250_SDMA_PAGING_EINVAL;
    memset(built, 0, sizeof(*built));
    if (!adev || !buffer || !dma_va || (dma_va & 3u) ||
        !entries || entries > 512 || ((source_va | destination_va) & 7u))
        return BC250_SDMA_PAGING_EINVAL;
    table_bytes = (u64)entries * 8u;
    if (source_va > ~(u64)0 - table_bytes ||
        destination_va > ~(u64)0 - table_bytes || dma_va > ~(u64)0 - 12288u)
        return BC250_SDMA_PAGING_EINVAL;

    memset(&layout, 0, sizeof(layout));
    layout.csa_offset = (0u - (u32)dma_va) & 63u;
    layout.marker_offset = layout.csa_offset + 64u;
    layout.ib_offset = layout.marker_offset + 32u;
    /* Reserve the existing AMD emitter's complete two-copy/two-barrier unit.
     * Its ring allocation rounds to align_mask; INDIRECT length rounds to8 DW. */
    reserved = (2u * bc250_sdma_copy_linear_size((unsigned)table_bytes) + 20u +
                funcs->align_mask) & ~funcs->align_mask;
    layout.staging_offset = layout.ib_offset + reserved * 4u;
    layout.staging_offset += (0u - (u32)(dma_va + layout.staging_offset)) & 4095u;
    layout.bytes = layout.staging_offset + 4096u;
    end = dma_va + layout.bytes;
    if ((source_va < end && dma_va < source_va + table_bytes) ||
        (destination_va < end && dma_va < destination_va + table_bytes))
        return BC250_SDMA_PAGING_EINVAL;
    if (capacity_bytes < layout.bytes) {
        built->bytes = layout.bytes;
        return BC250_SDMA_PAGING_INSUFFICIENT;
    }

    /* Fresh per-span marker and staging: no shared scratch page or marker value
     * can be reused by a different outstanding companion-context transaction. */
    memset(buffer, 0, layout.bytes);
    ib = (u32 *)((unsigned char *)buffer + layout.ib_offset);
    result = bc250_sdma_paging_copy_ptes(adev, ib, reserved, source_va, destination_va,
        entries, dma_va + layout.staging_offset, dma_va + layout.marker_offset,
        1u, &written);
    if (result != BC250_SDMA_PAGING_OK) return result;
    while (written & 7u) ib[written++] = funcs->nop;
    layout.ib_dwords = written;
    *built = layout;
    return BC250_SDMA_PAGING_OK;
}
