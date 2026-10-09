/* bc250hsa_mock.c - the device half of bc250hsa over the host heap.
 *
 * Section 5.4 of docs/design/m16-hip-route-b.md: layer 2 links this file in place of
 * bc250hsa/kmt_device.c, kmt_memory.c and submit.c. Everything else of the library is
 * the real code, so its HIP test exercises the real offload bundle reader, the real ELF
 * loader, the real kernel argument packer and the real PM4 builder with no adapter.
 *
 * What the mock does not do: it does not pretend to run a shader. A submission builds the
 * real packet stream, keeps it for bc250hsa_last_ib(), and retires the fence. A test that
 * wants a result in memory writes it itself.
 */
#include <stdlib.h>
#include <string.h>

#include "bc250hsa_mock.h"
#include "internal.h"
#include "pm4_regs.h"

#define MOCK_MAX_ALLOCATIONS 256u

typedef struct mock_allocation {
    void*    raw;
    uint64_t va;
    uint64_t bytes;
    uint32_t flags;
    int      in_use;
} mock_allocation;

struct bc250hsa_device {
    uint64_t        fence;
    uint64_t        last_submitted;
    uint64_t        submissions;
    uint32_t        last_ib[BC250HSA_PM4_MAX_DWORDS];
    uint32_t        last_ib_dwords;
    int             device_lost;
    mock_allocation allocations[MOCK_MAX_ALLOCATIONS];
    uint64_t        allocation_count;
};

static bc250hsa_mock_config g_config = { (uint32_t)sizeof(bc250hsa_mock_config), 1u, 0u, 0u, 0u };

bc250hsa_status bc250hsa_mock_configure(const bc250hsa_mock_config* config)
{
    if (config == NULL) {
        memset(&g_config, 0, sizeof(g_config));
        g_config.struct_bytes = (uint32_t)sizeof(g_config);
        g_config.complete_submissions = 1u;
        return BC250HSA_OK;
    }
    if (!bc250hsa_struct_bytes_ok(config->struct_bytes, sizeof(*config))) {
        return BC250HSA_EINVAL;
    }
    g_config = *config;
    return BC250HSA_OK;
}

/* The one device the mock hands out. A process that opens two gets two structures; the
 * counter below names the last one opened, which is what a single-device test needs. */
static struct bc250hsa_device* g_device;

uint64_t bc250hsa_mock_submission_count(void)
{
    return (g_device == NULL) ? 0u : g_device->submissions;
}

/* -------------------------------------------------------------------------------
 * Device
 * ----------------------------------------------------------------------------- */

bc250hsa_status bc250hsa_open(const bc250hsa_open_params* params, bc250hsa_device** out)
{
    if (out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (params != NULL && !bc250hsa_struct_bytes_ok(params->struct_bytes, sizeof(*params))) {
        return BC250HSA_EINVAL;
    }
    *out = (struct bc250hsa_device*)calloc(1, sizeof(struct bc250hsa_device));
    if (*out == NULL) {
        return BC250HSA_ENOMEM;
    }
    g_device = *out;
    return BC250HSA_OK;
}

void bc250hsa_close(bc250hsa_device* dev)
{
    uint32_t i;

    if (dev == NULL) {
        return;
    }
    for (i = 0; i < MOCK_MAX_ALLOCATIONS; i++) {
        if (dev->allocations[i].in_use) {
            free(dev->allocations[i].raw);
            dev->allocations[i].in_use = 0;
        }
    }
    if (g_device == dev) {
        g_device = NULL;
    }
    free(dev);
}

bc250hsa_status bc250hsa_props_read(bc250hsa_device* dev, bc250hsa_props* out)
{
    if (dev == NULL || out == NULL || !bc250hsa_struct_bytes_ok(out->struct_bytes, sizeof(*out))) {
        return BC250HSA_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->struct_bytes = (uint32_t)sizeof(*out);
    /* Mock values. Nothing may cite them as a measurement of the part. */
    memcpy(out->name, "mock BC-250", 12);
    out->gfx_ip_major = 10u;
    out->gfx_ip_minor = 1u;
    out->gfx_ip_rev = 3u;
    out->wave_size = 32u;
    out->cu_count = 40u;
    out->se_count = 2u;
    out->max_workgroup_size = 1024u;
    out->max_workgroups_per_dim = 0xFFFFFFFFu;
    out->lds_bytes_per_workgroup = 65536u;
    out->waves_per_cu = 16u;
    out->vram_bytes = 1024ull * 1024ull * 1024ull;
    out->visible_vram_bytes = out->vram_bytes;
    out->gtt_bytes = out->vram_bytes;
    return BC250HSA_OK;
}

/* -------------------------------------------------------------------------------
 * Memory
 * ----------------------------------------------------------------------------- */

static mock_allocation* free_slot(struct bc250hsa_device* dev)
{
    uint32_t i;
    for (i = 0; i < MOCK_MAX_ALLOCATIONS; i++) {
        if (!dev->allocations[i].in_use) {
            return &dev->allocations[i];
        }
    }
    return NULL;
}

bc250hsa_status bc250hsa_alloc(bc250hsa_device* dev, uint64_t bytes, uint64_t alignment,
                               uint32_t flags, bc250hsa_mem* out)
{
    mock_allocation* slot;
    uint8_t*         raw;
    uintptr_t        aligned;

    if (dev == NULL || out == NULL || bytes == 0u) {
        return BC250HSA_EINVAL;
    }
    if (alignment == 0u) {
        alignment = ((flags & BC250HSA_MEM_EXEC) != 0u) ? 4096u : 256u;
    }
    if (!bc250hsa_is_power_of_two_u64(alignment)) {
        return BC250HSA_EINVAL;
    }
    dev->allocation_count++;
    if (g_config.fail_alloc_after != 0u && dev->allocation_count >= g_config.fail_alloc_after) {
        return BC250HSA_ENOMEM;
    }
    slot = free_slot(dev);
    if (slot == NULL) {
        return BC250HSA_ENOMEM;
    }
    raw = (uint8_t*)calloc(1, (size_t)(bytes + alignment));
    if (raw == NULL) {
        return BC250HSA_ENOMEM;
    }
    aligned = ((uintptr_t)raw + (uintptr_t)alignment - 1u) & ~((uintptr_t)alignment - 1u);
    slot->raw = raw;
    slot->va = (uint64_t)aligned;
    slot->bytes = bytes;
    slot->flags = flags;
    slot->in_use = 1;

    memset(out, 0, sizeof(*out));
    out->va = slot->va;
    out->bytes = bytes;
    out->flags = flags;
    out->opaque = slot;
    if ((flags & BC250HSA_MEM_DEVICE) == 0u || (flags & BC250HSA_MEM_MAPPABLE) != 0u ||
        g_config.refuse_host_mapping == 0u) {
        out->host = (void*)aligned;
    }
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_free(bc250hsa_device* dev, bc250hsa_mem* mem)
{
    mock_allocation* slot;

    if (dev == NULL || mem == NULL) {
        return BC250HSA_EINVAL;
    }
    slot = (mock_allocation*)mem->opaque;
    if (slot == NULL || !slot->in_use) {
        return BC250HSA_EINVAL;
    }
    free(slot->raw);
    memset(slot, 0, sizeof(*slot));
    memset(mem, 0, sizeof(*mem));
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_map(bc250hsa_device* dev, bc250hsa_mem* mem, void** out)
{
    mock_allocation* slot;

    if (dev == NULL || mem == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    slot = (mock_allocation*)mem->opaque;
    if (slot == NULL || !slot->in_use) {
        return BC250HSA_EINVAL;
    }
    if (g_config.refuse_host_mapping != 0u && (slot->flags & BC250HSA_MEM_DEVICE) != 0u &&
        (slot->flags & BC250HSA_MEM_MAPPABLE) == 0u) {
        return BC250HSA_EUNSUPPORTED;
    }
    mem->host = (void*)(uintptr_t)slot->va;
    *out = mem->host;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_unmap(bc250hsa_device* dev, bc250hsa_mem* mem)
{
    if (dev == NULL || mem == NULL) {
        return BC250HSA_EINVAL;
    }
    mem->host = NULL;
    return BC250HSA_OK;
}

void bc250hsa_write_barrier(void)
{
}

bc250hsa_status bc250hsa_copy_to_device(bc250hsa_device* dev, const bc250hsa_mem* dst,
                                        uint64_t dst_offset, const void* src, uint64_t bytes)
{
    if (dev == NULL || dst == NULL || src == NULL) {
        return BC250HSA_EINVAL;
    }
    if (dst->host == NULL) {
        return BC250HSA_EUNSUPPORTED;
    }
    if (dst_offset > dst->bytes || bytes > dst->bytes - dst_offset) {
        return BC250HSA_EINVAL;
    }
    memcpy((uint8_t*)dst->host + dst_offset, src, (size_t)bytes);
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_copy_from_device(bc250hsa_device* dev, void* dst,
                                          const bc250hsa_mem* src, uint64_t src_offset,
                                          uint64_t bytes)
{
    if (dev == NULL || dst == NULL || src == NULL) {
        return BC250HSA_EINVAL;
    }
    if (src->host == NULL) {
        return BC250HSA_EUNSUPPORTED;
    }
    if (src_offset > src->bytes || bytes > src->bytes - src_offset) {
        return BC250HSA_EINVAL;
    }
    memcpy(dst, (const uint8_t*)src->host + src_offset, (size_t)bytes);
    return BC250HSA_OK;
}

static bc250hsa_status mock_allocator_alloc(void* ctx, uint64_t bytes, uint64_t alignment,
                                            uint32_t flags, bc250hsa_mem* out)
{
    return bc250hsa_alloc((bc250hsa_device*)ctx, bytes, alignment, flags, out);
}

static void mock_allocator_free(void* ctx, bc250hsa_mem* mem)
{
    (void)bc250hsa_free((bc250hsa_device*)ctx, mem);
}

bc250hsa_status bc250hsa_device_allocator(bc250hsa_device* dev, bc250hsa_allocator* out)
{
    if (dev == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    out->ctx = dev;
    out->alloc = mock_allocator_alloc;
    out->free = mock_allocator_free;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_module_load(bc250hsa_device* dev, const void* image, size_t image_bytes,
                                     bc250hsa_module** out)
{
    bc250hsa_allocator    alloc;
    const bc250hsa_status status = bc250hsa_device_allocator(dev, &alloc);

    if (status != BC250HSA_OK) {
        return status;
    }
    return bc250hsa_module_load_alloc(&alloc, image, image_bytes, out);
}

/* -------------------------------------------------------------------------------
 * Submission and completion
 * ----------------------------------------------------------------------------- */

uint64_t bc250hsa_fence_read(bc250hsa_device* dev)
{
    if (dev == NULL) {
        return 0u;
    }
    if (dev->device_lost) {
        return UINT64_MAX;
    }
    return dev->fence;
}

uint64_t bc250hsa_fence_last_submitted(bc250hsa_device* dev)
{
    return (dev == NULL) ? 0u : dev->last_submitted;
}

bc250hsa_status bc250hsa_dispatch_submit(bc250hsa_device* dev, const bc250hsa_dispatch* dispatch,
                                         uint64_t* fence_value_out)
{
    bc250hsa_pm4_env env;
    bc250hsa_status  status;
    uint32_t         written = 0;
    uint32_t         rsrc[4];

    if (dev == NULL || dispatch == NULL || fence_value_out == NULL) {
        return BC250HSA_EINVAL;
    }
    *fence_value_out = 0u;
    if (dev->device_lost) {
        return BC250HSA_EDEVICELOST;
    }
    status = bc250hsa_buffer_resource(0x0000004000000000ull, 4096u, rsrc);
    if (status != BC250HSA_OK) {
        return status;
    }
    memset(&env, 0, sizeof(env));
    env.struct_bytes = (uint32_t)sizeof(env);
    env.flags = dispatch->flags | BC250HSA_DISPATCH_GFX_RING;
    env.fence_va = 0x0000004000001000ull;
    env.fence_value = dev->last_submitted + 1u;
    memcpy(env.private_segment_rsrc, rsrc, sizeof(rsrc));
    env.ib_pad_dwords = BC250HSA_IB_PAD_DWORDS;

    /* The real builder, with the real refusals. A dispatch the hardware path would
     * refuse is refused here as well. */
    status = bc250hsa_pm4_build_dispatch(dispatch, &env, dev->last_ib,
                                         BC250HSA_PM4_MAX_DWORDS, &written);
    if (status != BC250HSA_OK) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return status;
    }
    dev->last_ib_dwords = written;
    dev->last_submitted = env.fence_value;
    dev->submissions++;
    bc250hsa_count_add(BC250HSA_C_SUBMISSIONS, 1u);
    if (g_config.lose_device_after != 0u && dev->submissions >= g_config.lose_device_after) {
        dev->device_lost = 1;
        bc250hsa_count_add(BC250HSA_C_DEVICE_LOSSES, 1u);
        return BC250HSA_EDEVICELOST;
    }
    if (g_config.complete_submissions != 0u) {
        dev->fence = env.fence_value;
    }
    *fence_value_out = env.fence_value;
    return BC250HSA_OK;
}

/* Section 8.1. This mock submits every dispatch at once, so a flush has nothing to do
 * and a policy that asks for batching is refused by name instead of being accepted and
 * ignored: a test that believed it batched here would measure nothing. The layer-2 mock
 * (hipmock_backend.c) does emulate a batch. */
bc250hsa_status bc250hsa_batch_policy_set(bc250hsa_device* dev,
                                          const bc250hsa_batch_policy* policy)
{
    if (dev == NULL || policy == NULL || policy->struct_bytes != (uint32_t)sizeof(*policy)) {
        return BC250HSA_EINVAL;
    }
    return (policy->enabled != 0u) ? BC250HSA_EUNSUPPORTED : BC250HSA_OK;
}

bc250hsa_status bc250hsa_batch_policy_get(bc250hsa_device* dev, bc250hsa_batch_policy* out)
{
    if (dev == NULL || out == NULL || out->struct_bytes != (uint32_t)sizeof(*out)) {
        return BC250HSA_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->struct_bytes = (uint32_t)sizeof(*out);
    out->max_dispatches = BC250HSA_BATCH_DISPATCHES_DEFAULT;
    out->max_hold_us = BC250HSA_BATCH_HOLD_US_DEFAULT;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_flush(bc250hsa_device* dev, uint64_t* fence_value_out)
{
    if (dev == NULL) {
        return BC250HSA_EINVAL;
    }
    if (fence_value_out != NULL) {
        *fence_value_out = dev->last_submitted;
    }
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_wait(bc250hsa_device* dev, uint64_t value, uint32_t slice_ms,
                             uint32_t total_ms)
{
    (void)slice_ms;
    (void)total_ms;
    if (dev == NULL) {
        return BC250HSA_EINVAL;
    }
    bc250hsa_count_add(BC250HSA_C_WAITS, 1u);
    if (dev->device_lost) {
        return BC250HSA_EDEVICELOST;
    }
    if (dev->fence >= value) {
        bc250hsa_count_add(BC250HSA_C_WAITS_FAST, 1u);
        return BC250HSA_OK;
    }
    /* The mock does not sleep: a configuration that never completes a submission is a
     * timeout at once, so a test of the caller's timeout path costs no seconds. */
    bc250hsa_count_add(BC250HSA_C_WAITS_TIMED_OUT, 1u);
    return BC250HSA_ETIMEOUT;
}

bc250hsa_status bc250hsa_query_fault(bc250hsa_device* dev, bc250hsa_fault* out)
{
    if (dev == NULL || out == NULL || !bc250hsa_struct_bytes_ok(out->struct_bytes, sizeof(*out))) {
        return BC250HSA_EINVAL;
    }
    memset((uint8_t*)out + sizeof(out->struct_bytes), 0, sizeof(*out) - sizeof(out->struct_bytes));
    out->device_lost = (uint32_t)(dev->device_lost ? 1 : 0);
    out->fence_value = bc250hsa_fence_read(dev);
    out->fence_expected = dev->last_submitted;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_last_ib(bc250hsa_device* dev, const uint32_t** dwords, uint32_t* count)
{
    if (dev == NULL || dwords == NULL || count == NULL) {
        return BC250HSA_EINVAL;
    }
    *dwords = dev->last_ib;
    *count = dev->last_ib_dwords;
    return BC250HSA_OK;
}
