/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "gfx_copy.h"
#include "gfx_copy_fields.h"
#include "../amdgpu-import/nvd.h"

/* radv_cp_dma.c cp_dma_max_byte_count(GFX10): GFX9+ field,32-byte alignment. */
#define COPY_MAX (S_506_BYTE_COUNT(~0u) & ~31u)
#define COPY_DWORDS 7u

unsigned int Bc250GfxCopyDwords(unsigned int bytes)
{
    return bytes ? ((bytes / COPY_MAX) + (bytes % COPY_MAX != 0u)) * COPY_DWORDS : 0u;
}

unsigned int Bc250EmitGfxCopySpan(unsigned int* buffer, unsigned int capacity,
    unsigned long long source, unsigned long long destination, unsigned int bytes,
    int waitBefore, int syncAfter)
{
    unsigned int required = Bc250GfxCopyDwords(bytes), written = 0, remaining = bytes;
    if (!buffer || !required || capacity < required) return 0;
    if (source > ~0ull - (bytes - 1u) || destination > ~0ull - (bytes - 1u)) return 0;
    if (source <= destination ? destination - source < bytes : source - destination < bytes) return 0;
    while (remaining) {
        unsigned int count = remaining > COPY_MAX ? COPY_MAX : remaining;
        unsigned int control = S_501_DST_SEL(V_501_DST_ADDR_USING_L2) |
            S_501_SRC_SEL(V_501_SRC_ADDR_USING_L2);
        unsigned int command = S_506_BYTE_COUNT(count);
        if (count == remaining && syncAfter) control |= S_501_CP_SYNC(1);
        if (!written && waitBefore) command |= S_506_RAW_WAIT(1);
        /* radv_cs_emit_cp_dma's GFX7+ seven-dword layout, specialized to GFX10. */
        buffer[written++] = (unsigned int)PACKET3(PACKET3_DMA_DATA, 5);
        buffer[written++] = control;
        buffer[written++] = (unsigned int)source;
        buffer[written++] = (unsigned int)(source >> 32);
        buffer[written++] = (unsigned int)destination;
        buffer[written++] = (unsigned int)(destination >> 32);
        buffer[written++] = command;
        remaining -= count;
        if (remaining) { source += count; destination += count; }
    }
    return written;
}

unsigned int Bc250GfxCopyMaxBytes(void)
{
    return COPY_MAX;
}

unsigned int Bc250EmitGfxCopy(unsigned int* buffer, unsigned int capacity,
    unsigned long long source, unsigned long long destination, unsigned int bytes)
{
    return Bc250EmitGfxCopySpan(buffer, capacity, source, destination, bytes, 1, 1);
}

unsigned int Bc250GfxCopyNop(void)
{
    return (unsigned int)PACKET3(PACKET3_NOP, 0x3fff);
}

unsigned int Bc250EmitGfxAcquire(unsigned int* buffer, unsigned int capacity)
{
    if (!buffer || capacity<BC250_GFX_ACQUIRE_DWORDS) return 0;
    /* Same imported gfx_v10_0 sequence as shim/bc250_dispatch.c emit_mem_sync. */
    buffer[0]=(unsigned int)PACKET3(PACKET3_ACQUIRE_MEM,6);
    buffer[1]=0;
    buffer[2]=0xffffffffu;
    buffer[3]=0xffffffu;
    buffer[4]=buffer[5]=0;
    buffer[6]=0xau;
    buffer[7]=PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_INV(1) |
        PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_WB(1) |
        PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_INV(1) |
        PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_WB(1) |
        PACKET3_ACQUIRE_MEM_GCR_CNTL_GL1_INV(1) |
        PACKET3_ACQUIRE_MEM_GCR_CNTL_GLV_INV(1) |
        PACKET3_ACQUIRE_MEM_GCR_CNTL_GLK_INV(1) |
        PACKET3_ACQUIRE_MEM_GCR_CNTL_GLI_INV(1);
    return BC250_GFX_ACQUIRE_DWORDS;
}
