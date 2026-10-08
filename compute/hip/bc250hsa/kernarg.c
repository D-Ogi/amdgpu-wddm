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

bc250hsa_status bc250hsa_kernarg_requirements(const bc250hsa_kernel* kernel, uint32_t* bytes,
                                              uint32_t* alignment)
{
    if (kernel == NULL || bytes == NULL || alignment == NULL) {
        return BC250HSA_EINVAL;
    }
    *bytes = kernel->kernarg_bytes;
    *alignment = kernel->kernarg_align;
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

bc250hsa_status bc250hsa_kernarg_pack(const bc250hsa_kernel* kernel,
                                      const bc250hsa_launch* launch, void* const* args,
                                      uint32_t arg_count, void* kernarg,
                                      uint32_t kernarg_bytes, bc250hsa_pack_result* result)
{
    uint8_t* base = (uint8_t*)kernarg;
    uint32_t explicit_index = 0;
    uint32_t i;

    if (kernel == NULL || launch == NULL || kernarg == NULL || result == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!bc250hsa_struct_bytes_ok(launch->struct_bytes, sizeof(*launch)) ||
        !bc250hsa_struct_bytes_ok(result->struct_bytes, sizeof(*result))) {
        return BC250HSA_EINVAL;
    }
    if (kernarg_bytes < kernel->kernarg_bytes) {
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
    result->first_unknown_kind[0] = '\0';

    /* Everything starts zero. A hidden field this build does not fill then holds
     * zero by construction, which is the documented value for all of them. */
    memset(base, 0, kernel->kernarg_bytes);

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
    return BC250HSA_OK;
}
