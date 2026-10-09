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
    check_variants();
    check_refusals();
    check_helpers();
    return test_report("test_pm4");
}
