/* test_kernarg.c - the kernel argument packer against the metadata of the committed
 * code object.
 *
 * Section 5.4 of docs/design/m16-hip-route-b.md: this test must catch a packer that
 * uses a fixed offset for a hidden field, that gets the grid in workgroups and the
 * implicit block in work items the wrong way round, or that leaves a hidden field with
 * whatever the buffer held before.
 *
 * The measured reason for the first of those: the three kernels of one code object have
 * kernarg_segment_size 28, 16 and 264, and only the kernel that reads the implicit
 * argument pointer gets an implicit block at all. The packer therefore walks the
 * .args list, and this test walks the same list to state what it expects.
 */
#include "test_common.h"

/* ENABLE_SGPR_DISPATCH_PTR of the kernel descriptor. The library keeps the name in its
 * own private header, which a host test does not include, so the bit is written down
 * here with the field it belongs to (AMDGPUUsage.rst, the kernel descriptor table). */
#define TEST_KCP_DISPATCH_PTR 0x0002u

#define KERNARG_MAX 512u

static uint8_t g_buffer[KERNARG_MAX];

static uint32_t read_u16(uint32_t at)
{
    uint16_t v = 0;
    memcpy(&v, g_buffer + at, 2);
    return v;
}

static uint32_t read_u32(uint32_t at)
{
    uint32_t v = 0;
    memcpy(&v, g_buffer + at, 4);
    return v;
}

static uint64_t read_u64(uint32_t at)
{
    uint64_t v = 0;
    memcpy(&v, g_buffer + at, 8);
    return v;
}

static void zero_result(bc250hsa_pack_result* r)
{
    memset(r, 0, sizeof(*r));
    r->struct_bytes = (uint32_t)sizeof(*r);
}

static void launch_of(bc250hsa_launch* l, uint32_t gx, uint32_t gy, uint32_t gz, uint32_t bx,
                      uint32_t by, uint32_t bz)
{
    memset(l, 0, sizeof(*l));
    l->struct_bytes = (uint32_t)sizeof(*l);
    l->grid[0] = gx; l->grid[1] = gy; l->grid[2] = gz;
    l->block[0] = bx; l->block[1] = by; l->block[2] = bz;
}

/* --------------------------------------------------------------------------------
 * vadd: four explicit arguments, no implicit block
 * ------------------------------------------------------------------------------ */

static void check_vadd(const struct bc250hsa_module* mod)
{
    const bc250hsa_kernel* k = bc250hsa_module_kernel_by_name(mod, "vadd");
    bc250hsa_launch        launch;
    bc250hsa_pack_result   result;
    uint64_t               a = 0x0000004000010000ull;
    uint64_t               b = 0x0000004000020000ull;
    uint64_t               c = 0x0000004000030000ull;
    int32_t                n = 1048576;
    void*                  args[4];
    uint32_t               bytes = 0;
    uint32_t               alignment = 0;

    CHECK(k != NULL);
    if (k == NULL) {
        return;
    }
    args[0] = &a; args[1] = &b; args[2] = &c; args[3] = &n;

    CHECK_STATUS(bc250hsa_kernarg_requirements(k, &bytes, &alignment), BC250HSA_OK);
    CHECK_U64(bytes, 28u);
    CHECK_U64(alignment, 16u);

    launch_of(&launch, 4096u, 1u, 1u, 256u, 1u, 1u);
    zero_result(&result);
    memset(g_buffer, 0xEE, sizeof(g_buffer));
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 4u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_OK);
    CHECK_U64(result.bytes_written, 28u);
    CHECK_U64(result.explicit_args_written, 4u);
    CHECK_U64(result.hidden_args_zeroed, 0u);
    CHECK_U64(result.unknown_arg_kinds, 0u);
    CHECK_U64(result.hostcall_buffer_requested, 0u);

    /* The three pointers and the scalar, at the offsets the metadata states. */
    CHECK_U64(read_u64(0u), a);
    CHECK_U64(read_u64(8u), b);
    CHECK_U64(read_u64(16u), c);
    CHECK_U64(read_u32(24u), (uint32_t)n);
    /* Nothing past kernarg_segment_size is touched. */
    CHECK_U64(g_buffer[28u], 0xEEu);

    /* The wrong number of arguments is refused, in both directions. */
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 3u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_EINVAL);
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 5u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_EINVAL);
    /* A buffer smaller than kernarg_segment_size is refused before any write. */
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 4u, g_buffer, 27u, 0u, &result),
                 BC250HSA_EINVAL);
    /* A null argument pointer in the array is refused, not read. */
    zero_result(&result);
    args[2] = NULL;
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 4u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_EINVAL);
    args[2] = &c;
    /* An unknown result size, and the null parameters. */
    zero_result(&result);
    result.struct_bytes = (uint32_t)sizeof(result) - 4u;
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 4u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_EINVAL);
    zero_result(&result);
    launch.struct_bytes = 0u;
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 4u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_EINVAL);
    launch_of(&launch, 4096u, 1u, 1u, 256u, 1u, 1u);
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(NULL, &launch, args, 4u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, NULL, args, 4u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 4u, NULL, KERNARG_MAX, 0u, &result),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 4u, g_buffer, KERNARG_MAX, 0u, NULL),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_kernarg_requirements(NULL, &bytes, &alignment), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_kernarg_requirements(k, NULL, &alignment), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_kernarg_requirements(k, &bytes, NULL), BC250HSA_EINVAL);
}

/* --------------------------------------------------------------------------------
 * reduce256: two explicit arguments, and local memory the packer does not touch
 * ------------------------------------------------------------------------------ */

static void check_reduce256(const struct bc250hsa_module* mod)
{
    const bc250hsa_kernel* k = bc250hsa_module_kernel_by_name(mod, "reduce256");
    bc250hsa_launch        launch;
    bc250hsa_pack_result   result;
    uint64_t               in = 0x0000004000040000ull;
    uint64_t               out = 0x0000004000050000ull;
    void*                  args[2];

    CHECK(k != NULL);
    if (k == NULL) {
        return;
    }
    args[0] = &in; args[1] = &out;
    launch_of(&launch, 64u, 1u, 1u, 256u, 1u, 1u);
    zero_result(&result);
    memset(g_buffer, 0xEE, sizeof(g_buffer));
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 2u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_OK);
    CHECK_U64(result.bytes_written, 16u);
    CHECK_U64(result.explicit_args_written, 2u);
    CHECK_U64(read_u64(0u), in);
    CHECK_U64(read_u64(8u), out);
    CHECK_U64(g_buffer[16u], 0xEEu);
}

/* --------------------------------------------------------------------------------
 * writeGridSize: one explicit argument and the whole implicit block
 * ------------------------------------------------------------------------------ */

static void check_write_grid_size(const struct bc250hsa_module* mod)
{
    const bc250hsa_kernel* k = bc250hsa_module_kernel_by_name(mod, "writeGridSize");
    bc250hsa_launch        launch;
    bc250hsa_pack_result   result;
    uint64_t               out = 0x0000004000060000ull;
    void*                  args[1];
    uint32_t               at;

    CHECK(k != NULL);
    if (k == NULL) {
        return;
    }
    args[0] = &out;

    /* The offsets this test states come from the metadata of the committed object:
     * block_count at 8, 12, 16; group_size at 20, 22, 24; remainder at 26, 28, 30;
     * global_offset at 48, 56, 64; grid_dims at 72. Two of them share a boundary,
     * which is why the packer may never use a structure. */
    CHECK_U64(k->kernarg_bytes, 264u);
    CHECK_U64(k->explicit_arg_count, 1u);
    CHECK_U64(k->hidden_arg_count, 13u);

    launch_of(&launch, 7u, 3u, 2u, 64u, 2u, 1u);
    zero_result(&result);
    memset(g_buffer, 0xEE, sizeof(g_buffer));
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 1u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_OK);
    CHECK_U64(result.bytes_written, 264u);
    CHECK_U64(result.explicit_args_written, 1u);
    CHECK_U64(result.hidden_args_zeroed, 0u);
    CHECK_U64(result.unknown_arg_kinds, 0u);

    CHECK_U64(read_u64(0u), out);
    /* The work-group count of the dispatch, which is the launch grid itself.
     * AMDGPUUsage.rst: hidden_block_count_* is "not the same as the value in the AQL
     * dispatch packet, which has the grid size in work-items". A test that expects
     * grid times block here would make every kernel that reads gridDim wrong. */
    CHECK_U64(read_u32(8u), 7u);
    CHECK_U64(read_u32(12u), 3u);
    CHECK_U64(read_u32(16u), 2u);
    /* The workgroup size itself, in 16-bit fields. */
    CHECK_U64(read_u16(20u), 64u);
    CHECK_U64(read_u16(22u), 2u);
    CHECK_U64(read_u16(24u), 1u);
    /* No workgroup of a HIP grid is partial, so every remainder is 0. */
    CHECK_U64(read_u16(26u), 0u);
    CHECK_U64(read_u16(28u), 0u);
    CHECK_U64(read_u16(30u), 0u);
    /* HIP has no global offset. */
    CHECK_U64(read_u64(48u), 0u);
    CHECK_U64(read_u64(56u), 0u);
    CHECK_U64(read_u64(64u), 0u);
    /* The third dimension is used, so the kernel sees three. */
    CHECK_U64(read_u16(72u), 3u);
    /* Every byte the metadata does not name is zero, and nothing past the segment
     * size is touched. */
    for (at = 32u; at < 48u; at++) {
        CHECK_U64(g_buffer[at], 0u);
    }
    for (at = 74u; at < 264u; at++) {
        CHECK_U64(g_buffer[at], 0u);
    }
    CHECK_U64(g_buffer[264u], 0xEEu);

    /* The number of dimensions follows the launch, from the grid or from the block. */
    launch_of(&launch, 4096u, 1u, 1u, 256u, 1u, 1u);
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 1u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_OK);
    CHECK_U64(read_u16(72u), 1u);
    launch_of(&launch, 4096u, 2u, 1u, 256u, 1u, 1u);
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 1u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_OK);
    CHECK_U64(read_u16(72u), 2u);
    launch_of(&launch, 4096u, 1u, 1u, 256u, 1u, 2u);
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(k, &launch, args, 1u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_OK);
    CHECK_U64(read_u16(72u), 3u);
}

/* --------------------------------------------------------------------------------
 * The hidden fields no fixture kernel asks for
 * ------------------------------------------------------------------------------ */

static void check_hidden_fields(void)
{
    /* A hand-made kernel, so that the fields a clang build without a device library
     * never emits are tested as well: the dynamic local memory size, the host call
     * buffer (kill criterion K4) and a key this build does not know. */
    static const bc250hsa_arg args_list[] = {
        { 0u,  8u, (uint16_t)BC250HSA_ARG_GLOBAL_BUFFER,            BC250HSA_AS_GLOBAL, 8u },
        { 8u,  4u, (uint16_t)BC250HSA_ARG_HIDDEN_DYNAMIC_LDS_SIZE,  BC250HSA_AS_NONE,   4u },
        { 16u, 8u, (uint16_t)BC250HSA_ARG_HIDDEN_HOSTCALL_BUFFER,   BC250HSA_AS_GLOBAL, 8u },
        { 24u, 8u, (uint16_t)BC250HSA_ARG_HIDDEN_PRINTF_BUFFER,     BC250HSA_AS_GLOBAL, 8u },
        { 32u, 8u, (uint16_t)BC250HSA_ARG_HIDDEN_OTHER,             BC250HSA_AS_NONE,   8u },
        { 40u, 8u, (uint16_t)BC250HSA_ARG_HIDDEN_MULTIGRID_SYNC_ARG, BC250HSA_AS_GLOBAL, 8u }
    };
    bc250hsa_kernel      k;
    bc250hsa_launch      launch;
    bc250hsa_pack_result result;
    bc250hsa_counters    before;
    bc250hsa_counters    after;
    uint64_t             buffer_va = 0x0000004000070000ull;
    void*                args[1];
    uint32_t             at;

    memset(&k, 0, sizeof(k));
    k.name = "hidden_fields";
    k.kernarg_bytes = 48u;
    k.kernarg_align = 16u;
    k.max_flat_workgroup_size = 1024u;
    k.arg_count = TEST_COUNT(args_list);
    k.explicit_arg_count = 1u;
    k.hidden_arg_count = TEST_COUNT(args_list) - 1u;
    k.args = args_list;
    args[0] = &buffer_va;

    before.struct_bytes = (uint32_t)sizeof(before);
    CHECK_STATUS(bc250hsa_counters_read(&before), BC250HSA_OK);

    launch_of(&launch, 8u, 1u, 1u, 128u, 1u, 1u);
    launch.dynamic_group_bytes = 2048u;
    zero_result(&result);
    memset(g_buffer, 0xEE, sizeof(g_buffer));
    CHECK_STATUS(bc250hsa_kernarg_pack(&k, &launch, args, 1u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_OK);
    CHECK_U64(result.bytes_written, 48u);
    CHECK_U64(result.explicit_args_written, 1u);
    /* The host call buffer, the printf buffer, the multigrid argument and the
     * unknown key are zeroed and counted. The dynamic local memory size is filled,
     * so it is not counted. */
    CHECK_U64(result.hidden_args_zeroed, 4u);
    CHECK_U64(result.unknown_arg_kinds, 1u);
    CHECK_U64(result.hostcall_buffer_requested, 1u);
    CHECK(strcmp(result.first_unknown_kind, "arg#4") == 0);
    CHECK_U64(read_u64(0u), buffer_va);
    CHECK_U64(read_u32(8u), 2048u);
    for (at = 16u; at < 48u; at++) {
        CHECK_U64(g_buffer[at], 0u);
    }

    after.struct_bytes = (uint32_t)sizeof(after);
    CHECK_STATUS(bc250hsa_counters_read(&after), BC250HSA_OK);
    CHECK_U64(after.hostcall_buffer_requests - before.hostcall_buffer_requests, 1u);
    CHECK_U64(after.unknown_arg_kinds - before.unknown_arg_kinds, 1u);
    CHECK_U64(after.hidden_args_zeroed - before.hidden_args_zeroed, 4u);

    /* An argument that reaches past kernarg_segment_size is a broken note, and the
     * packer says so instead of writing outside the buffer. */
    k.kernarg_bytes = 44u;
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(&k, &launch, args, 1u, g_buffer, KERNARG_MAX, 0u, &result),
                 BC250HSA_EBADMETADATA);

    /* A counters structure of an unknown size is refused. */
    after.struct_bytes = 0u;
    CHECK_STATUS(bc250hsa_counters_read(&after), BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_counters_read(NULL), BC250HSA_EINVAL);
}

/* ------------------------------------------------------------------------------
 * The AQL dispatch packet of section 7.1
 *
 * A kernel that reads its own blockDim enables ENABLE_SGPR_DISPATCH_PTR, and a PM4
 * path has to write the packet that register points at. MEASURED on the ggml-hip
 * backend: 1752 of its 7105 gfx1013 kernels enable the bit (defect BD-110). This
 * checks the size the caller must allocate and every field the packet holds.
 * ------------------------------------------------------------------------------ */

static void check_dispatch_packet(void)
{
    static const bc250hsa_arg args_list[] = {
        { 0u, 8u, (uint16_t)BC250HSA_ARG_GLOBAL_BUFFER, BC250HSA_AS_GLOBAL, 8u }
    };
    bc250hsa_kernel      k;
    bc250hsa_launch      launch;
    bc250hsa_pack_result result;
    uint64_t             buffer_va = 0x0000004000070000ull;
    void*                args[1];
    uint32_t             bytes = 0;
    uint32_t             alignment = 0;
    const uint64_t       kernarg_va = 0x0000004000080000ull;
    uint32_t             at;

    memset(&k, 0, sizeof(k));
    k.name = "reads_blockdim";
    k.descriptor_va = 0x00000040000A0000ull;
    k.kernarg_bytes = 24u;
    k.kernarg_align = 16u;
    k.group_segment_bytes = 1024u;
    k.max_flat_workgroup_size = 1024u;
    k.arg_count = TEST_COUNT(args_list);
    k.explicit_arg_count = 1u;
    k.args = args_list;
    args[0] = &buffer_va;

    /* Without the bit the requirement is the kernel argument block alone. */
    CHECK_STATUS(bc250hsa_kernarg_requirements(&k, &bytes, &alignment), BC250HSA_OK);
    CHECK_U64(bytes, 24u);
    CHECK_U64(alignment, 16u);

    /* With it the buffer grows by the packet, at the packet's own alignment. */
    k.kernel_code_properties = (uint16_t)TEST_KCP_DISPATCH_PTR;
    CHECK_STATUS(bc250hsa_kernarg_requirements(&k, &bytes, &alignment), BC250HSA_OK);
    CHECK_U64(bytes, BC250HSA_AQL_PACKET_ALIGN + BC250HSA_AQL_PACKET_BYTES);
    CHECK_U64(alignment, BC250HSA_AQL_PACKET_ALIGN);

    /* A buffer that holds the arguments but not the packet is refused. */
    launch_of(&launch, 7u, 3u, 2u, 64u, 4u, 1u);
    launch.dynamic_group_bytes = 512u;
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(&k, &launch, args, 1u, g_buffer, 24u, kernarg_va,
                                       &result),
                 BC250HSA_EINVAL);

    zero_result(&result);
    memset(g_buffer, 0xEE, sizeof(g_buffer));
    CHECK_STATUS(bc250hsa_kernarg_pack(&k, &launch, args, 1u, g_buffer, KERNARG_MAX, kernarg_va,
                                       &result),
                 BC250HSA_OK);
    CHECK_U64(result.dispatch_packet_requested, 1u);
    CHECK_U64(result.dispatch_packet_offset, BC250HSA_AQL_PACKET_ALIGN);
    CHECK_U64(result.bytes_written, BC250HSA_AQL_PACKET_ALIGN + BC250HSA_AQL_PACKET_BYTES);
    CHECK_U64(read_u64(0u), buffer_va);
    /* The gap between the arguments and the packet is zero and not what the last
     * launch out of a pooled buffer left there. */
    for (at = 24u; at < BC250HSA_AQL_PACKET_ALIGN; at++) {
        CHECK_U64(g_buffer[at], 0u);
    }
    {
        const uint32_t p = BC250HSA_AQL_PACKET_ALIGN;
        /* header: a kernel dispatch packet, the barrier bit, both fences at system
         * scope. setup: three dimensions, because block.y and grid.z are above 1. */
        CHECK_U64(read_u16(p + 0u), 2u | (1u << 8) | (2u << 9) | (2u << 11));
        CHECK_U64(read_u16(p + 2u), 3u);
        CHECK_U64(read_u16(p + 4u), 64u);
        CHECK_U64(read_u16(p + 6u), 4u);
        CHECK_U64(read_u16(p + 8u), 1u);
        CHECK_U64(read_u16(p + 10u), 0u);         /* reserved0 */
        /* The grid in work items, where bc250hsa_launch states it in workgroups. */
        CHECK_U64(read_u32(p + 12u), 7u * 64u);
        CHECK_U64(read_u32(p + 16u), 3u * 4u);
        CHECK_U64(read_u32(p + 20u), 2u * 1u);
        CHECK_U64(read_u32(p + 24u), 0u);         /* private segment, no scratch here */
        CHECK_U64(read_u32(p + 28u), 1024u + 512u);
        CHECK_U64(read_u64(p + 32u), k.descriptor_va);
        CHECK_U64(read_u64(p + 40u), kernarg_va);
        CHECK_U64(read_u64(p + 48u), 0u);         /* reserved2 */
        CHECK_U64(read_u64(p + 56u), 0u);         /* completion_signal: our own fence */
    }

    /* A one-dimensional launch says one dimension, and the packet is written again
     * from the new launch and not left as it was. */
    launch_of(&launch, 5u, 1u, 1u, 32u, 1u, 1u);
    zero_result(&result);
    CHECK_STATUS(bc250hsa_kernarg_pack(&k, &launch, args, 1u, g_buffer, KERNARG_MAX, 0u,
                                       &result),
                 BC250HSA_OK);
    {
        const uint32_t p = BC250HSA_AQL_PACKET_ALIGN;
        CHECK_U64(read_u16(p + 2u), 1u);
        CHECK_U64(read_u32(p + 12u), 5u * 32u);
        CHECK_U64(read_u32(p + 16u), 1u);
        CHECK_U64(read_u32(p + 28u), 1024u);
        CHECK_U64(read_u64(p + 40u), 0u);   /* a plain host buffer has no GPU address */
    }
}

int main(int argc, char** argv)
{
    const char*             dir = test_data_dir(argc, argv);
    bc250hsa_allocator      alloc;
    struct bc250hsa_module* mod = NULL;
    void*                   image;
    size_t                  image_bytes = 0;

    image = test_read_file(dir, "m16_kernels.gfx1013.co", &image_bytes);
    if (image != NULL) {
        test_allocator(&alloc);
        CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, image, image_bytes, &mod),
                     BC250HSA_OK);
        if (mod != NULL) {
            check_vadd(mod);
            check_reduce256(mod);
            check_write_grid_size(mod);
            bc250hsa_module_unload(mod);
        }
        free(image);
    }
    check_hidden_fields();
    check_dispatch_packet();
    return test_report("test_kernarg");
}
