// monfence_packets.h - the PM4 the monitored-fence positive control puts into its indirect buffers, the fence
// values it writes, and its exit codes. Shared by monfence.c (target) and monfence_host_test.c (development PC).
//
// PROVENANCE: packet numbers and bit fields are Linux amdgpu nvd.h (MIT) as vendored in
// driver/amdgpu-import/nvd.h; CACHE_FLUSH_AND_INV_TS_EVENT is navi10_enum.h (MIT) in third_party/linux-amdgpu.
// They are restated under an MF_ prefix only because kmtprobe.c, which monfence.c reuses, already defines
// PACKET3 itself. monfence_host_test.c includes both originals and fails the build if any value here drifts.
//
// No register is written by anything in this file. Three packets are used and nothing else:
//   RELEASE_MEM  the fence write under test, bit for bit the KMD's own fence packet (driver/shim/bc250_gfx.c:1544-1561,
//                gfx_v10_0_ring_emit_fence upstream) with the data and interrupt selectors as parameters;
//   DMA_DATA     the E27 GPU readback (experiments/E27-m9-inference/gpu-residency-probe.c, direct memory, no L2,
//                CP_SYNC), used to read the fence page from the GPU side;
//   NOP          a type-3 NOP, for a submission that does nothing but complete.
#pragma once

#include <stdint.h>

// ---- packet headers (nvd.h:33, :48, :55, :354, :397) --------------------------------------------------------
#define MF_PACKET_TYPE3            3u
#define MF_PACKET3(op, n)          ((MF_PACKET_TYPE3 << 30) | (((uint32_t)(op) & 0xFFu) << 8) | \
                                    (((uint32_t)(n) & 0x3FFFu) << 16))
#define MF_PACKET3_NOP             0x10u
#define MF_PACKET3_RELEASE_MEM     0x49u
#define MF_PACKET3_DMA_DATA        0x50u
// The PACKET2 pad all three GFX10 CP ring types use (driver/shim/bc250_gfx.c:67-70, BC250_CP_NOP); build.ps1
// compares the two.
#define MF_CP_NOP                  0xFFFF1000u

// ---- RELEASE_MEM fields (nvd.h:355-387) ----------------------------------------------------------------------
#define MF_RM_EVENT_TYPE(x)        ((uint32_t)(x) << 0)
#define MF_RM_EVENT_INDEX(x)       ((uint32_t)(x) << 8)
#define MF_RM_GCR_GLM_WB           (1u << 12)
#define MF_RM_GCR_GLM_INV          (1u << 13)
#define MF_RM_GCR_GL2_WB           (1u << 21)
#define MF_RM_GCR_SEQ              (1u << 22)
#define MF_RM_CACHE_POLICY(x)      ((uint32_t)(x) << 25)      // 3 = bypass
#define MF_RM_DATA_SEL(x)          ((uint32_t)(x) << 29)      // 1 = low 32 bits, 2 = 64 bits
#define MF_RM_INT_SEL(x)           ((uint32_t)(x) << 24)      // 0 = none, 2 = interrupt after write confirm
#define MF_RM_DST_SEL(x)           ((uint32_t)(x) << 16)      // 0 = memory controller
// navi10_enum.h:14176, VGT_EVENT_TYPE.
#define MF_EVENT_CACHE_FLUSH_AND_INV_TS 0x14u
// The EOP event index the KMD's fence uses (bc250_gfx.c:1552).
#define MF_EVENT_INDEX_EOP         5u

#define MF_DATA_SEL_32             1u
#define MF_DATA_SEL_64             2u
// INT_SEL 3 is not named in nvd.h. Mesa's sid.h calls it EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM; RADV emits it
// in the EOP writes of its own IBs, and those ran at VMID 1 on unit A under our KMD (the IB dump
// "c0064900 0070f514 23000000" in evidence/windows/2026-09-23-E27-m9-inference/ops-gpu-003/suite.err:40).
// monfence uses it only as a second latency mode and as an optional --int-sel, never by default.
#define MF_INT_SEL_NONE            0u
#define MF_INT_SEL_IRQ_CONFIRM     2u
#define MF_INT_SEL_DATA_CONFIRM    3u

// ---- DMA_DATA fields (nvd.h:398-438) -------------------------------------------------------------------------
#define MF_DMA_DATA_CP_SYNC        (1u << 31)

// Dwords per IB slot. Every IB here is 8 dwords, the CP's pad granularity (bc250_gfx.c:67-68).
#define MF_IB_DWORDS               8u

// The second dword of the KMD's fence, which is also amdgpu's (gfx_v10_0_ring_emit_fence): flush and write back,
// event CACHE_FLUSH_AND_INV_TS at index 5, data written with cache policy bypass. 0x06603514.
static inline uint32_t mf_release_mem_dw1(void)
{
    return MF_RM_GCR_SEQ | MF_RM_GCR_GL2_WB | MF_RM_GCR_GLM_INV | MF_RM_GCR_GLM_WB | MF_RM_CACHE_POLICY(3) |
           MF_RM_EVENT_TYPE(MF_EVENT_CACHE_FLUSH_AND_INV_TS) | MF_RM_EVENT_INDEX(MF_EVENT_INDEX_EOP);
}

// RELEASE_MEM of `value` to `va`, 8 dwords, DST_SEL 0. Returns the dword count, or 0 (nothing written that the
// caller may submit) when the address is not aligned for the data size: bc250_gfx_emit_fence refuses the same.
static inline unsigned mf_emit_release_mem(uint32_t* dw, uint64_t va, uint64_t value, unsigned data_sel,
                                           unsigned int_sel)
{
    if (dw == 0 || va == 0 || data_sel < MF_DATA_SEL_32 || data_sel > MF_DATA_SEL_64 || int_sel > 3u) return 0;
    if ((va & (data_sel == MF_DATA_SEL_64 ? 7u : 3u)) != 0) return 0;
    if (va >> 48) return 0;                                    // a GFX10 VM address is 48 bits
    dw[0] = MF_PACKET3(MF_PACKET3_RELEASE_MEM, 6);
    dw[1] = mf_release_mem_dw1();
    dw[2] = MF_RM_DATA_SEL(data_sel) | MF_RM_INT_SEL(int_sel) | MF_RM_DST_SEL(0);
    dw[3] = (uint32_t)va;
    dw[4] = (uint32_t)(va >> 32);
    dw[5] = (uint32_t)value;
    dw[6] = (uint32_t)(value >> 32);
    dw[7] = 0;                                                 // interrupt context id, unused with INT_SEL 0/3
    return 8;
}

// The E27 readback: one DMA_DATA from `src` to `dst`, direct memory addresses (no L2), the ME waits for the copy
// (CP_SYNC), then one PACKET2 pad. 8 dwords.
static inline unsigned mf_emit_dma_copy(uint32_t* dw, uint64_t src, uint64_t dst, uint32_t bytes)
{
    if (dw == 0 || src == 0 || dst == 0 || bytes == 0 || (bytes & 3u) != 0 || ((src | dst) & 3u) != 0) return 0;
    dw[0] = MF_PACKET3(MF_PACKET3_DMA_DATA, 5);
    dw[1] = MF_DMA_DATA_CP_SYNC;
    dw[2] = (uint32_t)src;
    dw[3] = (uint32_t)(src >> 32);
    dw[4] = (uint32_t)dst;
    dw[5] = (uint32_t)(dst >> 32);
    dw[6] = bytes;
    dw[7] = MF_CP_NOP;
    return 8;
}

// One type-3 NOP covering 8 dwords (count = total - 2, gfx_v10_0.c:9505).
static inline unsigned mf_emit_nop(uint32_t* dw)
{
    unsigned i;
    if (dw == 0) return 0;
    dw[0] = MF_PACKET3(MF_PACKET3_NOP, MF_IB_DWORDS - 2u);
    for (i = 1; i < MF_IB_DWORDS; i++) dw[i] = 0;
    return 8;
}

// ---- fence values ----------------------------------------------------------------------------------------------
//
// Value `step` of fence `fence`. Both halves change at every step, so a torn 64-bit store would show up as a value
// that is neither the old nor the new one; the high half grows with the step, so the sequence is monotonic, as a
// monitored fence's must be. Different fences use disjoint ranges, so a value seen in the wrong fence names its
// owner. Kept far below 2^63. fence < 16, step < 256.
#define MF_FENCE_LIMIT 16u
#define MF_STEP_LIMIT  256u
static inline uint64_t mf_value(unsigned fence, unsigned step)
{
    uint32_t hi = 0x000BC000u + ((uint32_t)(fence & 0xFu) << 8) + (uint32_t)(step & 0xFFu);
    uint32_t lo = 0xA5000000u | ((uint32_t)(fence & 0xFu) << 16) | ((uint32_t)(step & 0xFFu) << 4) | 1u;
    return ((uint64_t)hi << 32) | lo;
}

// ---- exit codes ------------------------------------------------------------------------------------------------
//
// The process exits with the code of the FIRST failed check; every check prints its own line regardless.
// 1 is never used, so a crash or an unexpected return is not mistaken for a check.
#define MF_EXIT_PASS              0
#define MF_EXIT_ARGS              2    // bad command line
#define MF_EXIT_SETUP             3    // adapter, device, paging queue, allocation, context or fence creation failed
#define MF_EXIT_WATCHDOG          4    // the process watchdog fired (kmtprobe's convention)
#define MF_EXIT_NO_GPU_VA         5    // CreateSynchronizationObject2 returned no FenceValueGPUVirtualAddress or no CPU VA
#define MF_EXIT_PACKET_CONTROL    10   // RELEASE_MEM to our own allocation did not land: the packet, not the fence
#define MF_EXIT_READ_CONTROL      11   // the GPU did not read the CPU-signalled value at the fence GPU VA; no write tried
#define MF_EXIT_A_VALUE           12   // (a) the CPU mapping did not read N after the IB's DMA completion
#define MF_EXIT_B_WAKE            13   // (b) WaitForSynchronizationObjectFromCpu(N) did not wake in time (or woke early)
#define MF_EXIT_C_ORDER           14   // (c) work queued behind WaitForSynchronizationObjectFromGpu ran before the fence
#define MF_EXIT_C_RELEASE         15   // (c) work queued behind the GPU wait was not released in time
#define MF_EXIT_UNEXPECTED_VALUE  16   // a torn value, or a write that changed some other fence on the page
#define MF_EXIT_LATENCY           17   // an iteration of the latency series failed
#define MF_EXIT_MT_CONTROL        21   // (d) packet or read control failed in a thread
#define MF_EXIT_MT_A              22   // (d) (a) failed in a thread
#define MF_EXIT_MT_B              23   // (d) (b) failed in a thread
#define MF_EXIT_MT_C_ORDER        24   // (d) (c) ordering, including the cross-thread wait
#define MF_EXIT_MT_C_RELEASE      25   // (d) (c) release, including the cross-thread wait
#define MF_EXIT_MT_SYNC           26   // (d) a thread barrier timed out or a thread did not finish
#define MF_EXIT_MT_UNEXPECTED     27   // (d) torn value or a foreign fence changed
#define MF_EXIT_RESCUE            30   // queued GPU waits could not be released by a CPU signal (no other failure)
