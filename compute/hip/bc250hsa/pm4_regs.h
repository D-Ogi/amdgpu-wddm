/* pm4_regs.h - the PM4 packet numbers, bit fields and register packet offsets of the
 * gfx1013 compute dispatch of section 3.7 of docs/design/m16-hip-route-b.md.
 *
 * PROVENANCE: every number is Linux amdgpu, MIT, as vendored in this repository:
 *   packet numbers and bit fields   driver/amdgpu-import/nvd.h
 *   VGT event types                 third_party/linux-amdgpu/navi10_enum.h
 *   register dword numbers          third_party/linux-amdgpu/gc_10_1_0_offset.h
 *   GC segment bases                third_party/linux-amdgpu/cyan_skillfish_ip_offset.h
 *
 * They are restated under a BC250HSA_ prefix because this library is a user-mode
 * component that must not pull the kernel headers into every translation unit. The
 * restatement is not trusted: tests/host/test_pm4.c includes all four originals and
 * fails the build when any value here drifts (repo rules 1 and 2, the form of
 * tools/win/monfence/monfence_packets.h).
 *
 * A SET_SH_REG names a register by its offset from the packet base. For the compute
 * block of this part the arithmetic is
 *
 *     offset = SOC15_REG_OFFSET(GC, 0, mm<name>) - PACKET3_SET_SH_REG_START
 *            = (GC_BASE__INST0_SEG0 + mm<name>) - 0x2C00
 *            = mm<name> - 0x19A0            (GC_BASE__INST0_SEG0 is 0x1260)
 *
 * and for the one uconfig register, whose BASE_IDX is 1,
 *
 *     offset = (GC_BASE__INST0_SEG1 + mm<name>) - 0xC000 = mm<name> - 0x2000.
 */
#ifndef BC250HSA_PM4_REGS_H
#define BC250HSA_PM4_REGS_H

#include <stdint.h>

/* ---- packet headers (nvd.h:33, :48) ------------------------------------------ */
#define BC250HSA_PACKET_TYPE3 3u
#define BC250HSA_PACKET_TYPE2 2u
#define BC250HSA_PACKET3(op, n)                                                  \
    (((uint32_t)BC250HSA_PACKET_TYPE3 << 30) | (((uint32_t)(op) & 0xFFu) << 8) | \
     (((uint32_t)(n) & 0x3FFFu) << 16))
/* Bit 1 of a type-3 header is the shader-type bit: it tells the command processor
 * that the SET_SH_REG writes which follow name the COMPUTE register space and not
 * the graphics one. Three packet kinds of this sequence carry it, and no other:
 * SET_SH_REG, SET_SH_REG_INDEX and DISPATCH_DIRECT. CONTEXT_CONTROL, ACQUIRE_MEM,
 * SET_UCONFIG_REG, EVENT_WRITE and RELEASE_MEM use the plain header. That is what
 * driver/shim/bc250_dispatch.c emits: BC250_PACKET3_COMPUTE for those three kinds
 * and PACKET3 for the others. Section 3.7 of the design states the same. */
#define BC250HSA_PACKET3_COMPUTE(op, n) (BC250HSA_PACKET3(op, n) | (1u << 1))
/* The pad dword all three GFX10 command processor ring types use
 * (driver/shim/bc250_gfx.c, BC250_CP_NOP, which is amdgpu's gfx_v10_0.c value). It is
 * PACKET3(PACKET3_NOP, 0x3FFF): a type-3 NOP whose count field is the maximum, which
 * the command processor reads as one dword to skip. The comment in amdgpu and in the
 * shim calls it a PACKET2 NOP; the value is a type-3 NOP, and test_pm4.c assembles it
 * from PACKET3 to keep the two apart. */
#define BC250HSA_CP_NOP 0xFFFF1000u

/* ---- opcodes (nvd.h) --------------------------------------------------------- */
#define BC250HSA_PKT3_NOP             0x10u
#define BC250HSA_PKT3_DISPATCH_DIRECT 0x15u
#define BC250HSA_PKT3_CONTEXT_CONTROL 0x28u
#define BC250HSA_PKT3_EVENT_WRITE     0x46u
#define BC250HSA_PKT3_RELEASE_MEM     0x49u
#define BC250HSA_PKT3_ACQUIRE_MEM     0x58u
#define BC250HSA_PKT3_SET_SH_REG      0x76u
#define BC250HSA_PKT3_SET_UCONFIG_REG 0x79u
#define BC250HSA_PKT3_SET_SH_REG_INDEX 0x9Bu

/* ---- register packet offsets ------------------------------------------------- */
#define BC250HSA_SH_REG_BIAS      0x19A0u  /* 0x2C00 - 0x1260 */
#define BC250HSA_UCONFIG_REG_BIAS 0x2000u  /* 0xC000 - 0xA000 */

#define BC250HSA_REG_COMPUTE_START_X            0x204u /* mm 0x1BA4 */
#define BC250HSA_REG_COMPUTE_NUM_THREAD_X       0x207u /* mm 0x1BA7 */
#define BC250HSA_REG_COMPUTE_PGM_LO             0x20Cu /* mm 0x1BAC */
#define BC250HSA_REG_COMPUTE_PGM_RSRC1          0x212u /* mm 0x1BB2 */
#define BC250HSA_REG_COMPUTE_RESOURCE_LIMITS    0x215u /* mm 0x1BB5 */
#define BC250HSA_REG_COMPUTE_STATIC_THREAD_MGMT_SE0 0x216u /* mm 0x1BB6 */
#define BC250HSA_REG_COMPUTE_TMPRING_SIZE       0x218u /* mm 0x1BB8 */
#define BC250HSA_REG_COMPUTE_STATIC_THREAD_MGMT_SE2 0x219u /* mm 0x1BB9 */
#define BC250HSA_REG_COMPUTE_REQ_CTRL           0x222u /* mm 0x1BC2 */
#define BC250HSA_REG_COMPUTE_PGM_RSRC3          0x228u /* mm 0x1BC8 */
#define BC250HSA_REG_COMPUTE_SHADER_CHKSUM      0x22Au /* mm 0x1BCA */
#define BC250HSA_REG_COMPUTE_USER_DATA_0        0x240u /* mm 0x1BE0 */
#define BC250HSA_REG_CP_COHER_START_DELAY       0x07Bu /* mm 0x207B, BASE_IDX 1 */

/* The index field of a SET_SH_REG_INDEX offset dword. Index 3 routes the compute
 * unit mask registers through the command processor's own path
 * (driver/shim/bc250_dispatch.c, BC250_DISPATCH_SH_REG_INDEX_CU). */
#define BC250HSA_SH_REG_INDEX_CU 3u

/* ---- ACQUIRE_MEM (nvd.h:447-488) -------------------------------------------- */
#define BC250HSA_AM_GCR_GLI_INV (1u << 0)
#define BC250HSA_AM_GCR_GLM_WB  (1u << 4)
#define BC250HSA_AM_GCR_GLM_INV (1u << 5)
#define BC250HSA_AM_GCR_GLK_INV (1u << 7)
#define BC250HSA_AM_GCR_GLV_INV (1u << 8)
#define BC250HSA_AM_GCR_GL1_INV (1u << 9)
#define BC250HSA_AM_GCR_GL2_INV (1u << 14)
#define BC250HSA_AM_GCR_GL2_WB  (1u << 15)
/* The eight actions driver/shim/bc250_dispatch.c emit_mem_sync() sets, which is what
 * amdgpu_ib_schedule() puts in front of every indirect buffer on Linux. The kernel
 * driver's own ring frame emits no acquire in front of a user indirect buffer, so the
 * host emits it here or the shader can be fetched through a stale cache line. */
#define BC250HSA_ACQUIRE_GCR_CNTL                                                \
    (BC250HSA_AM_GCR_GL2_INV | BC250HSA_AM_GCR_GL2_WB | BC250HSA_AM_GCR_GLM_INV | \
     BC250HSA_AM_GCR_GLM_WB | BC250HSA_AM_GCR_GL1_INV | BC250HSA_AM_GCR_GLV_INV | \
     BC250HSA_AM_GCR_GLK_INV | BC250HSA_AM_GCR_GLI_INV)
#define BC250HSA_ACQUIRE_POLL_INTERVAL 0x0000000Au
/* driver/shim/bc250_dispatch.c, BC250_DISPATCH_COHER_START_DELAY. */
#define BC250HSA_COHER_START_DELAY 0x20u

/* ---- EVENT_WRITE (nvd.h:326-343, navi10_enum.h) ----------------------------- */
#define BC250HSA_EVENT_TYPE(x)  (((uint32_t)(x) & 0x3Fu) << 0)
#define BC250HSA_EVENT_INDEX(x) (((uint32_t)(x) & 0xFu) << 8)
#define BC250HSA_EVENT_CS_PARTIAL_FLUSH       0x07u /* VGT_EVENT_TYPE */
#define BC250HSA_EVENT_INDEX_CS_PARTIAL_FLUSH 4u
#define BC250HSA_EVENT_CACHE_FLUSH_AND_INV_TS 0x14u
#define BC250HSA_EVENT_INDEX_EOP              5u

/* ---- RELEASE_MEM (nvd.h:354-387) -------------------------------------------- */
#define BC250HSA_RM_GCR_GLM_WB    (1u << 12)
#define BC250HSA_RM_GCR_GLM_INV   (1u << 13)
#define BC250HSA_RM_GCR_GL2_WB    (1u << 21)
#define BC250HSA_RM_GCR_SEQ       (1u << 22)
#define BC250HSA_RM_CACHE_POLICY(x) (((uint32_t)(x)) << 25)
#define BC250HSA_RM_DST_SEL(x)      (((uint32_t)(x)) << 16)
#define BC250HSA_RM_INT_SEL(x)      (((uint32_t)(x)) << 24)
#define BC250HSA_RM_DATA_SEL(x)     (((uint32_t)(x)) << 29)
/* The completion write of section 3.8, bit for bit our Vulkan driver's own progress
 * write and the kernel driver's own ring fence: CACHE_FLUSH_AND_INV_TS at event index
 * 5, GLM write-back and invalidate, GL2 write-back, SEQ 1, cache policy 3 (bypass);
 * then DATA_SEL 2 (a 64-bit value), INT_SEL 0 (no interrupt), DST_SEL 0 (memory).
 * Both dwords carry a static assertion in radv_wddm2_cs.c as well. */
#define BC250HSA_RELEASE_MEM_DW1                                                 \
    (BC250HSA_EVENT_TYPE(BC250HSA_EVENT_CACHE_FLUSH_AND_INV_TS) |                \
     BC250HSA_EVENT_INDEX(BC250HSA_EVENT_INDEX_EOP) | BC250HSA_RM_GCR_GLM_WB |   \
     BC250HSA_RM_GCR_GLM_INV | BC250HSA_RM_GCR_GL2_WB | BC250HSA_RM_GCR_SEQ |    \
     BC250HSA_RM_CACHE_POLICY(3u))
#define BC250HSA_RELEASE_MEM_DW2                                                 \
    (BC250HSA_RM_DATA_SEL(2u) | BC250HSA_RM_INT_SEL(0u) | BC250HSA_RM_DST_SEL(0u))
#define BC250HSA_RELEASE_MEM_DWORDS 8u

/* ---- DISPATCH_DIRECT initiator ---------------------------------------------- */
/* The three bits of COMPUTE_DISPATCH_INITIATOR this library writes. Every value is
 * the mask of gc_10_1_0_sh_mask.h, and test_pm4.c compares all three with that
 * header.
 *
 *   SHADER_EN     bit 0,  the one bit of the control dispatch measured on this
 *                 silicon (driver/shim/bc250_dispatch.c, BC250_DISPATCH_INITIATOR,
 *                 fact M49), whose shader is wave64.
 *   CS_W32_EN     bit 15, the only place a GFX10 compute dispatch selects wave32.
 *                 clang builds this part's kernels wave32, so the builder sets it
 *                 from the kernel descriptor and never from a measured constant.
 *   FORCE_START_0 bit 2,  not bit 4: bit 4 is ORDERED_APPEND_MODE. */
#define BC250HSA_DISPATCH_INITIATOR_SHADER_EN     0x00000001u
#define BC250HSA_DISPATCH_INITIATOR_FORCE_START_0 0x00000004u
#define BC250HSA_DISPATCH_INITIATOR_CS_W32_EN     0x00008000u

/* ---- the private segment buffer resource ------------------------------------ */
/* Word 3 of a 128-bit raw buffer resource on gfx10.1: DST_SEL X,Y,Z,W, FORMAT 0x4B
 * (32_32_32_32 uint), RESOURCE_LEVEL 1 (gfx10.1 needs it), OOB_SELECT 1, TYPE 0
 * (buffer). driver/shim/bc250_dispatch.c, BC250_DISPATCH_VDESC_W3. */
#define BC250HSA_BUFFER_RSRC_W3 0x1104BFACu

/* ---- CONTEXT_CONTROL -------------------------------------------------------- */
/* The graphics ring needs it in front of the first state write of an indirect
 * buffer; it is a no-operation on a compute ring. Both dwords are libdrm's and
 * RADV's own value (ref/libdrm/tests/amdgpu/shader_test_util.c): bit 31 is
 * UPDATE_LOAD_ENABLES in the first dword and UPDATE_SHADOW_ENABLES in the second,
 * and every per-type enable bit is clear, so the packet says "the enable set is
 * now this one, and it is empty": load nothing and shadow nothing. */
#define BC250HSA_CONTEXT_CONTROL_DW 0x80000000u

/* The indirect buffer padding of the GFX queue on this part. */
#define BC250HSA_IB_PAD_DWORDS 8u

#endif /* BC250HSA_PM4_REGS_H */
