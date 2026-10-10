/* co_metadata.c - the NT_AMDGPU_METADATA note to bc250hsa_kernel, and the 64-byte
 * kernel descriptor to the fields of the same structure.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, sections 3.4 step 6 and 3.5. Every
 * number here is read out of the object. This file computes one thing only: the
 * kernel argument alignment, which is max(.kernarg_segment_align, 16) by decision 4
 * of section 2.
 */
#include <stdlib.h>

#include "co_internal.h"
#include "internal.h"

/* The .value_kind strings of AMDGPUUsage.rst, in the order of bc250hsa_arg_kind.
 * A key that is absent from this table is BC250HSA_ARG_HIDDEN_OTHER: the packer
 * zeroes it, counts it and names it in the log, because a later code object version
 * may add a key. */
typedef struct arg_kind_row {
    const char*       text;
    bc250hsa_arg_kind kind;
} arg_kind_row;

static const arg_kind_row g_arg_kinds[] = {
    { "by_value",                 BC250HSA_ARG_BY_VALUE },
    { "global_buffer",            BC250HSA_ARG_GLOBAL_BUFFER },
    { "dynamic_shared_pointer",   BC250HSA_ARG_DYNAMIC_SHARED_POINTER },
    { "sampler",                  BC250HSA_ARG_SAMPLER },
    { "image",                    BC250HSA_ARG_IMAGE },
    { "pipe",                     BC250HSA_ARG_PIPE },
    { "queue",                    BC250HSA_ARG_QUEUE },
    { "hidden_block_count_x",     BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X },
    { "hidden_block_count_y",     BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Y },
    { "hidden_block_count_z",     BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Z },
    { "hidden_group_size_x",      BC250HSA_ARG_HIDDEN_GROUP_SIZE_X },
    { "hidden_group_size_y",      BC250HSA_ARG_HIDDEN_GROUP_SIZE_Y },
    { "hidden_group_size_z",      BC250HSA_ARG_HIDDEN_GROUP_SIZE_Z },
    { "hidden_remainder_x",       BC250HSA_ARG_HIDDEN_REMAINDER_X },
    { "hidden_remainder_y",       BC250HSA_ARG_HIDDEN_REMAINDER_Y },
    { "hidden_remainder_z",       BC250HSA_ARG_HIDDEN_REMAINDER_Z },
    { "hidden_global_offset_x",   BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_X },
    { "hidden_global_offset_y",   BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Y },
    { "hidden_global_offset_z",   BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Z },
    { "hidden_grid_dims",         BC250HSA_ARG_HIDDEN_GRID_DIMS },
    { "hidden_dynamic_lds_size",  BC250HSA_ARG_HIDDEN_DYNAMIC_LDS_SIZE },
    { "hidden_printf_buffer",     BC250HSA_ARG_HIDDEN_PRINTF_BUFFER },
    { "hidden_hostcall_buffer",   BC250HSA_ARG_HIDDEN_HOSTCALL_BUFFER },
    { "hidden_heap_v1",           BC250HSA_ARG_HIDDEN_HEAP_V1 },
    { "hidden_default_queue",     BC250HSA_ARG_HIDDEN_DEFAULT_QUEUE },
    { "hidden_completion_action", BC250HSA_ARG_HIDDEN_COMPLETION_ACTION },
    { "hidden_multigrid_sync_arg", BC250HSA_ARG_HIDDEN_MULTIGRID_SYNC_ARG },
    { "hidden_queue_ptr",         BC250HSA_ARG_HIDDEN_QUEUE_PTR },
    { "hidden_private_base",      BC250HSA_ARG_HIDDEN_PRIVATE_BASE },
    { "hidden_shared_base",       BC250HSA_ARG_HIDDEN_SHARED_BASE }
};

static const struct { const char* text; uint8_t value; } g_address_spaces[] = {
    { "private",  BC250HSA_AS_PRIVATE },
    { "global",   BC250HSA_AS_GLOBAL },
    { "constant", BC250HSA_AS_CONSTANT },
    { "local",    BC250HSA_AS_LOCAL },
    { "generic",  BC250HSA_AS_GENERIC }
};

static bc250hsa_arg_kind kind_of(const bc250hsa_mp_value* v)
{
    uint32_t i;
    for (i = 0; i < BC250HSA_ARRAY_COUNT(g_arg_kinds); i++) {
        if (bc250hsa_mp_str_is(v, g_arg_kinds[i].text)) {
            return g_arg_kinds[i].kind;
        }
    }
    return BC250HSA_ARG_HIDDEN_OTHER;
}

static uint8_t address_space_of(const bc250hsa_mp_value* v)
{
    uint32_t i;
    for (i = 0; i < BC250HSA_ARRAY_COUNT(g_address_spaces); i++) {
        if (bc250hsa_mp_str_is(v, g_address_spaces[i].text)) {
            return g_address_spaces[i].value;
        }
    }
    return BC250HSA_AS_NONE;
}

static int is_hidden(uint16_t kind)
{
    return kind >= (uint16_t)BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X;
}

/* AMDGPUUsage defines .name and .symbol as strings without a fixed length.
 * Own their complete bytes: truncating a descriptor symbol could bind another
 * kernel, and a fixed buffer rejects valid long C++ mangled names (BD-110).
 * ELF symbol lookup uses C strings, so embedded NULs are not admissible. */
static bc250hsa_status copy_string(char** dst, const bc250hsa_mp_value* v)
{
    size_t bytes;
    char* out;
    if (v->kind != BC250HSA_MP_STR) {
        return BC250HSA_EBADMETADATA;
    }
    bytes = (size_t)v->as.str.bytes;
    if (bytes == 0u || bytes == SIZE_MAX || memchr(v->as.str.text, '\0', bytes) != NULL) {
        return BC250HSA_EBADMETADATA;
    }
    out = (char*)malloc(bytes + 1u);
    if (out == NULL) {
        return BC250HSA_ENOMEM;
    }
    memcpy(out, v->as.str.text, bytes);
    out[bytes] = '\0';
    *dst = out;
    return BC250HSA_OK;
}

/* --------------------------------------------------------------------------
 * One .args list
 * ------------------------------------------------------------------------ */

static bc250hsa_status parse_args(bc250hsa_mp_reader* r, bc250hsa_kernel_internal* k)
{
    bc250hsa_mp_value list;
    uint32_t          i;

    if (!bc250hsa_mp_next(r, &list) || list.kind != BC250HSA_MP_ARRAY) {
        return BC250HSA_EBADMETADATA;
    }
    k->pub.arg_count = list.as.count;
    k->pub.explicit_arg_count = 0;
    k->pub.hidden_arg_count = 0;
    if (list.as.count == 0u) {
        k->args = NULL;
        k->pub.args = NULL;
        return BC250HSA_OK;
    }
    k->args = (bc250hsa_arg*)calloc(list.as.count, sizeof(bc250hsa_arg));
    if (k->args == NULL) {
        return BC250HSA_ENOMEM;
    }
    k->pub.args = k->args;

    for (i = 0; i < list.as.count; i++) {
        bc250hsa_mp_value entry;
        bc250hsa_arg*     arg = &k->args[i];
        uint32_t          pair;
        int               have_offset = 0;
        int               have_size = 0;
        int               have_kind = 0;

        arg->kind = (uint16_t)BC250HSA_ARG_HIDDEN_OTHER;
        if (!bc250hsa_mp_next(r, &entry) || entry.kind != BC250HSA_MP_MAP) {
            return BC250HSA_EBADMETADATA;
        }
        for (pair = 0; pair < entry.as.count; pair++) {
            bc250hsa_mp_value key;
            bc250hsa_mp_value value;
            uint64_t          number;

            if (!bc250hsa_mp_next(r, &key) || key.kind != BC250HSA_MP_STR) {
                return BC250HSA_EBADMETADATA;
            }
            if (bc250hsa_mp_str_is(&key, ".offset")) {
                if (!bc250hsa_mp_next(r, &value) || !bc250hsa_mp_as_u64(&value, &number) ||
                    number > 0xFFFFFFFFu) {
                    return BC250HSA_EBADMETADATA;
                }
                arg->offset = (uint32_t)number;
                have_offset = 1;
            } else if (bc250hsa_mp_str_is(&key, ".size")) {
                if (!bc250hsa_mp_next(r, &value) || !bc250hsa_mp_as_u64(&value, &number) ||
                    number > 0xFFFFFFFFu) {
                    return BC250HSA_EBADMETADATA;
                }
                arg->size = (uint32_t)number;
                have_size = 1;
            } else if (bc250hsa_mp_str_is(&key, ".value_kind")) {
                if (!bc250hsa_mp_next(r, &value) || value.kind != BC250HSA_MP_STR) {
                    return BC250HSA_EBADMETADATA;
                }
                arg->kind = (uint16_t)kind_of(&value);
                have_kind = 1;
            } else if (bc250hsa_mp_str_is(&key, ".address_space")) {
                if (!bc250hsa_mp_next(r, &value)) {
                    return BC250HSA_EBADMETADATA;
                }
                arg->address_space = address_space_of(&value);
            } else if (bc250hsa_mp_str_is(&key, ".value_align")) {
                if (!bc250hsa_mp_next(r, &value) || !bc250hsa_mp_as_u64(&value, &number) ||
                    number > 0xFFu) {
                    return BC250HSA_EBADMETADATA;
                }
                arg->value_align = (uint8_t)number;
            } else if (!bc250hsa_mp_skip(r)) {
                return BC250HSA_EBADMETADATA;
            }
        }
        if (!have_offset || !have_size || !have_kind) {
            return BC250HSA_EBADMETADATA;
        }
        if (is_hidden(arg->kind)) {
            k->pub.hidden_arg_count++;
        } else {
            k->pub.explicit_arg_count++;
        }
    }
    return BC250HSA_OK;
}

/* --------------------------------------------------------------------------
 * One kernel map
 * ------------------------------------------------------------------------ */

static bc250hsa_status parse_kernel(bc250hsa_mp_reader* r, bc250hsa_kernel_internal* k)
{
    bc250hsa_mp_value map;
    uint32_t          pair;
    uint32_t          kernarg_align = 0;
    int               have_name = 0;
    int               have_symbol = 0;

    if (!bc250hsa_mp_next(r, &map) || map.kind != BC250HSA_MP_MAP) {
        return BC250HSA_EBADMETADATA;
    }
    for (pair = 0; pair < map.as.count; pair++) {
        bc250hsa_mp_value key;
        bc250hsa_mp_value value;
        uint64_t          number;

        if (!bc250hsa_mp_next(r, &key) || key.kind != BC250HSA_MP_STR) {
            return BC250HSA_EBADMETADATA;
        }
        if (bc250hsa_mp_str_is(&key, ".args")) {
            const bc250hsa_status status = parse_args(r, k);
            if (status != BC250HSA_OK) {
                return status;
            }
            continue;
        }
        if (bc250hsa_mp_str_is(&key, ".name")) {
            bc250hsa_status status;
            if (have_name || !bc250hsa_mp_next(r, &value)) {
                return BC250HSA_EBADMETADATA;
            }
            status = copy_string(&k->name, &value);
            if (status != BC250HSA_OK) {
                return status;
            }
            k->pub.name = k->name;
            have_name = 1;
            continue;
        }
        if (bc250hsa_mp_str_is(&key, ".symbol")) {
            bc250hsa_status status;
            if (have_symbol || !bc250hsa_mp_next(r, &value)) {
                return BC250HSA_EBADMETADATA;
            }
            status = copy_string(&k->symbol, &value);
            if (status != BC250HSA_OK) {
                return status;
            }
            have_symbol = 1;
            continue;
        }
        if (!bc250hsa_mp_next(r, &value)) {
            return BC250HSA_EBADMETADATA;
        }
        if (!bc250hsa_mp_as_u64(&value, &number)) {
            /* A string or a container this build does not read, such as
             * .language or .device_enqueue_symbol. Skipping the value is enough:
             * bc250hsa_mp_next already consumed a scalar, and a container needs
             * its children stepped over. */
            if (value.kind == BC250HSA_MP_ARRAY || value.kind == BC250HSA_MP_MAP) {
                uint32_t children;
                uint32_t i;
                if (value.kind == BC250HSA_MP_MAP && value.as.count > UINT32_MAX / 2u) {
                    return BC250HSA_EBADMETADATA;
                }
                children = (value.kind == BC250HSA_MP_MAP) ? value.as.count * 2u : value.as.count;
                for (i = 0; i < children; i++) {
                    if (!bc250hsa_mp_skip(r)) {
                        return BC250HSA_EBADMETADATA;
                    }
                }
            }
            continue;
        }
        if (bc250hsa_mp_str_is(&key, ".group_segment_fixed_size")) {
            k->pub.group_segment_bytes = (uint32_t)number;
        } else if (bc250hsa_mp_str_is(&key, ".private_segment_fixed_size")) {
            k->pub.private_segment_bytes = (uint32_t)number;
        } else if (bc250hsa_mp_str_is(&key, ".kernarg_segment_size")) {
            k->pub.kernarg_bytes = (uint32_t)number;
        } else if (bc250hsa_mp_str_is(&key, ".kernarg_segment_align")) {
            kernarg_align = (uint32_t)number;
        } else if (bc250hsa_mp_str_is(&key, ".max_flat_workgroup_size")) {
            k->pub.max_flat_workgroup_size = (uint32_t)number;
        } else if (bc250hsa_mp_str_is(&key, ".sgpr_count")) {
            k->pub.sgpr_count = (uint16_t)number;
        } else if (bc250hsa_mp_str_is(&key, ".vgpr_count")) {
            k->pub.vgpr_count = (uint16_t)number;
        } else if (bc250hsa_mp_str_is(&key, ".wavefront_size")) {
            k->pub.wave_size = (uint8_t)number;
        } else if (bc250hsa_mp_str_is(&key, ".workgroup_processor_mode")) {
            k->pub.workgroup_processor_mode = (uint8_t)(number != 0u);
        } else if (bc250hsa_mp_str_is(&key, ".uses_dynamic_stack")) {
            k->pub.uses_dynamic_stack = (uint8_t)(number != 0u);
        }
    }
    if (!have_name || !have_symbol) {
        return BC250HSA_EBADMETADATA;
    }
    /* Decision 4 of section 2: the stricter of the metadata value and the
     * documented minimum of 16, reported once so that no caller repeats it. */
    k->pub.kernarg_align = (kernarg_align > 16u) ? kernarg_align : 16u;
    return BC250HSA_OK;
}

/* --------------------------------------------------------------------------
 * The note
 * ------------------------------------------------------------------------ */

bc250hsa_status bc250hsa_metadata_parse(const uint8_t* note, size_t note_bytes,
                                        struct bc250hsa_module* mod)
{
    bc250hsa_mp_reader r;
    bc250hsa_mp_value  root;
    uint32_t           pair;
    int                have_kernels = 0;

    bc250hsa_mp_init(&r, note, note_bytes);
    if (!bc250hsa_mp_next(&r, &root) || root.kind != BC250HSA_MP_MAP) {
        return BC250HSA_EBADMETADATA;
    }
    for (pair = 0; pair < root.as.count; pair++) {
        bc250hsa_mp_value key;

        if (!bc250hsa_mp_next(&r, &key) || key.kind != BC250HSA_MP_STR) {
            return BC250HSA_EBADMETADATA;
        }
        if (bc250hsa_mp_str_is(&key, "amdhsa.kernels")) {
            bc250hsa_mp_value list;
            uint32_t          i;

            if (have_kernels) {
                return BC250HSA_EBADMETADATA;
            }
            if (!bc250hsa_mp_next(&r, &list) || list.kind != BC250HSA_MP_ARRAY) {
                return BC250HSA_EBADMETADATA;
            }
            mod->kernel_count = list.as.count;
            if (list.as.count != 0u) {
                mod->kernels = (bc250hsa_kernel_internal*)calloc(
                    list.as.count, sizeof(bc250hsa_kernel_internal));
                if (mod->kernels == NULL) {
                    return BC250HSA_ENOMEM;
                }
            }
            for (i = 0; i < list.as.count; i++) {
                const bc250hsa_status status = parse_kernel(&r, &mod->kernels[i]);
                if (status != BC250HSA_OK) {
                    return status;
                }
            }
            have_kernels = 1;
            continue;
        }
        if (!bc250hsa_mp_skip(&r)) {
            return BC250HSA_EBADMETADATA;
        }
    }
    if (r.failed) {
        return BC250HSA_EBADMETADATA;
    }
    /* A code object with no kernel is legal and useless. The loader accepts it;
     * bc250hsa_module_kernel_by_name then finds nothing. */
    return BC250HSA_OK;
}

/* --------------------------------------------------------------------------
 * The kernel descriptor
 * ------------------------------------------------------------------------ */

bc250hsa_status bc250hsa_descriptor_read(const uint8_t* bytes, size_t byte_count,
                                        bc250hsa_kernel_internal* kernel)
{
    bc250hsa_kernel_descriptor d;
    uint32_t                   i;

    if (byte_count < 64u) {
        return BC250HSA_EBADELF;
    }
    memset(&d, 0, sizeof(d));
    memcpy(&d.group_segment_fixed_size,   bytes + 0,  4);
    memcpy(&d.private_segment_fixed_size, bytes + 4,  4);
    memcpy(&d.kernarg_size,               bytes + 8,  4);
    memcpy(&d.reserved0,                  bytes + 12, 4);
    memcpy(&d.kernel_code_entry_byte_offset, bytes + 16, 8);
    memcpy(d.reserved1,                   bytes + 24, 20);
    memcpy(&d.compute_pgm_rsrc3,          bytes + 44, 4);
    memcpy(&d.compute_pgm_rsrc1,          bytes + 48, 4);
    memcpy(&d.compute_pgm_rsrc2,          bytes + 52, 4);
    memcpy(&d.kernel_code_properties,     bytes + 56, 2);
    memcpy(&d.kernarg_preload,            bytes + 58, 2);
    memcpy(&d.reserved2,                  bytes + 60, 4);

    /* Every reserved field of the descriptor must be zero. A non-zero value means
     * a descriptor shape this build does not know, and running it would program
     * registers from bytes whose meaning is unknown. */
    if (d.reserved0 != 0u || d.reserved2 != 0u) {
        return BC250HSA_EBADELF;
    }
    for (i = 0; i < BC250HSA_ARRAY_COUNT(d.reserved1); i++) {
        if (d.reserved1[i] != 0u) {
            return BC250HSA_EBADELF;
        }
    }
    /* This build does not preload kernel arguments into user SGPRs. */
    if (d.kernarg_preload != 0u) {
        return BC250HSA_EUNSUPPORTED;
    }
    if (((uint32_t)d.kernel_code_properties & ~(uint32_t)BC250HSA_KCP_KNOWN_MASK) != 0u) {
        return BC250HSA_EUNSUPPORTED;
    }

    kernel->descriptor = d;
    kernel->pub.compute_pgm_rsrc1 = d.compute_pgm_rsrc1;
    kernel->pub.compute_pgm_rsrc2 = d.compute_pgm_rsrc2;
    kernel->pub.compute_pgm_rsrc3 = d.compute_pgm_rsrc3;
    kernel->pub.kernel_code_properties = d.kernel_code_properties;
    kernel->pub.user_sgpr_count =
        (uint8_t)((d.compute_pgm_rsrc2 >> BC250HSA_RSRC2_USER_SGPR_SHIFT) &
                  BC250HSA_RSRC2_USER_SGPR_MASK);
    /* The descriptor's own copies cross-check the metadata. They disagree only in a
     * broken object, so a disagreement is refused rather than resolved. */
    if (d.kernarg_size != kernel->pub.kernarg_bytes ||
        d.group_segment_fixed_size != kernel->pub.group_segment_bytes ||
        d.private_segment_fixed_size != kernel->pub.private_segment_bytes) {
        return BC250HSA_EBADELF;
    }
    if ((d.kernel_code_properties & BC250HSA_KCP_USES_DYNAMIC_STACK) != 0u) {
        kernel->pub.uses_dynamic_stack = 1u;
    }
    return BC250HSA_OK;
}

uint32_t bc250hsa_lds_size_field(uint32_t group_segment_bytes, uint32_t dynamic_group_bytes)
{
    const uint64_t total = (uint64_t)group_segment_bytes + (uint64_t)dynamic_group_bytes;
    const uint64_t granules =
        bc250hsa_align_up_u64(total, BC250HSA_LDS_GRANULE_BYTES) / BC250HSA_LDS_GRANULE_BYTES;
    if (granules > BC250HSA_RSRC2_LDS_SIZE_MASK) {
        return BC250HSA_RSRC2_LDS_SIZE_MASK;   /* the caller already refused this size */
    }
    return (uint32_t)granules;
}
