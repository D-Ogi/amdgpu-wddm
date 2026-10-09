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

    /* A kernel that spills. Open question 4 of the design: a scratch ring is not
     * measured on this silicon, so it is refused by name. */
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
    k.kernel_code_properties = 0x0409u | BC250HSA_KCP_DISPATCH_PTR;
    k.user_sgpr_count = 8u;
    CHECK_STATUS(bc250hsa_pm4_build_dispatch(&d, &env, out, GOLDEN_MAX, &written),
                 BC250HSA_EUNSUPPORTED);

    /* A plan that does not fill exactly the registers the prologue reads. */
    golden_kernel(&k);
    k.user_sgpr_count = 7u;
    {
        bc250hsa_user_sgpr_plan plan;
        plan.struct_bytes = (uint32_t)sizeof(plan);
        CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, 0x1000u, env.private_segment_rsrc, &plan),
                     BC250HSA_EUNSUPPORTED);
    }
    golden_kernel(&k);
    {
        bc250hsa_user_sgpr_plan plan;
        plan.struct_bytes = (uint32_t)sizeof(plan) + 4u;
        CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, 0x1000u, env.private_segment_rsrc, &plan),
                     BC250HSA_EINVAL);
        plan.struct_bytes = (uint32_t)sizeof(plan);
        CHECK_STATUS(bc250hsa_plan_user_sgprs(&k, 0x00000140ABCD2000ull,
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

int main(int argc, char** argv)
{
    const char* dir = test_data_dir(argc, argv);

    check_against_linux_headers();
    if (read_golden(dir)) {
        check_golden();
    }
    check_batch();
    check_state_cache();
    check_variants();
    check_refusals();
    check_helpers();
    return test_report("test_pm4");
}
