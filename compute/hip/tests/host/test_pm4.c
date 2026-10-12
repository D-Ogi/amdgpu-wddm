/* test_pm4.c - the PM4 gate of layer 1.
 *
 * Two jobs:
 *
 *   1. Restate-and-compare (repo rules 1 and 2). bc250hsa/pm4_regs.h restates every
 *      packet number, event type and register offset under a BC250HSA_ prefix, because
 *      a user-mode library must not pull the kernel headers into every translation
 *      unit. This test includes the four originals and fails the build when any
 *      restated value drifts. No number here is typed from memory: each one is an
 *      expression over a vendored header.
 *
 *   2. The golden stream. tests/data/pm4_vadd.golden.txt holds the dwords that section
 *      3.7 of docs/design/m16-hip-route-b.md asks for, written by hand from the design
 *      table. The builder must produce them dword for dword.
 *
 * The test needs no adapter, no device and no fixture code object: the kernel is a
 * structure of known numbers.
 */
#include "test_common.h"

#include "internal.h"
#include "pm4_regs.h"

/* The originals. The build adds driver/amdgpu-import and third_party/linux-amdgpu to
 * the include path for this test only. */
#include "nvd.h"

/* cyan_skillfish_ip_offset.h marks its two structures with a Linux compiler attribute.
 * driver/shim/include/amdgpu.h defines it away; a user-mode test does the same instead
 * of pulling the whole shim in. */
#ifndef __maybe_unused
#define __maybe_unused
#endif

#include "cyan_skillfish_ip_offset.h"
#include "gc_10_1_0_offset.h"
#include "gc_10_1_0_sh_mask.h"
#include "navi10_enum.h"

/* The Mesa generated field macros of the DMA_DATA packet, as driver/kmd/gfx_copy_fields.h
 * already carries them for the kernel-mode emitter (provenance:
 * driver/kmd/gfx-copy-provenance.md). They are the second, independent statement of the
 * encoding that bc250hsa/pm4_regs.h restates.
 *
 * gfx_copy.h declares that emitter itself. The build compiles driver/kmd/gfx_copy.c into
 * this test, so the two dword streams are compared and not only the two sets of field
 * macros (check_against_kernel_emitter). */
#include "gfx_copy_fields.h"
#include "gfx_copy.h"

/* --------------------------------------------------------------------------------
 * 1. Restate-and-compare
 * ------------------------------------------------------------------------------ */

static void check_against_linux_headers(void)
{
    /* The packet header shape and the shader-type bit. */
    CHECK_U64(BC250HSA_PACKET_TYPE3, PACKET_TYPE3);
    CHECK_U64(BC250HSA_PACKET_TYPE2, PACKET_TYPE2);
    CHECK_U64(BC250HSA_PACKET3(0x76u, 3u), (uint32_t)PACKET3(0x76, 3));
    CHECK_U64(BC250HSA_PACKET3_COMPUTE(0x76u, 3u), (uint32_t)PACKET3_COMPUTE(0x76, 3));
    /* The ring pad is a type-3 NOP with the largest count field, not a PACKET2. */
    CHECK_U64(BC250HSA_CP_NOP, (uint32_t)PACKET3(PACKET3_NOP, 0x3FFF));
    CHECK_U64(CP_PACKET3_GET_OPCODE(BC250HSA_CP_NOP), PACKET3_NOP);
    CHECK_U64(CP_PACKET_GET_TYPE(BC250HSA_CP_NOP), PACKET_TYPE3);

    /* The opcodes. */
    CHECK_U64(BC250HSA_PKT3_NOP, PACKET3_NOP);
    CHECK_U64(BC250HSA_PKT3_DISPATCH_DIRECT, PACKET3_DISPATCH_DIRECT);
    CHECK_U64(BC250HSA_PKT3_CONTEXT_CONTROL, PACKET3_CONTEXT_CONTROL);
    CHECK_U64(BC250HSA_PKT3_EVENT_WRITE, PACKET3_EVENT_WRITE);
    CHECK_U64(BC250HSA_PKT3_RELEASE_MEM, PACKET3_RELEASE_MEM);
    CHECK_U64(BC250HSA_PKT3_ACQUIRE_MEM, PACKET3_ACQUIRE_MEM);
    CHECK_U64(BC250HSA_PKT3_SET_SH_REG, PACKET3_SET_SH_REG);
    CHECK_U64(BC250HSA_PKT3_SET_UCONFIG_REG, PACKET3_SET_UCONFIG_REG);
    CHECK_U64(BC250HSA_PKT3_SET_SH_REG_INDEX, PACKET3_SET_SH_REG_INDEX);

    /* The two biases, each the difference of two header values. */
    CHECK_U64(BC250HSA_SH_REG_BIAS, PACKET3_SET_SH_REG_START - GC_BASE__INST0_SEG0);
    CHECK_U64(BC250HSA_UCONFIG_REG_BIAS, PACKET3_SET_UCONFIG_REG_START - GC_BASE__INST0_SEG1);

    /* Every register offset of the dispatch, as offset = mm<name> - bias. */
#define CHECK_SH_REG(name, restated) CHECK_U64(restated, mm##name - BC250HSA_SH_REG_BIAS)
    CHECK_SH_REG(COMPUTE_START_X, BC250HSA_REG_COMPUTE_START_X);
    CHECK_SH_REG(COMPUTE_NUM_THREAD_X, BC250HSA_REG_COMPUTE_NUM_THREAD_X);
    CHECK_SH_REG(COMPUTE_PGM_LO, BC250HSA_REG_COMPUTE_PGM_LO);
    CHECK_SH_REG(COMPUTE_PGM_RSRC1, BC250HSA_REG_COMPUTE_PGM_RSRC1);
    CHECK_SH_REG(COMPUTE_RESOURCE_LIMITS, BC250HSA_REG_COMPUTE_RESOURCE_LIMITS);
    CHECK_SH_REG(COMPUTE_STATIC_THREAD_MGMT_SE0, BC250HSA_REG_COMPUTE_STATIC_THREAD_MGMT_SE0);
    CHECK_SH_REG(COMPUTE_TMPRING_SIZE, BC250HSA_REG_COMPUTE_TMPRING_SIZE);
    CHECK_SH_REG(COMPUTE_STATIC_THREAD_MGMT_SE2, BC250HSA_REG_COMPUTE_STATIC_THREAD_MGMT_SE2);
    CHECK_SH_REG(COMPUTE_REQ_CTRL, BC250HSA_REG_COMPUTE_REQ_CTRL);
    CHECK_SH_REG(COMPUTE_PGM_RSRC3, BC250HSA_REG_COMPUTE_PGM_RSRC3);
    CHECK_SH_REG(COMPUTE_SHADER_CHKSUM, BC250HSA_REG_COMPUTE_SHADER_CHKSUM);
    CHECK_SH_REG(COMPUTE_USER_DATA_0, BC250HSA_REG_COMPUTE_USER_DATA_0);
#undef CHECK_SH_REG
    CHECK_U64(BC250HSA_REG_CP_COHER_START_DELAY,
              mmCP_COHER_START_DELAY - BC250HSA_UCONFIG_REG_BIAS);

    /* DMA_DATA, the copy and fill packet of the GPU copy route
     * (scratch/hip-gpu-copy/CONTRACT.md section 4). Every field is checked twice: against
     * nvd.h, which this library restates, and against the Mesa generated field macros
     * that driver/kmd/gfx_copy_fields.h already holds for the kernel-mode emitter. Two
     * independent statements of one encoding that agree is the whole point of the gate. */
    CHECK_U64(BC250HSA_PKT3_DMA_DATA, PACKET3_DMA_DATA);
    CHECK_U64(BC250HSA_DMA_DST_SEL(3u), (uint32_t)PACKET3_DMA_DATA_DST_SEL(3));
    CHECK_U64(BC250HSA_DMA_DST_SEL(3u), (uint32_t)S_501_DST_SEL(3));
    CHECK_U64(BC250HSA_DMA_SRC_SEL(3u), (uint32_t)PACKET3_DMA_DATA_SRC_SEL(3));
    CHECK_U64(BC250HSA_DMA_SRC_SEL(3u), (uint32_t)S_501_SRC_SEL(3));
    CHECK_U64(BC250HSA_DMA_SRC_SEL(2u), (uint32_t)PACKET3_DMA_DATA_SRC_SEL(2));
    CHECK_U64(BC250HSA_DMA_CP_SYNC, (uint32_t)PACKET3_DMA_DATA_CP_SYNC);
    CHECK_U64(BC250HSA_DMA_CP_SYNC, (uint32_t)S_501_CP_SYNC(1));
    CHECK_U64(BC250HSA_DMA_ENGINE(1u), (uint32_t)PACKET3_DMA_DATA_ENGINE(1));
    CHECK_U64(BC250HSA_DMA_SRC_CACHE_POLICY(1u),
              (uint32_t)PACKET3_DMA_DATA_SRC_CACHE_POLICY(1));
    CHECK_U64(BC250HSA_DMA_DST_CACHE_POLICY(1u),
              (uint32_t)PACKET3_DMA_DATA_DST_CACHE_POLICY(1));
    CHECK_U64(BC250HSA_DMA_CMD_SAS, (uint32_t)PACKET3_DMA_DATA_CMD_SAS);
    CHECK_U64(BC250HSA_DMA_CMD_DAS, (uint32_t)PACKET3_DMA_DATA_CMD_DAS);
    CHECK_U64(BC250HSA_DMA_CMD_SAIC, (uint32_t)PACKET3_DMA_DATA_CMD_SAIC);
    CHECK_U64(BC250HSA_DMA_CMD_DAIC, (uint32_t)PACKET3_DMA_DATA_CMD_DAIC);
    CHECK_U64(BC250HSA_DMA_CMD_RAW_WAIT, (uint32_t)PACKET3_DMA_DATA_CMD_RAW_WAIT);
    CHECK_U64(BC250HSA_DMA_CMD_RAW_WAIT, (uint32_t)S_506_RAW_WAIT(1));
    /* DIS_WC is bit 31 of COMMAND, which nvd.h does not name and the Mesa register
     * table does (CP_DMA_ME_COMMAND). It is the one field of the packet whose only
     * statement in this repository is this one, so the value is stated here as well. */
    CHECK_U64(BC250HSA_DMA_CMD_DIS_WC, 0x80000000u);
    CHECK_U64(BC250HSA_DMA_BYTE_COUNT(~0u), (uint32_t)S_506_BYTE_COUNT(~0u));
    CHECK_U64(BC250HSA_DMA_BYTE_COUNT(~0u), 0x03FFFFFFu);
    CHECK_U64(BC250HSA_DMA_DST_ADDR_USING_L2, V_501_DST_ADDR_USING_L2);
    CHECK_U64(BC250HSA_DMA_SRC_ADDR_USING_L2, V_501_SRC_ADDR_USING_L2);
    /* cp_dma_max_byte_count(GFX10) of radv_cp_dma.c:34-41, which is also COPY_MAX of
     * driver/kmd/gfx_copy.c. */
    CHECK_U64(BC250HSA_DMA_MAX_BYTES, (uint32_t)(S_506_BYTE_COUNT(~0u) & ~31u));
    CHECK_U64(BC250HSA_DMA_MAX_BYTES, 67108832u);
    CHECK_U64(BC250HSA_GPU_COPY_MAX_BYTES, BC250HSA_DMA_MAX_BYTES);
    /* The seven dwords are header plus six, so the header count field is 5. */
    CHECK_U64(BC250HSA_PACKET3(BC250HSA_PKT3_DMA_DATA, 5u),
              (uint32_t)PACKET3(PACKET3_DMA_DATA, 5));

    /* The two event types, from the VGT event enumeration. */
    CHECK_U64(BC250HSA_EVENT_CS_PARTIAL_FLUSH, CS_PARTIAL_FLUSH);
    CHECK_U64(BC250HSA_EVENT_CACHE_FLUSH_AND_INV_TS, CACHE_FLUSH_AND_INV_TS_EVENT);

    /* The two RELEASE_MEM dwords, assembled a second time from the fields of nvd.h.
     * They are also the two values our Vulkan driver asserts in radv_wddm2_cs.c. */
    CHECK_U64(BC250HSA_RELEASE_MEM_DW1,
              (uint32_t)(PACKET3_RELEASE_MEM_EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |
                         PACKET3_RELEASE_MEM_EVENT_INDEX(5) |
                         PACKET3_RELEASE_MEM_GCR_GLM_WB | PACKET3_RELEASE_MEM_GCR_GLM_INV |
                         PACKET3_RELEASE_MEM_GCR_GL2_WB | PACKET3_RELEASE_MEM_GCR_SEQ |
                         PACKET3_RELEASE_MEM_CACHE_POLICY(3)));
    CHECK_U64(BC250HSA_RELEASE_MEM_DW1, 0x06603514u);
    CHECK_U64(BC250HSA_RELEASE_MEM_DW2,
              (uint32_t)(PACKET3_RELEASE_MEM_DATA_SEL(2) | PACKET3_RELEASE_MEM_INT_SEL(0) |
                         PACKET3_RELEASE_MEM_DST_SEL(0)));
    CHECK_U64(BC250HSA_RELEASE_MEM_DW2, 0x40000000u);

    /* The eight cache actions of the acquire, assembled from nvd.h as well. */
    CHECK_U64(BC250HSA_ACQUIRE_GCR_CNTL,
              (uint32_t)(PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_INV(1) |
                         PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_WB(1) |
                         PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_INV(1) |
                         PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_WB(1) |
                         PACKET3_ACQUIRE_MEM_GCR_CNTL_GL1_INV(1) |
                         PACKET3_ACQUIRE_MEM_GCR_CNTL_GLV_INV(1) |
                         PACKET3_ACQUIRE_MEM_GCR_CNTL_GLK_INV(1) |
                         PACKET3_ACQUIRE_MEM_GCR_CNTL_GLI_INV(1)));
    CHECK_U64(BC250HSA_ACQUIRE_GCR_CNTL, 0x0000C3B1u);

    /* The event dword of the partial flush, assembled from nvd.h. */
    CHECK_U64(BC250HSA_EVENT_TYPE(BC250HSA_EVENT_CS_PARTIAL_FLUSH) |
                  BC250HSA_EVENT_INDEX(BC250HSA_EVENT_INDEX_CS_PARTIAL_FLUSH),
              (uint32_t)(EVENT_TYPE(CS_PARTIAL_FLUSH) | EVENT_INDEX(4)));

    /* Every bit field this library restates, against gc_10_1_0_sh_mask.h. Without
     * these lines the register offsets were gated and the bits inside the registers
     * were not, which is how a FORCE_START_AT_000 on the wrong bit and a missing
     * CS_W32_EN both reached a green build. */
    CHECK_U64(BC250HSA_DISPATCH_INITIATOR_SHADER_EN,
              (uint32_t)COMPUTE_DISPATCH_INITIATOR__COMPUTE_SHADER_EN_MASK);
    CHECK_U64(BC250HSA_DISPATCH_INITIATOR_FORCE_START_0,
              (uint32_t)COMPUTE_DISPATCH_INITIATOR__FORCE_START_AT_000_MASK);
    CHECK_U64(BC250HSA_DISPATCH_INITIATOR_CS_W32_EN,
              (uint32_t)COMPUTE_DISPATCH_INITIATOR__CS_W32_EN_MASK);
    /* The three are distinct bits, so a copy of one constant into another name is a
     * failure as well. */
    CHECK_U64(BC250HSA_DISPATCH_INITIATOR_SHADER_EN &
                  (BC250HSA_DISPATCH_INITIATOR_FORCE_START_0 |
                   BC250HSA_DISPATCH_INITIATOR_CS_W32_EN), 0u);
    CHECK_U64(BC250HSA_DISPATCH_INITIATOR_FORCE_START_0 &
                  BC250HSA_DISPATCH_INITIATOR_CS_W32_EN, 0u);
    /* COMPUTE_PGM_RSRC2: the two fields the builder reads and writes. A shift and a
     * mask are restated, so both are compared with the header's own mask. */
    CHECK_U64(BC250HSA_RSRC2_USER_SGPR_SHIFT, COMPUTE_PGM_RSRC2__USER_SGPR__SHIFT);
    CHECK_U64((uint32_t)BC250HSA_RSRC2_USER_SGPR_MASK << BC250HSA_RSRC2_USER_SGPR_SHIFT,
              (uint32_t)COMPUTE_PGM_RSRC2__USER_SGPR_MASK);
    CHECK_U64(BC250HSA_RSRC2_LDS_SIZE_SHIFT, COMPUTE_PGM_RSRC2__LDS_SIZE__SHIFT);
    CHECK_U64((uint32_t)BC250HSA_RSRC2_LDS_SIZE_MASK << BC250HSA_RSRC2_LDS_SIZE_SHIFT,
              (uint32_t)COMPUTE_PGM_RSRC2__LDS_SIZE_MASK);
}

/* --------------------------------------------------------------------------------
 * 2. The golden stream
 * ------------------------------------------------------------------------------ */

#define GOLDEN_MAX 256u

static uint32_t g_golden[GOLDEN_MAX];
static uint32_t g_golden_count;

static int read_golden(const char* dir)
{
    char  path[1024];
    char  line[512];
    FILE* f;

    if (snprintf(path, sizeof(path), "%s/pm4_vadd.golden.txt", dir) < 0) {
        return 0;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL cannot open %s\n", path);
        g_failures++;
        return 0;
    }
    g_golden_count = 0;
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        char*         p = line;
        unsigned long value;
        char*         end;
        while (*p == ' ' || *p == '\t') { p++; }
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') {
            continue;
        }
        value = strtoul(p, &end, 16);
        if (end == p || (end - p) != 8) {
            printf("FAIL %s: a dword line must hold 8 hexadecimal digits: %s", path, line);
            g_failures++;
            continue;
        }
        if (g_golden_count >= GOLDEN_MAX) {
            printf("FAIL %s holds more than %u dwords\n", path, (unsigned)GOLDEN_MAX);
            g_failures++;
            break;
        }
        g_golden[g_golden_count++] = (uint32_t)value;
    }
    fclose(f);
    return g_golden_count > 0;
}

/* The kernel of the golden stream: the measured numbers of vadd, with no fixture. */
static void golden_kernel(bc250hsa_kernel* k)
{
    memset(k, 0, sizeof(*k));
    k->name = "vadd";
    k->descriptor_va = 0x00000140ABCD0C80ull;
    k->entry_va = 0x00000140ABCD1E00ull;
    k->kernarg_bytes = 28u;
    k->kernarg_align = 16u;
    k->group_segment_bytes = 0u;
    k->private_segment_bytes = 0u;
    k->max_flat_workgroup_size = 1024u;
    k->sgpr_count = 9u;
    k->vgpr_count = 8u;
    k->wave_size = 32u;
    k->workgroup_processor_mode = 1u;
    k->uses_dynamic_stack = 0u;
    k->user_sgpr_count = 6u;
    k->compute_pgm_rsrc1 = 0xE0AF0000u;
    k->compute_pgm_rsrc2 = 0x0000008Cu;
    k->compute_pgm_rsrc3 = 0u;
    k->kernel_code_properties = 0x0409u;
    k->arg_count = 0u;
}

static void golden_inputs(bc250hsa_dispatch* d, bc250hsa_pm4_env* env,
                          const bc250hsa_kernel* k)
{
    memset(d, 0, sizeof(*d));
    d->struct_bytes = (uint32_t)sizeof(*d);
    d->flags = 0u;
    d->kernel = k;
    d->kernarg_va = 0x00000140ABCD2000ull;
    d->launch.struct_bytes = (uint32_t)sizeof(d->launch);
    d->launch.grid[0] = 4096u; d->launch.grid[1] = 1u; d->launch.grid[2] = 1u;
    d->launch.block[0] = 256u; d->launch.block[1] = 1u; d->launch.block[2] = 1u;
    d->launch.dynamic_group_bytes = 0u;

    memset(env, 0, sizeof(*env));
    env->struct_bytes = (uint32_t)sizeof(*env);
    env->flags = BC250HSA_DISPATCH_GFX_RING;
    env->fence_va = 0x00000140ABCD3008ull;
    env->fence_value = 0x0000000100000007ull;
    env->private_segment_rsrc[0] = 0x00001000u;
    env->private_segment_rsrc[1] = 0x00000040u;
    env->private_segment_rsrc[2] = 0x00010000u;
    env->private_segment_rsrc[3] = BC250HSA_BUFFER_RSRC_W3;
    env->ib_pad_dwords = 0u; /* takes 8 */
}

static void check_golden(void)
{
    bc250hsa_kernel   k;
    bc250hsa_dispatch d;
    bc250hsa_pm4_env  env;
    uint32_t          out[GOLDEN_MAX];
    uint32_t          written = 0;
    uint32_t          i;

    golden_kernel(&k);
    golden_inputs(&d, &env, &k);
    memset(out, 0xCD, sizeof(out));
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    CHECK_U64(written, g_golden_count);
    CHECK_U64(written % 8u, 0u);
    if (written != g_golden_count) {
        return;
    }
    for (i = 0; i < written; i++) {
        if (out[i] != g_golden[i]) {
            g_failures++;
            printf("FAIL dword %u is 0x%08lx, the golden stream says 0x%08lx\n", (unsigned)i,
                   (unsigned long)out[i], (unsigned long)g_golden[i]);
        }
    }
    g_checks++;
}

/* --------------------------------------------------------------------------------
 * 2b. The batched indirect buffer (section 8.1 of the interface)
 * ------------------------------------------------------------------------------ */

#define BATCH_MAX 2048u

/* What one walk of a stream found: how many packets of each kind it holds, and the
 * GCR_CNTL dword of every ACQUIRE_MEM in order. The walker reads the count field of
 * every type-3 header, so a malformed stream ends the walk short and the dword count
 * it reports does not match the stream length. */
typedef struct stream_shape {
    uint32_t walked;          /* dwords the walk consumed */
    uint32_t context_control;
    uint32_t acquire;
    uint32_t dispatch_direct;
    uint32_t cs_partial_flush;
    uint32_t release_mem;
    uint32_t nop;
    uint32_t gcr[8];          /* the GCR_CNTL of the first eight acquires */
} stream_shape;

static void walk_stream(const uint32_t* dwords, uint32_t count, stream_shape* shape)
{
    uint32_t at = 0;

    memset(shape, 0, sizeof(*shape));
    while (at < count) {
        const uint32_t header = dwords[at];
        const uint32_t opcode = (header >> 8) & 0xFFu;
        const uint32_t body = ((header >> 16) & 0x3FFFu) + 1u;
        if (CP_PACKET_GET_TYPE(header) != PACKET_TYPE3) {
            break;
        }
        if (opcode == BC250HSA_PKT3_NOP) {
            /* The ring pad: the count field is the maximum and the command processor
             * reads one dword. */
            shape->nop++;
            at++;
            shape->walked = at;
            continue;
        }
        if (at + 1u + body > count) {
            break;
        }
        if (opcode == BC250HSA_PKT3_CONTEXT_CONTROL) {
            shape->context_control++;
        } else if (opcode == BC250HSA_PKT3_ACQUIRE_MEM) {
            if (shape->acquire < 8u) {
                shape->gcr[shape->acquire] = dwords[at + body];
            }
            shape->acquire++;
        } else if (opcode == BC250HSA_PKT3_DISPATCH_DIRECT) {
            shape->dispatch_direct++;
        } else if (opcode == BC250HSA_PKT3_EVENT_WRITE) {
            if ((dwords[at + 1u] & 0x3Fu) == BC250HSA_EVENT_CS_PARTIAL_FLUSH) {
                shape->cs_partial_flush++;
            }
        } else if (opcode == BC250HSA_PKT3_RELEASE_MEM) {
            shape->release_mem++;
        }
        at += 1u + body;
        shape->walked = at;
    }
}

static void check_batch(void)
{
    bc250hsa_kernel   k;
    bc250hsa_dispatch d[3];
    bc250hsa_dispatch one;
    bc250hsa_pm4_env  env;
    uint32_t          single[GOLDEN_MAX];
    uint32_t          batch[BATCH_MAX];
    uint32_t          single_count = 0;
    uint32_t          batch_count = 0;
    stream_shape      shape;
    uint32_t          i;

    golden_kernel(&k);
    golden_inputs(&one, &env, &k);

    /* 1. A batch of one is the single dispatch, dword for dword. The golden stream is
     *    already compared with the single build, so this is the control that the
     *    batched path writes the same head, body and completion write. */
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&one, &env, single, GOLDEN_MAX, &single_count),
                 BC250HSA_OK);
    CHECK_STATUS(bc250hsa_pm4_build_batch(&one, 1u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_OK);
    CHECK_U64(batch_count, single_count);
    if (batch_count == single_count) {
        int same = 1;
        for (i = 0; i < batch_count; i++) {
            if (batch[i] != single[i]) {
                same = 0;
            }
        }
        CHECK(same);
    }

    /* 2. Three dispatches, the full barrier. One head, one completion write, three
     *    dispatches, three waits for the waves, and three acquires: the head's and one
     *    in front of each dispatch but the first. */
    for (i = 0; i < 3u; i++) {
        golden_inputs(&d[i], &env, &k);
        d[i].launch.grid[0] = 16u + i;   /* so the bodies are not identical */
    }
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, 3u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_OK);
    CHECK_U64(batch_count % 8u, 0u);
    walk_stream(batch, batch_count, &shape);
    CHECK_U64(shape.walked, batch_count);
    CHECK_U64(shape.context_control, 1u);
    CHECK_U64(shape.dispatch_direct, 3u);
    CHECK_U64(shape.cs_partial_flush, 3u);
    CHECK_U64(shape.release_mem, 1u);
    CHECK_U64(shape.acquire, 3u);
    CHECK_U64(shape.gcr[0], BC250HSA_ACQUIRE_GCR_CNTL);
    CHECK_U64(shape.gcr[1], BC250HSA_ACQUIRE_GCR_CNTL);
    CHECK_U64(shape.gcr[2], BC250HSA_ACQUIRE_GCR_CNTL);
    /* One buffer of three is shorter than three buffers of one: it drops two heads and
     * two completion writes and keeps the acquires. */
    CHECK(batch_count < 3u * single_count);
    /* The grid of each dispatch, in the order they were appended. */
    {
        uint32_t at = 0;
        uint32_t seen = 0;
        while (at < batch_count) {
            const uint32_t header = batch[at];
            const uint32_t opcode = (header >> 8) & 0xFFu;
            const uint32_t body = ((header >> 16) & 0x3FFFu) + 1u;
            if (opcode == BC250HSA_PKT3_NOP) {
                at++;
                continue;
            }
            if (opcode == BC250HSA_PKT3_DISPATCH_DIRECT) {
                CHECK_U64(batch[at + 1u], 16u + seen);
                seen++;
            }
            at += 1u + body;
        }
        CHECK_U64(seen, 3u);
    }

    /* 3. The light barrier. The head keeps the full acquire, because the instruction
     *    cache invalidate belongs there, and the two barriers between the dispatches
     *    carry the level-0 and level-1 invalidate only. */
    golden_inputs(&d[0], &env, &k);
    env.flags |= BC250HSA_DISPATCH_LIGHT_BARRIER;
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, 3u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_OK);
    walk_stream(batch, batch_count, &shape);
    CHECK_U64(shape.walked, batch_count);
    CHECK_U64(shape.acquire, 3u);
    CHECK_U64(shape.gcr[0], BC250HSA_ACQUIRE_GCR_CNTL);
    CHECK_U64(shape.gcr[1], BC250HSA_ACQUIRE_GCR_CNTL_LIGHT);
    CHECK_U64(shape.gcr[2], BC250HSA_ACQUIRE_GCR_CNTL_LIGHT);
    /* The light barrier is a subset of the full one, and a strict subset: a test that
     * passed with the two values equal would prove nothing. */
    CHECK_U64(BC250HSA_ACQUIRE_GCR_CNTL_LIGHT & ~(uint32_t)BC250HSA_ACQUIRE_GCR_CNTL, 0u);
    /* And the bits it drops, exactly. An empty difference would make every check above
     * pass with the two barriers equal, which would prove nothing. */
    CHECK_U64(BC250HSA_ACQUIRE_GCR_CNTL & ~(uint32_t)BC250HSA_ACQUIRE_GCR_CNTL_LIGHT,
              (uint32_t)(BC250HSA_AM_GCR_GL2_INV | BC250HSA_AM_GCR_GL2_WB |
                         BC250HSA_AM_GCR_GLM_INV | BC250HSA_AM_GCR_GLM_WB |
                         BC250HSA_AM_GCR_GLI_INV));
    /* What it leaves out, by name: the level-2 cache, the metadata cache and the
     * instruction cache. What it keeps: the two level-0 caches and level 1. */
    CHECK_U64(BC250HSA_ACQUIRE_GCR_CNTL_LIGHT &
                  (uint32_t)(BC250HSA_AM_GCR_GL2_INV | BC250HSA_AM_GCR_GL2_WB |
                             BC250HSA_AM_GCR_GLM_INV | BC250HSA_AM_GCR_GLM_WB |
                             BC250HSA_AM_GCR_GLI_INV),
              0u);
    CHECK_U64(BC250HSA_ACQUIRE_GCR_CNTL_LIGHT,
              (uint32_t)(BC250HSA_AM_GCR_GL1_INV | BC250HSA_AM_GCR_GLV_INV |
                         BC250HSA_AM_GCR_GLK_INV));
    /* The negative control of the barrier: with the acquire left out there is none at
     * all, and the light bit changes nothing. */
    golden_inputs(&d[0], &env, &k);
    env.flags |= BC250HSA_DISPATCH_LIGHT_BARRIER | BC250HSA_DISPATCH_NO_ACQUIRE;
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, 3u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_OK);
    walk_stream(batch, batch_count, &shape);
    CHECK_U64(shape.acquire, 0u);
    CHECK_U64(shape.dispatch_direct, 3u);

    /* 4. The refusals of the batch builder. */
    golden_inputs(&d[0], &env, &k);
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, 0u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, BC250HSA_BATCH_DISPATCHES_MAX + 1u, &env, batch,
                                          BATCH_MAX, &batch_count),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_build_batch(NULL, 1u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_EINVAL);
    /* A capacity that holds two dispatches and not three is BC250HSA_ENOMEM and no
     * half-written buffer: submit.c reads the overflow and opens another buffer. The
     * capacity is measured and not computed from the single-dispatch length, because a
     * dispatch that repeats the one before it is shorter than the first one of the
     * buffer (section 8.8): exactly the length of the two-dispatch stream leaves no room
     * for a third. */
    {
        bc250hsa_counters before;
        bc250hsa_counters after;
        uint32_t          two_count = 0;

        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, batch, BATCH_MAX, &two_count),
                     BC250HSA_OK);
        CHECK(two_count > 0u && two_count < 3u * single_count);
        before.struct_bytes = (uint32_t)sizeof(before);
        CHECK_STATUS(bc250hsa_counters_read(&before), BC250HSA_OK);
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 3u, &env, batch, two_count, &batch_count),
                     BC250HSA_ENOMEM);
        /* And the dispatch the buffer had no room for is not counted as built. submit.c
         * appends it again into the next buffer, so a count here would count it twice,
         * and dispatches_built is the denominator of submissions per dispatch. */
        after.struct_bytes = (uint32_t)sizeof(after);
        CHECK_STATUS(bc250hsa_counters_read(&after), BC250HSA_OK);
        CHECK_U64(after.dispatches_built - before.dispatches_built, 2u);
    }
}

/* --------------------------------------------------------------------------------
 * 2c. The per-dispatch state cache (section 8.8 of the design)
 *
 * A SET_SH_REG write is persistent register state, and DISPATCH_DIRECT does not clear
 * it, so a dispatch that follows another one in the same indirect buffer has to write
 * only the registers whose value differs. These tests say exactly which packets a
 * second dispatch holds, in dwords, for four cases, and the negative control is the
 * one that matters: a field that DID change must be written again.
 * ------------------------------------------------------------------------------ */

/* The segments of a batched stream: segment i is everything the walk met after
 * DISPATCH_DIRECT i-1 and up to and including DISPATCH_DIRECT i. For each segment it
 * records the first register offset of every SET_SH_REG, how many SET_SH_REG_INDEX and
 * SET_UCONFIG_REG packets it holds, how many acquires, and how many dwords it is. */
#define SEG_MAX  8u
#define SEG_REGS 24u

typedef struct seg_shape {
    uint32_t count;
    uint32_t dwords[SEG_MAX];
    uint32_t regs[SEG_MAX][SEG_REGS];
    uint32_t reg_count[SEG_MAX];
    uint32_t sh_reg_index[SEG_MAX];
    uint32_t uconfig[SEG_MAX];
    uint32_t acquire[SEG_MAX];
    uint32_t partial_flush[SEG_MAX];
} seg_shape;

static void walk_segments(const uint32_t* dwords, uint32_t count, seg_shape* out)
{
    uint32_t at = 0;
    uint32_t seg = 0;
    uint32_t start = 0;
    int      dispatched = 0;

    memset(out, 0, sizeof(*out));
    while (at < count && seg < SEG_MAX) {
        const uint32_t header = dwords[at];
        const uint32_t opcode = (header >> 8) & 0xFFu;
        const uint32_t body = ((header >> 16) & 0x3FFFu) + 1u;

        if (CP_PACKET_GET_TYPE(header) != PACKET_TYPE3) {
            break;
        }
        if (opcode == BC250HSA_PKT3_NOP) {
            at++;
            continue;
        }
        if (at + 1u + body > count) {
            break;
        }
        if (opcode == BC250HSA_PKT3_SET_SH_REG) {
            if (out->reg_count[seg] < SEG_REGS) {
                out->regs[seg][out->reg_count[seg]] = dwords[at + 1u];
            }
            out->reg_count[seg]++;
        } else if (opcode == BC250HSA_PKT3_SET_SH_REG_INDEX) {
            out->sh_reg_index[seg]++;
        } else if (opcode == BC250HSA_PKT3_SET_UCONFIG_REG) {
            out->uconfig[seg]++;
        } else if (opcode == BC250HSA_PKT3_ACQUIRE_MEM) {
            out->acquire[seg]++;
        } else if (opcode == BC250HSA_PKT3_DISPATCH_DIRECT) {
            dispatched = 1;
        } else if (opcode == BC250HSA_PKT3_EVENT_WRITE) {
            if ((dwords[at + 1u] & 0x3Fu) == BC250HSA_EVENT_CS_PARTIAL_FLUSH) {
                out->partial_flush[seg]++;
            }
        }
        at += 1u + body;
        /* The wait for the waves belongs to the dispatch in front of it, so the segment
         * ends after that packet and not at DISPATCH_DIRECT itself. */
        if (dispatched && opcode == BC250HSA_PKT3_EVENT_WRITE) {
            out->dwords[seg] = at - start;
            start = at;
            seg++;
            out->count = seg;
            dispatched = 0;
        }
    }
}

static int seg_writes(const seg_shape* s, uint32_t seg, uint32_t reg)
{
    uint32_t i;
    const uint32_t held = (s->reg_count[seg] < SEG_REGS) ? s->reg_count[seg] : SEG_REGS;

    for (i = 0; i < held; i++) {
        if (s->regs[seg][i] == reg) {
            return 1;
        }
    }
    return 0;
}

/* The dwords of a dispatch that repeats everything of the one before it: the barrier,
 * DISPATCH_DIRECT with its four values, and the wait for the waves. */
#define SEG_REPEAT_DWORDS    (8u + 5u + 2u)
/* The same, plus the one run of user data registers, which is what a real launch is:
 * layer 2 hands out another kernel argument buffer per launch. */
#define SEG_NEW_KERNARG_DWORDS (SEG_REPEAT_DWORDS + 8u)
/* And with no cache at all: the whole compute state of section 3.7 again. */
#define SEG_FULL_DWORDS      (8u + 64u)

static void check_state_cache(void)
{
    bc250hsa_kernel   k;
    bc250hsa_kernel   k2;
    bc250hsa_dispatch d[3];
    bc250hsa_pm4_env  env;
    uint32_t          batch[BATCH_MAX];
    uint32_t          full[BATCH_MAX];
    uint32_t          batch_count = 0;
    uint32_t          full_count = 0;
    seg_shape         s;
    uint32_t          i;

    golden_kernel(&k);

    /* 1. The same kernel twice, the same arguments, the same block. The second
     *    dispatch writes no register at all: only the barrier, the dispatch and the
     *    wait for the waves. */
    for (i = 0; i < 3u; i++) {
        golden_inputs(&d[i], &env, &k);
    }
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_OK);
    walk_segments(batch, batch_count, &s);
    CHECK_U64(s.count, 2u);
    CHECK_U64(s.reg_count[1], 0u);
    CHECK_U64(s.sh_reg_index[1], 0u);
    CHECK_U64(s.uconfig[1], 0u);
    CHECK_U64(s.acquire[1], 1u);
    CHECK_U64(s.partial_flush[1], 1u);
    CHECK_U64(s.dwords[1], SEG_REPEAT_DWORDS);
    /* The first dispatch of the buffer is complete: the eight register runs of section
     * 3.7, the two compute-unit masks and the one uconfig write. */
    CHECK_U64(s.reg_count[0], 10u);
    CHECK_U64(s.sh_reg_index[0], 2u);
    CHECK_U64(s.uconfig[0], 1u);

    /* 1b. The control: with BC250HSA_DISPATCH_FULL_STATE the second dispatch is the
     *     whole sequence again, and the stream is the one the builder wrote before this
     *     section existed. A cache that wrote nothing in case 1 and nothing here too
     *     would pass case 1 and fail this. */
    golden_inputs(&d[0], &env, &k);
    env.flags |= BC250HSA_DISPATCH_FULL_STATE;
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, full, BATCH_MAX, &full_count),
                 BC250HSA_OK);
    walk_segments(full, full_count, &s);
    CHECK_U64(s.count, 2u);
    CHECK_U64(s.reg_count[1], 10u);
    CHECK_U64(s.sh_reg_index[1], 2u);
    CHECK_U64(s.uconfig[1], 1u);
    CHECK_U64(s.dwords[1], SEG_FULL_DWORDS);
    CHECK(full_count > batch_count);

    /* 2. Two kernels. Everything that belongs to the kernel goes out again: the
     *    program address, the two resource registers, RSRC3 and the user data run,
     *    because the private segment resource and the kernel argument pointer share
     *    that run. What stays out is the constant state: the start registers, the
     *    checksum, the request control, the coherency delay, the compute-unit masks,
     *    the scratch ring size and the resource limits. */
    golden_kernel(&k2);
    k2.name = "other";
    k2.entry_va = 0x00000140ABCE5100ull;
    k2.compute_pgm_rsrc1 = 0xE0AF0001u;
    k2.compute_pgm_rsrc3 = 0x00000002u;
    golden_inputs(&d[0], &env, &k);
    golden_inputs(&d[1], &env, &k2);
    d[1].kernel = &k2;
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_OK);
    walk_segments(batch, batch_count, &s);
    CHECK_U64(s.count, 2u);
    CHECK(seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_PGM_LO));
    CHECK(seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_PGM_RSRC1));
    CHECK(seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_PGM_RSRC3));
    CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_START_X));
    CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_SHADER_CHKSUM));
    CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_REQ_CTRL));
    CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_TMPRING_SIZE));
    CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_RESOURCE_LIMITS));
    CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_NUM_THREAD_X));
    CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_USER_DATA_0));
    CHECK_U64(s.sh_reg_index[1], 0u);
    CHECK_U64(s.uconfig[1], 0u);

    /* 3. Kernel A, kernel B, kernel A. The third dispatch is not the second, so the
     *    program address and the resource registers go out a third time: the cache
     *    holds what the hardware was last told and not a set of everything seen. */
    golden_inputs(&d[0], &env, &k);
    golden_inputs(&d[1], &env, &k2);
    d[1].kernel = &k2;
    golden_inputs(&d[2], &env, &k);
    CHECK_STATUS(bc250hsa_pm4_build_batch(d, 3u, &env, batch, BATCH_MAX, &batch_count),
                 BC250HSA_OK);
    walk_segments(batch, batch_count, &s);
    CHECK_U64(s.count, 3u);
    CHECK(seg_writes(&s, 2u, BC250HSA_REG_COMPUTE_PGM_LO));
    CHECK(seg_writes(&s, 2u, BC250HSA_REG_COMPUTE_PGM_RSRC1));
    CHECK(seg_writes(&s, 2u, BC250HSA_REG_COMPUTE_PGM_RSRC3));
    CHECK(!seg_writes(&s, 2u, BC250HSA_REG_COMPUTE_START_X));
    CHECK_U64(s.dwords[1], s.dwords[2]);

    /* 4. The negative control, one changed field at a time. Each of these dispatches
     *    repeats the one before it except in the one field named, and the register run
     *    that carries that field must be in the second segment. A cache that skipped it
     *    would launch the second dispatch with the first one's value, which is the
     *    whole risk of this mechanism, and each line below is the test that catches it. */
    {
        /* the block, which is COMPUTE_NUM_THREAD_X */
        golden_inputs(&d[0], &env, &k);
        golden_inputs(&d[1], &env, &k);
        d[1].launch.block[1] = 2u;
        d[1].launch.grid[0] = 2048u;   /* the work items per dispatch stay inside 1024 */
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, batch, BATCH_MAX, &batch_count),
                     BC250HSA_OK);
        walk_segments(batch, batch_count, &s);
        CHECK(seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_NUM_THREAD_X));
        CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_PGM_LO));

        /* the kernel argument pointer, which is the user data run */
        golden_inputs(&d[0], &env, &k);
        golden_inputs(&d[1], &env, &k);
        d[1].kernarg_va = 0x00000140ABCD4000ull;
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, batch, BATCH_MAX, &batch_count),
                     BC250HSA_OK);
        walk_segments(batch, batch_count, &s);
        CHECK(seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_USER_DATA_0));
        CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_PGM_LO));
        CHECK_U64(s.dwords[1], SEG_NEW_KERNARG_DWORDS);

        /* the local memory a dispatch asks for, which is RSRC2 and shares its run
         * with RSRC1 */
        golden_inputs(&d[0], &env, &k);
        golden_inputs(&d[1], &env, &k);
        d[1].launch.dynamic_group_bytes = 1024u;
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, batch, BATCH_MAX, &batch_count),
                     BC250HSA_OK);
        walk_segments(batch, batch_count, &s);
        CHECK(seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_PGM_RSRC1));
        CHECK(!seg_writes(&s, 1u, BC250HSA_REG_COMPUTE_PGM_LO));

        /* the grid, which is DISPATCH_DIRECT and is never cached */
        golden_inputs(&d[0], &env, &k);
        golden_inputs(&d[1], &env, &k);
        d[1].launch.grid[2] = 3u;
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, batch, BATCH_MAX, &batch_count),
                     BC250HSA_OK);
        walk_segments(batch, batch_count, &s);
        CHECK_U64(s.count, 2u);
        CHECK_U64(s.dwords[1], SEG_REPEAT_DWORDS);
    }

    /* 5. The buffer boundary resets the cache. bc250hsa_pm4_build_batch starts a new
     *    cache for every call, which is what submit.c does when it opens a buffer, and
     *    the first dispatch of the second buffer must be complete: another context's
     *    work runs between two of our submissions, and this build programs no state at
     *    the ring frame. Two buffers of two dispatches each are therefore twice the
     *    stream of case 1, dword for dword. */
    {
        uint32_t second[BATCH_MAX];
        uint32_t second_count = 0;

        golden_inputs(&d[0], &env, &k);
        golden_inputs(&d[1], &env, &k);
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, batch, BATCH_MAX, &batch_count),
                     BC250HSA_OK);
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 2u, &env, second, BATCH_MAX, &second_count),
                     BC250HSA_OK);
        CHECK_U64(second_count, batch_count);
        if (second_count == batch_count) {
            int same = 1;
            for (i = 0; i < batch_count; i++) {
                if (second[i] != batch[i]) {
                    same = 0;
                }
            }
            CHECK(same);
        }
        walk_segments(second, second_count, &s);
        CHECK_U64(s.reg_count[0], 10u);
        CHECK_U64(s.sh_reg_index[0], 2u);
        CHECK_U64(s.uconfig[0], 1u);
    }

    /* 6. The measurement this change is worth, in dwords, for the two shapes a decode
     *    loop has. The slot ceiling follows from it: one 65536-byte command ring slot
     *    holds 16368 usable dwords (the completion write and the padding aside). */
    {
        const uint32_t usable = 16368u;
        uint32_t       cached = 0;
        uint32_t       uncached = 0;

        for (i = 0; i < 3u; i++) {
            golden_inputs(&d[i], &env, &k);
        }
        d[1].kernarg_va = 0x00000140ABCD4000ull;
        d[2].kernarg_va = 0x00000140ABCD6000ull;
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 3u, &env, batch, BATCH_MAX, &batch_count),
                     BC250HSA_OK);
        walk_segments(batch, batch_count, &s);
        cached = s.dwords[1];
        CHECK_U64(cached, SEG_NEW_KERNARG_DWORDS);
        CHECK_U64(s.dwords[2], SEG_NEW_KERNARG_DWORDS);

        golden_inputs(&d[0], &env, &k);
        env.flags |= BC250HSA_DISPATCH_FULL_STATE;
        d[1].kernarg_va = 0x00000140ABCD4000ull;
        d[2].kernarg_va = 0x00000140ABCD6000ull;
        CHECK_STATUS(bc250hsa_pm4_build_batch(d, 3u, &env, full, BATCH_MAX, &full_count),
                     BC250HSA_OK);
        walk_segments(full, full_count, &s);
        uncached = s.dwords[1];
        CHECK_U64(uncached, SEG_FULL_DWORDS);
        CHECK_U64(s.dwords[2], SEG_FULL_DWORDS);

        /* 72 dwords a dispatch becomes 23, so the slot holds 711 dispatches instead of
         * 227. Printed and not only asserted, because the number belongs in the
         * evidence of the change and a reader of the test log should not have to
         * divide. */
        printf("test_pm4: dwords a repeated dispatch: %u without the state cache"
               " (%u a slot), %u with it (%u a slot)\n",
               (unsigned)uncached, (unsigned)(usable / uncached), (unsigned)cached,
               (unsigned)(usable / cached));
        CHECK_U64(usable / uncached, 227u);
        CHECK_U64(usable / cached, 711u);
    }
}

/* --------------------------------------------------------------------------------
 * 3. The variants and the refusals
 * ------------------------------------------------------------------------------ */

/* The offset dword of the first SET_SH_REG that names `reg`, or 0 when the stream
 * holds none. It walks the packets, so it also proves the stream is well formed. */
static int stream_writes_reg(const uint32_t* dwords, uint32_t count, uint32_t reg,
                             uint32_t* value_out)
{
    uint32_t at = 0;

    while (at < count) {
        const uint32_t header = dwords[at];
        const uint32_t type = header >> 30;
        uint32_t       payload;
        uint32_t       opcode;

        if (header == BC250HSA_CP_NOP) {
            at++;
            continue;
        }
        if (type != BC250HSA_PACKET_TYPE3) {
            return -1;
        }
        payload = ((header >> 16) & 0x3FFFu) + 1u;
        opcode = (header >> 8) & 0xFFu;
        if (opcode == BC250HSA_PKT3_SET_SH_REG && at + 1u + payload <= count) {
            const uint32_t first = dwords[at + 1];
            const uint32_t n = payload - 1u;
            if (reg >= first && reg < first + n) {
                *value_out = dwords[at + 2u + (reg - first)];
                return 1;
            }
        }
        at += 1u + payload;
    }
    return 0;
}

static void check_variants(void)
{
    bc250hsa_kernel   k;
    bc250hsa_dispatch d;
    bc250hsa_pm4_env  env;
    uint32_t          out[GOLDEN_MAX];
    uint32_t          written = 0;
    uint32_t          baseline = 0;
    uint32_t          value = 0;

    golden_kernel(&k);
    golden_inputs(&d, &env, &k);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &baseline),
                 BC250HSA_OK);

    /* The compute unit mask packets are the two the design drops first if the command
     * processor refuses the opcode: 8 dwords, and the padding takes it back to 8. */
    env.flags = BC250HSA_DISPATCH_GFX_RING | BC250HSA_DISPATCH_NO_CU_MASK;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    CHECK_U64(written, baseline - 8u);
    CHECK_U64(stream_writes_reg(out, written,
                                BC250HSA_REG_COMPUTE_STATIC_THREAD_MGMT_SE0, &value), 0);

    /* Without the fence the stream holds no RELEASE_MEM, and a fence address is then
     * not needed. */
    env.flags = BC250HSA_DISPATCH_GFX_RING | BC250HSA_DISPATCH_NO_FENCE;
    env.fence_va = 0u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    CHECK_U64(written, baseline - 8u);

    /* A fence address that is not 8-byte aligned is refused, and so is a missing one. */
    env.flags = BC250HSA_DISPATCH_GFX_RING;
    env.fence_va = 0x00000140ABCD300Cull;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
    env.fence_va = 0u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);

    /* A compute ring would need no CONTEXT_CONTROL. Node 0 of this part is the
     * graphics ring, so the flag is always set by the device path; the builder must
     * still be able to leave it out. */
    golden_inputs(&d, &env, &k);
    env.flags = 0u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    CHECK(out[0] != BC250HSA_PACKET3(BC250HSA_PKT3_CONTEXT_CONTROL, 1u));

    /* The initiator of the golden stream: the shader enable and the wave size of this
     * kernel, and no other bit. The dword is read out of the stream and not out of
     * the builder's own constants. */
    golden_inputs(&d, &env, &k);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    CHECK_U64(out[72], (uint32_t)COMPUTE_DISPATCH_INITIATOR__COMPUTE_SHADER_EN_MASK |
                           (uint32_t)COMPUTE_DISPATCH_INITIATOR__CS_W32_EN_MASK);

    /* FORCE_START_AT_000 only changes the initiator. */
    golden_inputs(&d, &env, &k);
    env.flags = BC250HSA_DISPATCH_GFX_RING | BC250HSA_DISPATCH_START_AT_000;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    CHECK_U64(written, baseline);
    CHECK_U64(out[72], (uint32_t)COMPUTE_DISPATCH_INITIATOR__COMPUTE_SHADER_EN_MASK |
                           (uint32_t)COMPUTE_DISPATCH_INITIATOR__CS_W32_EN_MASK |
                           (uint32_t)COMPUTE_DISPATCH_INITIATOR__FORCE_START_AT_000_MASK);

    /* Decision 3: the local memory size is computed by the host and written into
     * COMPUTE_PGM_RSRC2, because the kernel descriptor holds 0. reduce256 has 1024
     * bytes, which is two granules of 512. */
    golden_inputs(&d, &env, &k);
    k.group_segment_bytes = 1024u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    CHECK(stream_writes_reg(out, written, BC250HSA_REG_COMPUTE_PGM_RSRC1 + 1u, &value) == 1);
    CHECK_U64(value, 0x0000008Cu | (2u << BC250HSA_RSRC2_LDS_SIZE_SHIFT));
    /* The dynamic part is added, and a descriptor that already held a stale field is
     * overwritten and not merged. */
    d.launch.dynamic_group_bytes = 512u;
    k.compute_pgm_rsrc2 = 0x0000008Cu | (0x1FFu << BC250HSA_RSRC2_LDS_SIZE_SHIFT);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    CHECK(stream_writes_reg(out, written, BC250HSA_REG_COMPUTE_PGM_RSRC1 + 1u, &value) == 1);
    CHECK_U64(value, 0x0000008Cu | (3u << BC250HSA_RSRC2_LDS_SIZE_SHIFT));

    /* The capacity is honoured: one dword short is BC250HSA_ENOMEM, not a write past
     * the buffer. */
    golden_inputs(&d, &env, &k);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, baseline - 1u, &written),
                 BC250HSA_ENOMEM);

    /* A padding that is not a power of two is refused. */
    golden_inputs(&d, &env, &k);
    env.ib_pad_dwords = 6u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);

    /* An unknown structure size is refused before anything else. */
    golden_inputs(&d, &env, &k);
    env.struct_bytes = (uint32_t)sizeof(env) - 4u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
    golden_inputs(&d, &env, &k);
    d.launch.struct_bytes = 0u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
}

static void check_refusals(void)
{
    bc250hsa_kernel   k;
    bc250hsa_dispatch d;
    bc250hsa_pm4_env  env;
    uint32_t          out[GOLDEN_MAX];
    uint32_t          written = 0;

    /* A zero grid or block. */
    golden_kernel(&k);
    golden_inputs(&d, &env, &k);
    d.launch.grid[1] = 0u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
    golden_inputs(&d, &env, &k);
    d.launch.block[2] = 0u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);

    /* A workgroup above the kernel's own maximum. */
    golden_inputs(&d, &env, &k);
    d.launch.block[0] = 1024u;
    d.launch.block[1] = 2u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);

    /* Local memory above the device limit, through the shared checker. */
    golden_inputs(&d, &env, &k);
    k.group_segment_bytes = 65536u + 512u;
    CHECK_STATUS(bc250hsa_pm4_check_dispatch(&d, 65536u), BC250HSA_EINVAL);
    k.group_segment_bytes = 65536u;
    CHECK_STATUS(bc250hsa_pm4_check_dispatch(&d, 65536u), BC250HSA_OK);

    /* A fixed-private kernel with no SCRATCH_EN descriptor contract is refused. */
    golden_inputs(&d, &env, &k);
    k.private_segment_bytes = 256u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EUNSUPPORTED);
    golden_kernel(&k);
    k.uses_dynamic_stack = 1u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EUNSUPPORTED);

    /* A wave64 kernel, and a kernel whose metadata and descriptor disagree about its
     * own wave size. CS_W32_EN is the only place a dispatch states the wave size, so
     * neither is launched with a guess. */
    golden_kernel(&k);
    golden_inputs(&d, &env, &k);
    k.kernel_code_properties = 0x0409u & ~(uint16_t)BC250HSA_KCP_WAVEFRONT_SIZE32;
    k.wave_size = 64u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EUNSUPPORTED);
    golden_kernel(&k);
    golden_inputs(&d, &env, &k);
    k.wave_size = 64u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EUNSUPPORTED);

    /* A kernel argument buffer that is not aligned to the kernel's own requirement,
     * and a kernel that needs one and got none. */
    golden_kernel(&k);
    golden_inputs(&d, &env, &k);
    d.kernarg_va = 0x00000140ABCD2008ull;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
    golden_inputs(&d, &env, &k);
    d.kernarg_va = 0u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);

    /* A user SGPR item this build does not program. */
    golden_kernel(&k);
    golden_inputs(&d, &env, &k);
    /* The dispatch pointer with no AQL packet behind it. The kernel reads its own
     * blockDim through that register, so a dispatch with no packet is refused by name
     * and not started with a register that points nowhere (section 7.1 of the header,
     * defect BD-110). */
    k.kernel_code_properties = 0x0409u | BC250HSA_KCP_DISPATCH_PTR;
    k.user_sgpr_count = 8u;
    CHECK_U64(d.dispatch_packet_va, 0u);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EUNSUPPORTED);
    /* The same kernel with a packet: the plan grows by two registers, and they hold the
     * packet's address in the documented order (private segment buffer, dispatch
     * pointer, kernel argument pointer). */
    {
        bc250hsa_user_sgpr_plan plan;
        plan.struct_bytes = (uint32_t)sizeof(plan);
        CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, 0x00000140ABCD2000ull,
                                              0x00000140ABCD3040ull,
                                              env.private_segment_rsrc, &plan),
                     BC250HSA_OK);
        CHECK_U64(plan.count, 8u);
        CHECK_U64(plan.value[3], BC250HSA_BUFFER_RSRC_W3);
        CHECK_U64(plan.value[4], 0xABCD3040u);
        CHECK_U64(plan.value[5], 0x00000140u);
        CHECK_U64(plan.value[6], 0xABCD2000u);
        CHECK_U64(plan.value[7], 0x00000140u);
    }
    d.dispatch_packet_va = 0x0000014000003040ull;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_OK);
    /* An address that is not 64-byte aligned is not an AQL packet. */
    d.dispatch_packet_va = 0x0000014000003048ull;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
    d.dispatch_packet_va = 0u;

    /* A plan that does not fill exactly the registers the prologue reads. */
    golden_kernel(&k);
    k.user_sgpr_count = 7u;
    {
        bc250hsa_user_sgpr_plan plan;
        plan.struct_bytes = (uint32_t)sizeof(plan);
        CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, 0x1000u, 0u, env.private_segment_rsrc, &plan),
                     BC250HSA_EUNSUPPORTED);
    }
    golden_kernel(&k);
    {
        bc250hsa_user_sgpr_plan plan;
        plan.struct_bytes = (uint32_t)sizeof(plan) + 4u;
        CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, 0x1000u, 0u, env.private_segment_rsrc, &plan),
                     BC250HSA_EINVAL);
        plan.struct_bytes = (uint32_t)sizeof(plan);
        CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, 0x00000140ABCD2000ull, 0u,
                                              env.private_segment_rsrc, &plan),
                     BC250HSA_OK);
        CHECK_U64(plan.count, 6u);
        CHECK_U64(plan.value[0], 0x00001000u);
        CHECK_U64(plan.value[3], BC250HSA_BUFFER_RSRC_W3);
        CHECK_U64(plan.value[4], 0xABCD2000u);
        CHECK_U64(plan.value[5], 0x00000140u);
    }

    /* A null parameter never faults. */
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(NULL, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, NULL, out, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, NULL, GOLDEN_MAX, &written),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, NULL),
                 BC250HSA_EINVAL);
}

typedef struct scratch_allocator_test {
    uint32_t allocations;
    uint32_t frees;
    uint32_t fail;
    uint32_t malformed;
} scratch_allocator_test;

static bc250hsa_status scratch_test_alloc(void* ctx, uint64_t bytes, uint64_t alignment,
                                          uint32_t flags, bc250hsa_mem* out)
{
    scratch_allocator_test* a = (scratch_allocator_test*)ctx;
    CHECK_U64(alignment, 4096u);
    CHECK_U64(flags, BC250HSA_MEM_DEVICE);
    if (a->fail) { return BC250HSA_ENOMEM; }
    a->allocations++;
    memset(out, 0, sizeof(*out));
    out->va = 0x4000000000ull + (uint64_t)a->allocations * 0x10000000ull;
    if (a->malformed) { out->va++; }
    out->bytes = bytes;
    out->opaque = (void*)(uintptr_t)a->allocations;
    return BC250HSA_OK;
}

static void scratch_test_free(void* ctx, bc250hsa_mem* mem)
{
    scratch_allocator_test* a = (scratch_allocator_test*)ctx;
    a->frees++;
    memset(mem, 0, sizeof(*mem));
}

static uint32_t scratch_reg(const uint32_t* stream, uint32_t count, uint32_t reg)
{
    uint32_t at = 0u;
    while (at < count) {
        uint32_t op = (stream[at] >> 8) & 255u;
        uint32_t body = ((stream[at] >> 16) & 0x3FFFu) + 1u;
        if (op == BC250HSA_PKT3_NOP) { at++; continue; }
        if (body > count - at - 1u) { break; }
        if (op == BC250HSA_PKT3_SET_SH_REG && body >= 2u && stream[at + 1u] == reg) {
            return stream[at + 2u];
        }
        at += body + 1u;
    }
    return UINT32_MAX;
}

typedef struct close_test {
    bc250hsa_status flush_status, wait_status;
    uint32_t order, release_count, quarantine_count;
} close_test;
static bc250hsa_status test_close_flush(void* ctx)
{
    close_test* t = (close_test*)ctx;
    t->order = t->order * 10u + 1u;
    return t->flush_status;
}
static bc250hsa_status test_close_wait(void* ctx)
{
    close_test* t = (close_test*)ctx;
    t->order = t->order * 10u + 2u;
    return t->wait_status;
}
static void test_close_release(void* ctx)
{
    close_test* t = (close_test*)ctx;
    t->order = t->order * 10u + 3u;
    t->release_count++;
}
static void test_close_quarantine(void* ctx, bc250hsa_status status)
{
    close_test* t = (close_test*)ctx;
    CHECK(status != BC250HSA_OK);
    t->order = t->order * 10u + 4u;
    t->quarantine_count++;
}
static void check_close_retirement(void)
{
    close_test t;
    bc250hsa_close_ops ops = { &t, test_close_flush, test_close_wait,
                                test_close_release, test_close_quarantine };
    memset(&t, 0, sizeof(t));
    bc250hsa_finish_close(&ops);
    CHECK_U64(t.order, 123u);
    CHECK_U64(t.release_count, 1u);
    CHECK_U64(t.quarantine_count, 0u);
    memset(&t, 0, sizeof(t));
    t.flush_status = BC250HSA_EDEVICELOST;
    bc250hsa_finish_close(&ops);
    CHECK_U64(t.order, 14u);
    CHECK_U64(t.release_count, 0u);
    CHECK_U64(t.quarantine_count, 1u);
    memset(&t, 0, sizeof(t));
    t.wait_status = BC250HSA_ETIMEOUT;
    bc250hsa_finish_close(&ops);
    CHECK_U64(t.order, 124u);
    CHECK_U64(t.release_count, 0u);
    CHECK_U64(t.quarantine_count, 1u);
    memset(&t, 0, sizeof(t));
    t.wait_status = BC250HSA_EDEVICELOST;
    bc250hsa_finish_close(&ops);
    CHECK_U64(t.order, 124u);
    CHECK_U64(t.release_count, 0u);
    CHECK_U64(t.quarantine_count, 1u);
}

static void check_scratch(void)
{
    bc250hsa_scratch_plan plan, larger;
    uint32_t resource[4], out[GOLDEN_MAX], written = 0u;
    bc250hsa_kernel k;
    bc250hsa_dispatch d, batch[2];
    bc250hsa_pm4_env env;
    bc250hsa_user_sgpr_plan sgprs;
    scratch_allocator_test a = { 0, 0, 0, 0 };
    bc250hsa_allocator allocator = { &a, scratch_test_alloc, scratch_test_free };
    bc250hsa_mem slots[2];
    uint64_t old_va;
    memset(slots, 0, sizeof(slots));
    CHECK_U64(BC250HSA_SCRATCH_EN, COMPUTE_PGM_RSRC2__SCRATCH_EN_MASK);
    CHECK_U64(BC250HSA_SCRATCH_WAVESIZE_SHIFT, COMPUTE_TMPRING_SIZE__WAVESIZE__SHIFT);
    CHECK_U64(BC250HSA_SCRATCH_WAVESIZE_MASK << BC250HSA_SCRATCH_WAVESIZE_SHIFT,
              COMPUTE_TMPRING_SIZE__WAVESIZE_MASK);
    CHECK_U64(BC250HSA_SCRATCH_FORMAT_32_FLOAT, BUF_FMT_32_FLOAT);
    CHECK_STATUS(bc250hsa_plan_scratch(0u, 32u, &plan), BC250HSA_OK);
    CHECK_U64(plan.bytes, 0u);
    CHECK_STATUS(bc250hsa_plan_scratch(1u, 32u, &plan), BC250HSA_OK);
    CHECK_U64(plan.bytes_per_thread, 4u);
    CHECK_U64(plan.bytes_per_wave, 1024u);
    CHECK_U64(plan.bytes, 32768u);
    CHECK_U64(plan.tmpring_size, (1u << 12) | 32u);
    CHECK_STATUS(bc250hsa_plan_scratch(3516u, 32u, &plan), BC250HSA_OK);
    CHECK_U64(plan.bytes_per_wave, 112640u);
    CHECK_U64(plan.bytes, 3604480u);
    CHECK_STATUS(bc250hsa_scratch_resource(0x140ABCD2000ull, plan.bytes, &plan, 32u, resource), BC250HSA_OK);
    CHECK_U64(resource[0], 0xABCD2000u);
    CHECK_U64(resource[1], 0x80000140u);
    CHECK_U64(resource[2], UINT32_MAX);
    CHECK_U64(resource[3], 0x31C16000u);
    CHECK_STATUS(bc250hsa_scratch_resource(0x140ABCD2000ull, plan.bytes - 1u, &plan, 32u, resource), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_scratch_resource(0x140ABCD2001ull, plan.bytes, &plan, 32u, resource), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_scratch_resource((1ull << 48) - 4096u, plan.bytes, &plan, 32u, resource), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_plan_scratch(3516u, 64u, &larger), BC250HSA_OK);
    CHECK_U64(larger.bytes_per_wave, 225280u);
    CHECK_STATUS(bc250hsa_scratch_resource(0x140ABCD2000ull, larger.bytes, &larger, 64u, resource), BC250HSA_OK);
    CHECK_U64(resource[3], 0x31E16000u);
    CHECK_STATUS(bc250hsa_plan_scratch(262112u, 32u, &larger), BC250HSA_OK);
    CHECK_U64(larger.tmpring_size >> 12, 8191u);
    CHECK_STATUS(bc250hsa_plan_scratch(262113u, 32u, &larger), BC250HSA_EUNSUPPORTED);
    CHECK_STATUS(bc250hsa_plan_scratch(UINT32_MAX, 32u, &larger), BC250HSA_EUNSUPPORTED);
    CHECK_STATUS(bc250hsa_plan_scratch(1u, 16u, &larger), BC250HSA_EINVAL);

    /* Two live slots never alias. A pending fence and removal cannot cause a free;
     * allocation failure preserves ownership, and only retirement permits growth. */
    CHECK_STATUS(bc250hsa_prepare_scratch(&slots[0], 0u, 0u, &plan, &allocator), BC250HSA_OK);
    CHECK_STATUS(bc250hsa_prepare_scratch(&slots[1], 0u, 0u, &plan, &allocator), BC250HSA_OK);
    CHECK(slots[0].va != slots[1].va);
    old_va = slots[0].va;
    CHECK_STATUS(bc250hsa_plan_scratch(4096u, 32u, &larger), BC250HSA_OK);
    CHECK_STATUS(bc250hsa_prepare_scratch(&slots[0], 7u, 6u, &larger, &allocator), BC250HSA_EBUSY);
    CHECK_STATUS(bc250hsa_prepare_scratch(&slots[0], 7u, UINT64_MAX, &larger, &allocator), BC250HSA_EDEVICELOST);
    CHECK_U64(a.allocations, 2u);
    CHECK_U64(a.frees, 0u);
    a.fail = 1u;
    CHECK_STATUS(bc250hsa_prepare_scratch(&slots[0], 7u, 7u, &larger, &allocator), BC250HSA_ENOMEM);
    CHECK_U64(slots[0].va, old_va);
    a.fail = 0u;
    CHECK_STATUS(bc250hsa_prepare_scratch(&slots[0], 7u, 7u, &larger, &allocator), BC250HSA_OK);
    CHECK_U64(a.allocations, 3u);
    CHECK_U64(a.frees, 1u);
    CHECK_STATUS(bc250hsa_prepare_scratch(&slots[0], 8u, 8u, &plan, &allocator), BC250HSA_OK);
    CHECK_U64(a.allocations, 3u);
    CHECK_U64(a.frees, 1u);

    /* An allocator claiming success with an unusable VA is not allowed to replace
     * the last valid retired buffer. Only the malformed new allocation is freed. */
    CHECK_STATUS(bc250hsa_plan_scratch(8192u, 32u, &larger), BC250HSA_OK);
    old_va = slots[0].va;
    a.malformed = 1u;
    CHECK_STATUS(bc250hsa_prepare_scratch(&slots[0], 9u, 9u, &larger, &allocator), BC250HSA_EINVAL);
    CHECK_U64(slots[0].va, old_va);
    CHECK_U64(a.frees, 2u);
    a.malformed = 0u;

    golden_kernel(&k);
    golden_inputs(&d, &env, &k);
    k.private_segment_bytes = 3516u;
    k.compute_pgm_rsrc2 |= BC250HSA_SCRATCH_EN;
    k.kernel_code_properties |= BC250HSA_KCP_FLAT_SCRATCH_INIT | BC250HSA_KCP_PRIVATE_SEGMENT_SIZE;
    k.user_sgpr_count += 3u;
    /* Old environment refuses fixed-private without reading beyond its old size. */
    env.struct_bytes = (uint32_t)offsetof(bc250hsa_pm4_env, scratch_va);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written), BC250HSA_EINVAL);
    env.struct_bytes = sizeof(env);
    env.scratch_va = 0x140ABCD2000ull;
    env.scratch_bytes = plan.bytes;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written), BC250HSA_OK);
    CHECK_U64(scratch_reg(out, written, BC250HSA_REG_COMPUTE_TMPRING_SIZE), plan.tmpring_size);
    CHECK_U64(scratch_reg(out, written, BC250HSA_REG_COMPUTE_USER_DATA_0), 0xABCD2000u);
    CHECK_STATUS(bc250hsa_scratch_resource(env.scratch_va, plan.bytes, &plan, 32u, resource), BC250HSA_OK);
    sgprs.struct_bytes = sizeof(sgprs);
    CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, d.kernarg_va, 0u, resource, &sgprs), BC250HSA_OK);
    CHECK_U64(sgprs.value[6], 0xABCD2000u);
    CHECK_U64(sgprs.value[7], 0x140u); /* swizzle flag does not enter FLAT_SCRATCH_INIT */
    CHECK_U64(sgprs.value[8], 3516u);
    k.kernel_code_properties |= BC250HSA_KCP_DISPATCH_PTR;
    k.user_sgpr_count += 2u;
    CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, d.kernarg_va, 0x140FFFF0040ull, resource, &sgprs), BC250HSA_OK);
    CHECK_U64(sgprs.value[4], 0xFFFF0040u);
    CHECK_U64(sgprs.value[5], 0x140u);
    CHECK_U64(sgprs.value[8], 0xABCD2000u);
    CHECK_U64(sgprs.value[9], 0x140u);
    CHECK_U64(sgprs.value[10], 3516u);
    k.kernel_code_properties &= ~(uint16_t)BC250HSA_KCP_DISPATCH_PTR;
    k.user_sgpr_count -= 2u;
    {
        bc250hsa_pm4_writer writer;
        bc250hsa_pm4_state state;
        memset(&state, 0, sizeof(state));
        bc250hsa_pm4_writer_init(&writer, out, GOLDEN_MAX, 0u);
        CHECK_STATUS(bc250hsa_pm4_ib_append(&writer, &d, &env, 1, &state), BC250HSA_OK);
        CHECK_U64(state.tmpring_size, plan.tmpring_size);
        golden_kernel(&k);
        bc250hsa_pm4_writer_init(&writer, out, GOLDEN_MAX, 0u);
        CHECK_STATUS(bc250hsa_pm4_ib_append(&writer, &d, &env, 0, &state), BC250HSA_OK);
        CHECK_U64(scratch_reg(out, writer.count, BC250HSA_REG_COMPUTE_TMPRING_SIZE), 0u);
        CHECK_U64(state.tmpring_size, 0u);
        k.private_segment_bytes = 3516u;
        k.compute_pgm_rsrc2 |= BC250HSA_SCRATCH_EN;
    }
    batch[0] = d; batch[1] = d;
    CHECK_STATUS(bc250hsa_pm4_build_batch(batch, 2u, &env, out, GOLDEN_MAX, &written), BC250HSA_EUNSUPPORTED);
    d.launch.block[0] = 1025u;
    CHECK_STATUS(bc250hsa_pm4_check_dispatch(&d, 65536u), BC250HSA_EINVAL);
    d.launch.block[0] = UINT32_MAX;
    d.launch.block[1] = UINT32_MAX;
    d.launch.block[2] = UINT32_MAX;
    CHECK_STATUS(bc250hsa_pm4_check_dispatch(&d, 65536u), BC250HSA_EINVAL);
    golden_inputs(&d, &env, &k);
    golden_kernel(&k);
    env.struct_bytes = (uint32_t)offsetof(bc250hsa_pm4_env, scratch_va);
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written), BC250HSA_OK);
    CHECK_U64(written, g_golden_count);
    {
        const size_t legacy_bytes = offsetof(bc250hsa_pm4_env, scratch_va);
        void* legacy = malloc(legacy_bytes);
        CHECK(legacy != NULL);
        if (legacy != NULL) {
            memcpy(legacy, &env, legacy_bytes);
            CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, (const bc250hsa_pm4_env*)legacy,
                         out, GOLDEN_MAX, &written), BC250HSA_OK);
            k.private_segment_bytes = 3516u;
            k.compute_pgm_rsrc2 |= BC250HSA_SCRATCH_EN;
            CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, (const bc250hsa_pm4_env*)legacy,
                         out, GOLDEN_MAX, &written), BC250HSA_EINVAL);
            free(legacy);
        }
    }
}

static void check_helpers(void)
{
    uint32_t rsrc[4];

    /* The local memory field: align to 512 bytes, then count granules. */
    CHECK_U64(bc250hsa_lds_size_field(0u, 0u), 0u);
    CHECK_U64(bc250hsa_lds_size_field(1u, 0u), 1u);
    CHECK_U64(bc250hsa_lds_size_field(512u, 0u), 1u);
    CHECK_U64(bc250hsa_lds_size_field(513u, 0u), 2u);
    CHECK_U64(bc250hsa_lds_size_field(1024u, 0u), 2u);
    CHECK_U64(bc250hsa_lds_size_field(0u, 1024u), 2u);
    CHECK_U64(bc250hsa_lds_size_field(1024u, 512u), 3u);
    CHECK_U64(bc250hsa_lds_size_field(65536u, 0u), 128u);

    /* The raw buffer resource of the private segment. */
    CHECK_STATUS(bc250hsa_buffer_resource(0x00000140ABCD2000ull, 0x10000u, rsrc), BC250HSA_OK);
    CHECK_U64(rsrc[0], 0xABCD2000u);
    CHECK_U64(rsrc[1], 0x00000140u);
    CHECK_U64(rsrc[2], 0x00010000u);
    CHECK_U64(rsrc[3], BC250HSA_BUFFER_RSRC_W3);
    /* An address above 48 bits and a size above 32 bits are refused, not truncated. */
    CHECK_STATUS(bc250hsa_buffer_resource(0x0001000000000000ull, 0x1000u, rsrc), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_buffer_resource(0x1000u, 0x100000000ull, rsrc), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_buffer_resource(0x1000u, 0x1000u, NULL), BC250HSA_EINVAL);

    /* The interface version the library reports. */
    CHECK_U64(bc250hsa_abi_version_major(), BC250HSA_ABI_VERSION_MAJOR);
    CHECK_U64(bc250hsa_abi_version_minor(), BC250HSA_ABI_VERSION_MINOR);
}

/* --------------------------------------------------------------------------------
 * The GPU copy and fill (scratch/hip-gpu-copy/CONTRACT.md)
 * ------------------------------------------------------------------------------ */

/* The source address and the destination address of every case below. They are far
 * apart, so no case overlaps by accident, and both are inside the address window of
 * this library (kmt_blobs.h, 0x0000004000000000 and 64 GiB). */
#define COPY_SRC 0x0000004010000000ull
#define COPY_DST 0x0000004020000000ull

static void check_copy_packet(const uint32_t* dwords, uint64_t src, uint64_t dst,
                              uint32_t bytes, int raw_wait, int cp_sync, int is_fill)
{
    uint32_t control = is_fill ? (uint32_t)(S_501_DST_SEL(V_501_DST_ADDR_USING_L2) |
                                            S_501_SRC_SEL(2))
                               : (uint32_t)(S_501_DST_SEL(V_501_DST_ADDR_USING_L2) |
                                            S_501_SRC_SEL(V_501_SRC_ADDR_USING_L2));
    uint32_t command = (uint32_t)S_506_BYTE_COUNT(bytes);

    if (cp_sync) { control |= (uint32_t)S_501_CP_SYNC(1); }
    if (raw_wait) { command |= (uint32_t)S_506_RAW_WAIT(1); }
    CHECK_U64(dwords[0], (uint32_t)PACKET3(PACKET3_DMA_DATA, 5));
    CHECK_U64(dwords[1], control);
    CHECK_U64(dwords[2], (uint32_t)src);
    CHECK_U64(dwords[3], (uint32_t)(src >> 32));
    CHECK_U64(dwords[4], (uint32_t)dst);
    CHECK_U64(dwords[5], (uint32_t)(dst >> 32));
    CHECK_U64(dwords[6], command);
}

static void check_gpu_copy(void)
{
    uint32_t            buffer[64];
    bc250hsa_pm4_writer w;
    bc250hsa_copy_op    ops[2];
    uint64_t            fences[4];
    uint32_t            slot = 0u;

    /* The dword count: one packet up to the maximum, two above it. */
    CHECK_U64(bc250hsa_pm4_copy_dwords(0u), 0u);
    CHECK_U64(bc250hsa_pm4_copy_dwords(1u), 7u);
    CHECK_U64(bc250hsa_pm4_copy_dwords(BC250HSA_DMA_MAX_BYTES), 7u);
    CHECK_U64(bc250hsa_pm4_copy_dwords(BC250HSA_DMA_MAX_BYTES + 1u), 14u);
    CHECK_U64(bc250hsa_pm4_copy_dwords(2u * BC250HSA_DMA_MAX_BYTES), 14u);
    CHECK_U64(bc250hsa_pm4_copy_dwords(2u * BC250HSA_DMA_MAX_BYTES + 1u), 21u);
    /* The per-token logits copy of Gemma 12B is one packet. */
    CHECK_U64(bc250hsa_pm4_copy_dwords(1048576u), 7u);

    /* One copy of 1 MiB: seven dwords, RAW_WAIT on the first chunk, CP_SYNC on the
     * last, both addresses as given. */
    memset(buffer, 0xCC, sizeof(buffer));
    bc250hsa_pm4_writer_init(&w, buffer, TEST_COUNT(buffer), 0u);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_DST, COPY_SRC, 1048576u, 1, 1),
                 BC250HSA_OK);
    CHECK_U64(w.count, 7u);
    CHECK_U64((uint64_t)w.overflow, 0u);
    check_copy_packet(buffer, COPY_SRC, COPY_DST, 1048576u, 1, 1, 0);

    /* A copy one byte above the packet maximum: two packets, the first with RAW_WAIT
     * and no CP_SYNC, the second with CP_SYNC and no RAW_WAIT, and both addresses
     * advanced by the bytes the first packet moved. */
    memset(buffer, 0xCC, sizeof(buffer));
    bc250hsa_pm4_writer_init(&w, buffer, TEST_COUNT(buffer), 0u);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_DST, COPY_SRC,
                                          BC250HSA_DMA_MAX_BYTES + 1u, 1, 1),
                 BC250HSA_OK);
    CHECK_U64(w.count, 14u);
    check_copy_packet(buffer, COPY_SRC, COPY_DST, BC250HSA_DMA_MAX_BYTES, 1, 0, 0);
    check_copy_packet(buffer + 7, COPY_SRC + BC250HSA_DMA_MAX_BYTES,
                      COPY_DST + BC250HSA_DMA_MAX_BYTES, 1u, 0, 1, 0);

    /* A fill: the same packet with SRC_SEL DATA, the pattern in dword 3 and zero in
     * dword 4. The destination of a fill advances and the pattern does not. */
    memset(buffer, 0xCC, sizeof(buffer));
    bc250hsa_pm4_writer_init(&w, buffer, TEST_COUNT(buffer), 0u);
    CHECK_STATUS(bc250hsa_pm4_append_fill(&w, COPY_DST, 0xA5A5A5A5u,
                                          BC250HSA_DMA_MAX_BYTES + 4u, 1, 1),
                 BC250HSA_OK);
    CHECK_U64(w.count, 14u);
    check_copy_packet(buffer, 0xA5A5A5A5u, COPY_DST, BC250HSA_DMA_MAX_BYTES, 1, 0, 1);
    check_copy_packet(buffer + 7, 0xA5A5A5A5u, COPY_DST + BC250HSA_DMA_MAX_BYTES, 4u, 0, 1,
                      1);

    /* Without the two flags neither bit is set, which is what a copy in the middle of
     * a list of spans needs. */
    memset(buffer, 0xCC, sizeof(buffer));
    bc250hsa_pm4_writer_init(&w, buffer, TEST_COUNT(buffer), 0u);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_DST, COPY_SRC, 64u, 0, 0), BC250HSA_OK);
    check_copy_packet(buffer, COPY_SRC, COPY_DST, 64u, 0, 0, 0);

    /* The refusals. Every one of them leaves the writer untouched. */
    bc250hsa_pm4_writer_init(&w, buffer, TEST_COUNT(buffer), 0u);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_DST, COPY_SRC, 0u, 1, 1), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, 0u, COPY_SRC, 64u, 1, 1), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_DST, 0u, 64u, 1, 1), BC250HSA_EINVAL);
    /* An address at or above 2^40, which dword 6 of the packet cannot carry. */
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, 0x0000010000000000ull, COPY_SRC, 64u, 1, 1),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_DST, 0x0000010000000000ull, 64u, 1, 1),
                 BC250HSA_EINVAL);
    /* Overlapping ranges, in both directions and touching by one byte. */
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_SRC + 32u, COPY_SRC, 64u, 1, 1),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_SRC, COPY_SRC + 32u, 64u, 1, 1),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_SRC + 64u, COPY_SRC, 64u, 1, 1),
                 BC250HSA_OK);
    /* A fill that is not 4-byte aligned in address or in size. */
    bc250hsa_pm4_writer_init(&w, buffer, TEST_COUNT(buffer), 0u);
    CHECK_STATUS(bc250hsa_pm4_append_fill(&w, COPY_DST + 1u, 0u, 16u, 1, 1), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_append_fill(&w, COPY_DST, 0u, 15u, 1, 1), BC250HSA_EINVAL);
    CHECK_U64(w.count, 0u);
    CHECK_U64((uint64_t)w.overflow, 0u);

    /* A buffer with no room writes nothing and says overflow, which is how submit.c
     * learns to submit the open indirect buffer and append the list again. */
    bc250hsa_pm4_writer_init(&w, buffer, 6u, 0u);
    CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_DST, COPY_SRC, 64u, 1, 1), BC250HSA_OK);
    CHECK_U64((uint64_t)w.overflow, 1u);

    /* A list of operations in order, which is what a row-by-row copy becomes. */
    ops[0].kind = BC250HSA_COPY_OP_COPY;
    ops[0].pattern = 0u;
    ops[0].src_va = COPY_SRC;
    ops[0].dst_va = COPY_DST;
    ops[0].bytes = 128u;
    ops[1].kind = BC250HSA_COPY_OP_FILL;
    ops[1].pattern = 0x01010101u;
    ops[1].src_va = 0u;
    ops[1].dst_va = COPY_DST + 4096u;
    ops[1].bytes = 256u;
    memset(buffer, 0xCC, sizeof(buffer));
    bc250hsa_pm4_writer_init(&w, buffer, TEST_COUNT(buffer), 0u);
    CHECK_STATUS(bc250hsa_pm4_append_ops(&w, ops, 2u), BC250HSA_OK);
    CHECK_U64(w.count, 14u);
    check_copy_packet(buffer, COPY_SRC, COPY_DST, 128u, 1, 1, 0);
    check_copy_packet(buffer + 7, 0x01010101u, COPY_DST + 4096u, 256u, 1, 1, 1);
    /* An unknown kind is a refusal and not a packet. */
    ops[1].kind = 7u;
    bc250hsa_pm4_writer_init(&w, buffer, TEST_COUNT(buffer), 0u);
    CHECK_STATUS(bc250hsa_pm4_append_ops(&w, ops, 2u), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_append_ops(&w, ops, 0u), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_pm4_append_ops(&w, NULL, 1u), BC250HSA_EINVAL);

    /* The overlap test itself. */
    CHECK_U64((uint64_t)bc250hsa_ranges_overlap(100u, 100u, 0u), 0u);
    CHECK_U64((uint64_t)bc250hsa_ranges_overlap(100u, 100u, 1u), 1u);
    CHECK_U64((uint64_t)bc250hsa_ranges_overlap(100u, 164u, 64u), 0u);
    CHECK_U64((uint64_t)bc250hsa_ranges_overlap(100u, 163u, 64u), 1u);
    CHECK_U64((uint64_t)bc250hsa_ranges_overlap(164u, 100u, 64u), 0u);
    CHECK_U64((uint64_t)bc250hsa_ranges_overlap(163u, 100u, 64u), 1u);

    /* The staging ring: the round size, the wrap, and the state in which every slot is
     * still in flight. */
    CHECK_U64(bc250hsa_stage_round(0u, 1024u), 0u);
    CHECK_U64(bc250hsa_stage_round(1u, 1024u), 1u);
    CHECK_U64(bc250hsa_stage_round(1024u, 1024u), 1024u);
    CHECK_U64(bc250hsa_stage_round(1025u, 1024u), 1024u);
    CHECK_U64(bc250hsa_stage_round(1025u, 0u), 0u);

    /* Nothing has been written yet: slot 0. */
    fences[0] = 0u; fences[1] = 0u; fences[2] = 0u; fences[3] = 0u;
    CHECK_STATUS(bc250hsa_stage_pick(fences, 4u, 0u, 0u, &slot), BC250HSA_OK);
    CHECK_U64(slot, 0u);
    /* Slot 2 is next in turn and its fence retired. */
    fences[0] = 10u; fences[1] = 11u; fences[2] = 12u; fences[3] = 13u;
    CHECK_STATUS(bc250hsa_stage_pick(fences, 4u, 12u, 2u, &slot), BC250HSA_OK);
    CHECK_U64(slot, 2u);
    /* Slot 2 and slot 3 are still in flight, so the search wraps to slot 0. */
    CHECK_STATUS(bc250hsa_stage_pick(fences, 4u, 11u, 2u, &slot), BC250HSA_OK);
    CHECK_U64(slot, 0u);
    /* Every slot is in flight: the caller is told to wait, and the slot it is given is
     * the one whose fence it must wait for. */
    CHECK_STATUS(bc250hsa_stage_pick(fences, 4u, 9u, 1u, &slot), BC250HSA_EBUSY);
    CHECK_U64(slot, 1u);
    /* A removed device never hands out a slot. */
    CHECK_STATUS(bc250hsa_stage_pick(fences, 4u, UINT64_MAX, 0u, &slot),
                 BC250HSA_EDEVICELOST);
    CHECK_STATUS(bc250hsa_stage_pick(NULL, 4u, 0u, 0u, &slot), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_stage_pick(fences, 0u, 0u, 0u, &slot), BC250HSA_EINVAL);
    /* A ring of one slot is legal: every round waits for the round before it. */
    fences[0] = 5u;
    CHECK_STATUS(bc250hsa_stage_pick(fences, 1u, 5u, 0u, &slot), BC250HSA_OK);
    CHECK_U64(slot, 0u);
    CHECK_STATUS(bc250hsa_stage_pick(fences, 1u, 4u, 0u, &slot), BC250HSA_EBUSY);
}

/* The split a fill is built from, against a memset oracle.
 *
 * This is the arithmetic that decides which bytes a fill packet carries and which bytes a
 * copy packet out of a staging slot carries, and it is the part of kmt_memory.c that a host
 * test can reach: an edge off by one byte would leave a hipMemset short or write past its
 * range, on a part where no allocation with a host mapping would show it. Every start
 * offset in a dword and every size up to six dwords is compared with what memset does to
 * the same range. */
static void check_fill_split(void)
{
    unsigned char      want[64];
    unsigned char      got[64];
    bc250hsa_fill_plan plan;
    uint32_t           offset;
    uint32_t           bytes;

    CHECK_STATUS(bc250hsa_fill_split(COPY_DST, 0u, &plan), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_fill_split(0u, 16u, &plan), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_fill_split(COPY_DST, 16u, NULL), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_fill_split(UINT64_MAX, 2u, &plan), BC250HSA_EINVAL);

    for (offset = 0u; offset < 8u; offset++) {
        for (bytes = 1u; bytes <= 24u; bytes++) {
            const uint64_t va = COPY_DST + offset;

            CHECK_STATUS(bc250hsa_fill_split(va, bytes, &plan), BC250HSA_OK);
            /* The three parts tile the range in address order, with no gap between two of
             * them and no byte in two of them. */
            CHECK_U64(plan.head_va, va);
            CHECK_U64(plan.interior_va, plan.head_va + plan.head_bytes);
            CHECK_U64(plan.tail_va, plan.interior_va + plan.interior_bytes);
            CHECK_U64(plan.tail_va + plan.tail_bytes, va + bytes);
            /* What each packet can carry: an edge is under one dword, and the interior is
             * 4-byte aligned in address and in size (bc250hsa_pm4_append_fill). A part of
             * 0 bytes is never appended, so only a part with bytes must be aligned. */
            CHECK_U64((uint64_t)(plan.head_bytes < 4u), 1u);
            CHECK_U64((uint64_t)(plan.tail_bytes < 4u), 1u);
            CHECK_U64(plan.interior_bytes % 4u, 0u);
            if (plan.interior_bytes != 0u) {
                CHECK_U64(plan.interior_va % 4u, 0u);
            }
            if (plan.tail_bytes != 0u) {
                CHECK_U64(plan.tail_va % 4u, 0u);
            }
            /* And the bytes themselves: filling the three parts equals one memset of the
             * whole range, and touches nothing outside it. */
            memset(want, 0x11, sizeof(want));
            memset(got, 0x11, sizeof(got));
            memset(want + offset, 0xA7, (size_t)bytes);
            memset(got + (size_t)(plan.head_va - COPY_DST), 0xA7, (size_t)plan.head_bytes);
            memset(got + (size_t)(plan.interior_va - COPY_DST), 0xA7,
                   (size_t)plan.interior_bytes);
            memset(got + (size_t)(plan.tail_va - COPY_DST), 0xA7, (size_t)plan.tail_bytes);
            CHECK_U64((uint64_t)memcmp(want, got, sizeof(want)), 0u);
        }
    }

    /* The hipMemset of a tensor: aligned in both, so the interior alone and no slot. */
    CHECK_STATUS(bc250hsa_fill_split(COPY_DST, 1048576u, &plan), BC250HSA_OK);
    CHECK_U64(plan.head_bytes, 0u);
    CHECK_U64(plan.interior_bytes, 1048576u);
    CHECK_U64(plan.tail_bytes, 0u);
    /* A range that holds no whole dword is one edge and no fill packet at all. */
    CHECK_STATUS(bc250hsa_fill_split(COPY_DST + 1u, 2u, &plan), BC250HSA_OK);
    CHECK_U64(plan.head_bytes, 2u);
    CHECK_U64(plan.interior_bytes, 0u);
    CHECK_U64(plan.tail_bytes, 0u);
}

/* The restatement gate of pm4_regs.h, carried as far as the stream.
 *
 * driver/kmd/gfx_copy.c emits the same packet for the kernel-mode path, from the same Mesa
 * field macros, and it is the one other place in this repository that holds this encoding.
 * The field macros are compared above; this compares the dwords, so that a divergence in
 * the chunking, in the address advance or in which chunk carries RAW_WAIT and CP_SYNC is a
 * build failure. The fill has no counterpart there: that emitter copies only. */
static void check_against_kernel_emitter(void)
{
    static const uint32_t sizes[] = { 1u, 4u, 64u, 4096u, 1048576u,
                                      BC250HSA_DMA_MAX_BYTES,
                                      BC250HSA_DMA_MAX_BYTES + 1u,
                                      2u * BC250HSA_DMA_MAX_BYTES + 17u };
    uint32_t            ours[32];
    uint32_t            theirs[32];
    bc250hsa_pm4_writer w;
    size_t              i;
    int                 wait_before;
    int                 sync_after;

    CHECK_U64(Bc250GfxCopyMaxBytes(), BC250HSA_DMA_MAX_BYTES);
    for (i = 0; i < TEST_COUNT(sizes); i++) {
        CHECK_U64(Bc250GfxCopyDwords(sizes[i]), bc250hsa_pm4_copy_dwords(sizes[i]));
        for (wait_before = 0; wait_before < 2; wait_before++) {
            for (sync_after = 0; sync_after < 2; sync_after++) {
                unsigned int wrote;

                memset(theirs, 0xCC, sizeof(theirs));
                memset(ours, 0xCC, sizeof(ours));
                wrote = Bc250EmitGfxCopySpan(theirs, (unsigned int)TEST_COUNT(theirs),
                                             COPY_SRC, COPY_DST, sizes[i], wait_before,
                                             sync_after);
                CHECK_U64(wrote, bc250hsa_pm4_copy_dwords(sizes[i]));
                bc250hsa_pm4_writer_init(&w, ours, TEST_COUNT(ours), 0u);
                CHECK_STATUS(bc250hsa_pm4_append_copy(&w, COPY_DST, COPY_SRC, sizes[i],
                                                      wait_before, sync_after),
                             BC250HSA_OK);
                CHECK_U64(w.count, wrote);
                CHECK_U64((uint64_t)memcmp(ours, theirs, (size_t)wrote * 4u), 0u);
            }
        }
    }
}

int main(int argc, char** argv)
{
    const char* dir = test_data_dir(argc, argv);

    check_against_linux_headers();
    check_against_kernel_emitter();
    check_gpu_copy();
    check_fill_split();
    if (read_golden(dir)) {
        check_golden();
    }
    check_batch();
    check_state_cache();
    check_variants();
    check_refusals();
    check_helpers();
    check_scratch();
    check_close_retirement();
    return test_report("test_pm4");
}
