/* kernarg.c - the kernel argument buffer, from the metadata .args list and from
 * nothing else.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, section 3.6. The packer never computes
 * an offset of its own. The measured reason: a kernel gets the implicit block only
 * when it reads the implicit argument pointer, and when the block exists the compiler
 * packs only the fields the kernel needs, so no fixed structure can describe it.
 * vadd has kernarg_segment_size 28 (four explicit arguments and no block);
 * writeGridSize has 264.
 */
#include "internal.h"

static void write_u16(uint8_t* at, uint32_t value) { const uint16_t v = (uint16_t)value; memcpy(at, &v, 2); }
static void write_u32(uint8_t* at, uint32_t value) { memcpy(at, &value, 4); }
static void write_u64(uint8_t* at, uint64_t value) { memcpy(at, &value, 8); }

/* Does this kernel read the AQL dispatch packet? The compiler says so with
 * ENABLE_SGPR_DISPATCH_PTR, and section 7.1 of the header says why a PM4 path has to
 * write a packet for it. */
static int wants_dispatch_packet(const bc250hsa_kernel* kernel)
{
    return (kernel->kernel_code_properties & BC250HSA_KCP_DISPATCH_PTR) != 0u;
}

/* Where the packet goes: behind the kernel arguments, at its own 64-byte alignment. */
static uint32_t dispatch_packet_offset(const bc250hsa_kernel* kernel)
{
    return (uint32_t)bc250hsa_align_up_u64((uint64_t)kernel->kernarg_bytes,
                                           BC250HSA_AQL_PACKET_ALIGN);
}

bc250hsa_status bc250hsa_kernarg_requirements(const bc250hsa_kernel* kernel, uint32_t* bytes,
                                              uint32_t* alignment)
{
    if (kernel == NULL || bytes == NULL || alignment == NULL) {
        return BC250HSA_EINVAL;
    }
    *bytes = kernel->kernarg_bytes;
    *alignment = kernel->kernarg_align;
    if (wants_dispatch_packet(kernel)) {
        /* The packet is part of the launch, so it shares the launch's one allocation
         * and its one lifetime (section 7.1). The buffer therefore grows by the packet
         * and its alignment, and the whole buffer is aligned at least as strictly as
         * the packet needs, so the offset below lands on a 64-byte boundary. */
        *bytes = dispatch_packet_offset(kernel) + BC250HSA_AQL_PACKET_BYTES;
        if (*alignment < BC250HSA_AQL_PACKET_ALIGN) {
            *alignment = BC250HSA_AQL_PACKET_ALIGN;
        }
    }
    return BC250HSA_OK;
}

/* What hidden_block_count_* carries. AMDGPUUsage.rst, the code object v5 kernel argument
 * metadata table, is exact about this field: "The grid dispatch work-group count for the
 * X dimension is passed in the kernarg ... This is not the same as the value in the AQL
 * dispatch packet, which has the grid size in work-items." So this is the number of
 * workgroups, which is exactly bc250hsa_launch.grid, and not the grid in work items.
 *
 * A kernel of the usual "if (i < n)" shape never notices the difference, because it
 * reads only the work-item identifier. Every kernel that reads gridDim does. */
static uint32_t block_count(const bc250hsa_launch* launch, uint32_t dim)
{
    return launch->grid[dim];
}

/* The AQL kernel dispatch packet of the HSA interface, 64 bytes, field by field. The
 * offsets come from the HSA Platform System Architecture specification, version 1.2,
 * table 2-9 (hsa_kernel_dispatch_packet_t), and LLVM reads the same offsets:
 * AMDGPUUsage.rst, "Kernel Dispatch", gives the workgroup size at bytes 4 to 9 and the
 * grid size in work items at bytes 12 to 23. */
#define AQL_OFF_HEADER            0u
#define AQL_OFF_SETUP             2u
#define AQL_OFF_WORKGROUP_SIZE_X  4u
#define AQL_OFF_RESERVED0        10u
#define AQL_OFF_GRID_SIZE_X      12u
#define AQL_OFF_PRIVATE_SEGMENT  24u
#define AQL_OFF_GROUP_SEGMENT    28u
#define AQL_OFF_KERNEL_OBJECT    32u
#define AQL_OFF_KERNARG_ADDRESS  40u
#define AQL_OFF_RESERVED2        48u
#define AQL_OFF_COMPLETION       56u

/* header: one kernel dispatch packet, with the barrier bit set and both memory fences
 * at system scope. The command processor of a PM4 path never reads this field, and a
 * kernel that reads it must see a well-formed packet and not zero. */
#define AQL_PACKET_TYPE_KERNEL_DISPATCH 2u
#define AQL_HEADER_TYPE_SHIFT     0u
#define AQL_HEADER_BARRIER_SHIFT  8u
#define AQL_HEADER_SCACQUIRE_SHIFT 9u
#define AQL_HEADER_SCRELEASE_SHIFT 11u
#define AQL_FENCE_SCOPE_SYSTEM    2u
#define AQL_SETUP_DIMENSIONS_SHIFT 0u

static uint32_t grid_dims(const bc250hsa_launch* launch)
{
    if (launch->grid[2] > 1u || launch->block[2] > 1u) {
        return 3u;
    }
    if (launch->grid[1] > 1u || launch->block[1] > 1u) {
        return 2u;
    }
    return 1u;
}

/* Fills the 64-byte packet from the launch this dispatch really runs. Everything the
 * packet says is true of that dispatch; nothing is invented. */
static bc250hsa_status write_dispatch_packet(const bc250hsa_kernel* kernel,
                                             const bc250hsa_launch* launch,
                                             uint64_t kernarg_va, uint8_t* at)
{
    uint32_t header;
    uint32_t i;
    uint64_t group_bytes;

    memset(at, 0, BC250HSA_AQL_PACKET_BYTES);
    header = (AQL_PACKET_TYPE_KERNEL_DISPATCH << AQL_HEADER_TYPE_SHIFT) |
             (1u << AQL_HEADER_BARRIER_SHIFT) |
             (AQL_FENCE_SCOPE_SYSTEM << AQL_HEADER_SCACQUIRE_SHIFT) |
             (AQL_FENCE_SCOPE_SYSTEM << AQL_HEADER_SCRELEASE_SHIFT);
    write_u16(at + AQL_OFF_HEADER, header);
    write_u16(at + AQL_OFF_SETUP, grid_dims(launch) << AQL_SETUP_DIMENSIONS_SHIFT);
    for (i = 0; i < 3u; i++) {
        /* The workgroup size is a 16-bit field. A block larger than 65535 in one
         * dimension cannot be stated in a packet, and bc250hsa_pm4_check_dispatch
         * refuses a block product above max_flat_workgroup_size (1024 on this part)
         * before this runs, so the narrowing cannot lose a value in practice. The
         * check is here because the field, and not the caller, sets the limit. */
        if (launch->block[i] > 0xFFFFu) {
            return BC250HSA_EINVAL;
        }
        write_u16(at + AQL_OFF_WORKGROUP_SIZE_X + i * 2u, launch->block[i]);
    }
    for (i = 0; i < 3u; i++) {
        /* The packet states the grid in work items, where bc250hsa_launch states it in
         * workgroups (the note above block_count says where the two differ). */
        const uint64_t items = (uint64_t)launch->grid[i] * (uint64_t)launch->block[i];
        if (items > 0xFFFFFFFFull) {
            return BC250HSA_EINVAL;
        }
        write_u32(at + AQL_OFF_GRID_SIZE_X + i * 4u, (uint32_t)items);
    }
    write_u32(at + AQL_OFF_PRIVATE_SEGMENT, kernel->private_segment_bytes);
    group_bytes = (uint64_t)kernel->group_segment_bytes + launch->dynamic_group_bytes;
    if (group_bytes > 0xFFFFFFFFull) {
        return BC250HSA_EINVAL;
    }
    write_u32(at + AQL_OFF_GROUP_SEGMENT, (uint32_t)group_bytes);
    write_u64(at + AQL_OFF_KERNEL_OBJECT, kernel->descriptor_va);
    write_u64(at + AQL_OFF_KERNARG_ADDRESS, kernarg_va);
    /* reserved0, reserved2 and completion_signal stay zero: this layer completes
     * through its own fence and has no HSA signal to name. */
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_kernarg_pack(const bc250hsa_kernel* kernel,
                                      const bc250hsa_launch* launch, void* const* args,
                                      uint32_t arg_count, void* kernarg,
                                      uint32_t kernarg_bytes, uint64_t kernarg_va,
                                      bc250hsa_pack_result* result)
{
    uint8_t* base = (uint8_t*)kernarg;
    uint32_t explicit_index = 0;
    uint32_t i;
    uint32_t needed_bytes = 0;
    uint32_t needed_align = 0;

    if (kernel == NULL || launch == NULL || kernarg == NULL || result == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!bc250hsa_struct_bytes_ok(launch->struct_bytes, sizeof(*launch)) ||
        !bc250hsa_struct_bytes_ok(result->struct_bytes, sizeof(*result))) {
        return BC250HSA_EINVAL;
    }
    if (bc250hsa_kernarg_requirements(kernel, &needed_bytes, &needed_align) != BC250HSA_OK) {
        return BC250HSA_EINVAL;
    }
    if (kernarg_bytes < needed_bytes) {
        return BC250HSA_EINVAL;
    }
    if (kernel->explicit_arg_count != arg_count) {
        bc250hsa_log(BC250HSA_LOG_ERROR, "kernel %s wants %u arguments, the launch gave %u",
                     kernel->name, kernel->explicit_arg_count, arg_count);
        return BC250HSA_EINVAL;
    }
    if (arg_count != 0u && args == NULL) {
        return BC250HSA_EINVAL;
    }

    result->bytes_written = kernel->kernarg_bytes;
    result->explicit_args_written = 0;
    result->hidden_args_zeroed = 0;
    result->unknown_arg_kinds = 0;
    result->hostcall_buffer_requested = 0;
    result->dispatch_packet_requested = 0;
    result->dispatch_packet_offset = 0;
    result->first_unknown_kind[0] = '\0';

    /* Everything starts zero. A hidden field this build does not fill then holds
     * zero by construction, which is the documented value for all of them, and the
     * alignment gap in front of the dispatch packet holds zero rather than whatever
     * the last launch out of this pooled buffer left there. */
    memset(base, 0, needed_bytes);

    for (i = 0; i < kernel->arg_count; i++) {
        const bc250hsa_arg* arg = &kernel->args[i];
        uint8_t*            at;

        if (arg->size == 0u) {
            continue;
        }
        if ((uint64_t)arg->offset + arg->size > (uint64_t)kernel->kernarg_bytes) {
            bc250hsa_log(BC250HSA_LOG_ERROR, "kernel %s argument %u is outside kernarg_segment_size",
                         kernel->name, i);
            return BC250HSA_EBADMETADATA;
        }
        at = base + arg->offset;

        switch ((bc250hsa_arg_kind)arg->kind) {
        case BC250HSA_ARG_BY_VALUE:
        case BC250HSA_ARG_GLOBAL_BUFFER:
        case BC250HSA_ARG_DYNAMIC_SHARED_POINTER:
        case BC250HSA_ARG_SAMPLER:
        case BC250HSA_ARG_IMAGE:
        case BC250HSA_ARG_PIPE:
        case BC250HSA_ARG_QUEUE:
            /* An explicit argument. hipLaunchKernel gives one pointer per
             * parameter in declaration order; the packer walks the list and the
             * array in step. A global_buffer takes the device pointer by value,
             * which is why its size is 8. */
            if (explicit_index >= arg_count || args[explicit_index] == NULL) {
                return BC250HSA_EINVAL;
            }
            memcpy(at, args[explicit_index], arg->size);
            explicit_index++;
            result->explicit_args_written++;
            break;

        case BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X:
        case BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Y:
        case BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Z: {
            const uint32_t dim = (uint32_t)arg->kind - (uint32_t)BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X;
            if (arg->size != 4u) { return BC250HSA_EBADMETADATA; }
            write_u32(at, block_count(launch, dim));
            break;
        }
        case BC250HSA_ARG_HIDDEN_GROUP_SIZE_X:
        case BC250HSA_ARG_HIDDEN_GROUP_SIZE_Y:
        case BC250HSA_ARG_HIDDEN_GROUP_SIZE_Z: {
            const uint32_t dim = (uint32_t)arg->kind - (uint32_t)BC250HSA_ARG_HIDDEN_GROUP_SIZE_X;
            if (arg->size != 2u) { return BC250HSA_EBADMETADATA; }
            write_u16(at, launch->block[dim]);
            break;
        }
        case BC250HSA_ARG_HIDDEN_REMAINDER_X:
        case BC250HSA_ARG_HIDDEN_REMAINDER_Y:
        case BC250HSA_ARG_HIDDEN_REMAINDER_Z:
            if (arg->size != 2u) { return BC250HSA_EBADMETADATA; }
            /* 0, always. The field holds the size of a partial last workgroup, which
             * only a language such as OpenCL has. A HIP grid is a count of whole
             * workgroups, so no workgroup of this interface is partial. */
            write_u16(at, 0u);
            break;

        case BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_X:
        case BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Y:
        case BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Z:
            if (arg->size != 8u) { return BC250HSA_EBADMETADATA; }
            write_u64(at, 0u);   /* HIP has no global offset */
            break;

        case BC250HSA_ARG_HIDDEN_GRID_DIMS:
            if (arg->size != 2u) { return BC250HSA_EBADMETADATA; }
            write_u16(at, grid_dims(launch));
            break;

        case BC250HSA_ARG_HIDDEN_DYNAMIC_LDS_SIZE:
            if (arg->size != 4u) { return BC250HSA_EBADMETADATA; }
            write_u32(at, launch->dynamic_group_bytes);
            break;

        case BC250HSA_ARG_HIDDEN_HOSTCALL_BUFFER:
            /* The measurement that answers kill criterion K4, device-side printf.
             * We get the answer from a counter, not from a crash. */
            result->hostcall_buffer_requested = 1u;
            result->hidden_args_zeroed++;
            bc250hsa_count_add(BC250HSA_C_HOSTCALL_BUFFER_REQUESTS, 1u);
            bc250hsa_count_add(BC250HSA_C_HIDDEN_ARGS_ZEROED, 1u);
            break;

        case BC250HSA_ARG_HIDDEN_PRINTF_BUFFER:
        case BC250HSA_ARG_HIDDEN_HEAP_V1:
        case BC250HSA_ARG_HIDDEN_DEFAULT_QUEUE:
        case BC250HSA_ARG_HIDDEN_COMPLETION_ACTION:
        case BC250HSA_ARG_HIDDEN_MULTIGRID_SYNC_ARG:
        case BC250HSA_ARG_HIDDEN_QUEUE_PTR:
        case BC250HSA_ARG_HIDDEN_PRIVATE_BASE:
        case BC250HSA_ARG_HIDDEN_SHARED_BASE:
            /* Zeroed by the memset above, and counted so that a kernel which needs
             * one of these is visible without a debugger. */
            result->hidden_args_zeroed++;
            bc250hsa_count_add(BC250HSA_C_HIDDEN_ARGS_ZEROED, 1u);
            break;

        case BC250HSA_ARG_HIDDEN_OTHER:
        default:
            /* A key this build does not know. Zero fill, count, name it in the log,
             * and continue: a later code object version may add a key. */
            result->unknown_arg_kinds++;
            result->hidden_args_zeroed++;
            bc250hsa_count_add(BC250HSA_C_UNKNOWN_ARG_KINDS, 1u);
            bc250hsa_count_add(BC250HSA_C_HIDDEN_ARGS_ZEROED, 1u);
            if (result->first_unknown_kind[0] == '\0') {
                /* The metadata key itself is not kept per argument, so the report
                 * names the index, which is what a reader needs to find the entry
                 * in the note dump. */
                char     text[16];
                uint32_t n = i;
                uint32_t length = 0;
                uint32_t j;
                do {
                    text[length++] = (char)('0' + (n % 10u));
                    n /= 10u;
                } while (n != 0u && length < (uint32_t)sizeof(text));
                memcpy(result->first_unknown_kind, "arg#", 4);
                for (j = 0; j < length; j++) {
                    result->first_unknown_kind[4u + j] = text[length - 1u - j];
                }
                result->first_unknown_kind[4u + length] = '\0';
            }
            bc250hsa_log(BC250HSA_LOG_WARN, "kernel %s argument %u has an unknown value_kind",
                         kernel->name, i);
            break;
        }
    }

    if (explicit_index != arg_count) {
        return BC250HSA_EINVAL;
    }

    if (wants_dispatch_packet(kernel)) {
        const uint32_t        offset = dispatch_packet_offset(kernel);
        const bc250hsa_status status =
            write_dispatch_packet(kernel, launch, kernarg_va, base + offset);
        if (status != BC250HSA_OK) {
            return status;
        }
        result->dispatch_packet_requested = 1u;
        result->dispatch_packet_offset = offset;
        result->bytes_written = offset + BC250HSA_AQL_PACKET_BYTES;
    }
    return BC250HSA_OK;
}
