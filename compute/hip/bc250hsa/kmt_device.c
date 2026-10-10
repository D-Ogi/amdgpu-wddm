/* kmt_device.c - the adapter, the device, the paging queue, the node-0 context, the
 * monitored fence, the command ring and the device properties.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, section 3.2. The call order is our own
 * Vulkan winsys's (the Mesa fork, branch amdgpu-wddm/b19-icd,
 * src/amd/vulkan/winsys/wddm2/radv_wddm2_winsys.c and radv_wddm2_cs.c), which ran on
 * unit A. The abstraction of RADV is deliberately not copied, only the call sequence.
 *
 * The adapter is identified by its own capability blob: D3DKMTEnumAdapters2 lists
 * every adapter, and the one whose KMTQAITYPE_UMDRIVERPRIVATE answer starts with the
 * magic "BC25" is ours. A caller that knows the LUID passes it and this step is
 * skipped.
 */
#include <stdlib.h>

#include "kmt_device.h"
#include "pm4_regs.h"

bc250hsa_status bc250hsa_os_status(const char* what, NTSTATUS status)
{
    if (NT_SUCCESS(status)) {
        return BC250HSA_OK;
    }
    bc250hsa_set_os_status((int32_t)status);
    bc250hsa_log(BC250HSA_LOG_ERROR, "%s failed 0x%08lx", what, (unsigned long)status);
    return BC250HSA_EOS;
}

void bc250hsa_mark_lost(struct bc250hsa_device* dev, const char* why)
{
    if (dev->device_lost) {
        return;
    }
    dev->device_lost = 1;
    bc250hsa_count_add(BC250HSA_C_DEVICE_LOSSES, 1u);
    bc250hsa_log(BC250HSA_LOG_ERROR, "device lost: %s", why);
}

int bc250hsa_device_executing(struct bc250hsa_device* dev)
{
    D3DKMT_GETDEVICESTATE state;
    NTSTATUS              status;

    memset(&state, 0, sizeof(state));
    state.hDevice = dev->device;
    state.StateType = D3DKMT_DEVICESTATE_EXECUTION;
    status = D3DKMTGetDeviceState(&state);
    /* An unanswered query is no evidence of a loss. */
    if (!NT_SUCCESS(status)) {
        return 1;
    }
    return state.ExecutionState == D3DKMT_DEVICEEXECUTION_ACTIVE;
}

static uint32_t caps_u32(const uint8_t* caps, uint32_t offset)
{
    uint32_t v;
    memcpy(&v, caps + offset, sizeof(v));
    return v;
}

static uint64_t caps_u64(const uint8_t* caps, uint32_t offset)
{
    uint64_t v;
    memcpy(&v, caps + offset, sizeof(v));
    return v;
}

static NTSTATUS query_caps(D3DKMT_HANDLE adapter, uint8_t* caps)
{
    D3DKMT_QUERYADAPTERINFO query;

    memset(&query, 0, sizeof(query));
    query.hAdapter = adapter;
    query.Type = KMTQAITYPE_UMDRIVERPRIVATE;
    query.pPrivateDriverData = caps;
    query.PrivateDriverDataSize = BC250HSA_CAPS_BYTES;
    return D3DKMTQueryAdapterInfo(&query);
}

static int caps_are_ours(const uint8_t* caps)
{
    return caps_u32(caps, BC250HSA_CAPS_OFF_MAGIC) == BC250HSA_CAPS_MAGIC &&
           caps_u32(caps, BC250HSA_CAPS_OFF_VERSION) >= 3u;
}

static void close_adapter(D3DKMT_HANDLE adapter)
{
    D3DKMT_CLOSEADAPTER close;
    if (adapter == 0u) {
        return;
    }
    memset(&close, 0, sizeof(close));
    close.hAdapter = adapter;
    (void)D3DKMTCloseAdapter(&close);
}

/* The BC-250 adapter, by its own capability blob. */
static bc250hsa_status find_adapter(const bc250hsa_open_params* params,
                                    struct bc250hsa_device* dev)
{
    D3DKMT_ENUMADAPTERS2 enumerate;
    D3DKMT_ADAPTERINFO*  list;
    NTSTATUS             status;
    ULONG                i;
    ULONG                count;

    if (params != NULL && (params->luid_low != 0u || params->luid_high != 0)) {
        D3DKMT_OPENADAPTERFROMLUID open;
        memset(&open, 0, sizeof(open));
        open.AdapterLuid.LowPart = params->luid_low;
        open.AdapterLuid.HighPart = params->luid_high;
        status = D3DKMTOpenAdapterFromLuid(&open);
        if (!NT_SUCCESS(status)) {
            return bc250hsa_os_status("OpenAdapterFromLuid", status);
        }
        dev->adapter = open.hAdapter;
        /* A named adapter is checked exactly as an enumerated one is. An adapter that
         * does not answer the BC-250 capability blob is another vendor's, and a
         * submission built for this part must not reach it. */
        if (!NT_SUCCESS(query_caps(dev->adapter, dev->caps)) || !caps_are_ours(dev->caps)) {
            bc250hsa_log(BC250HSA_LOG_ERROR,
                         "the adapter of the given LUID does not answer the BC-250"
                         " capability blob");
            close_adapter(dev->adapter);
            dev->adapter = 0u;
            return BC250HSA_ENODEV;
        }
        dev->caps_valid = 1;
        return BC250HSA_OK;
    }

    memset(&enumerate, 0, sizeof(enumerate));
    status = D3DKMTEnumAdapters2(&enumerate);
    if (!NT_SUCCESS(status) || enumerate.NumAdapters == 0u) {
        return BC250HSA_ENODEV;
    }
    count = enumerate.NumAdapters;
    list = (D3DKMT_ADAPTERINFO*)calloc(count, sizeof(*list));
    if (list == NULL) {
        return BC250HSA_ENOMEM;
    }
    enumerate.NumAdapters = count;
    enumerate.pAdapters = list;
    status = D3DKMTEnumAdapters2(&enumerate);
    if (!NT_SUCCESS(status)) {
        free(list);
        return bc250hsa_os_status("EnumAdapters2", status);
    }
    for (i = 0; i < enumerate.NumAdapters; i++) {
        if (dev->adapter == 0u && NT_SUCCESS(query_caps(list[i].hAdapter, dev->caps)) &&
            caps_are_ours(dev->caps)) {
            dev->adapter = list[i].hAdapter;
            dev->caps_valid = 1;
            continue;
        }
        close_adapter(list[i].hAdapter);
    }
    free(list);
    if (dev->adapter == 0u) {
        bc250hsa_log(BC250HSA_LOG_ERROR, "no adapter answered the BC-250 capability blob");
        return BC250HSA_ENODEV;
    }
    return BC250HSA_OK;
}

static void device_teardown(struct bc250hsa_device* dev)
{
    bc250hsa_free_all_allocations(dev);
    if (dev->fence != 0u) {
        D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.hSyncObject = dev->fence;
        (void)D3DKMTDestroySynchronizationObject(&destroy);
        dev->fence = 0u;
    }
    if (dev->context != 0u) {
        D3DKMT_DESTROYCONTEXT destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.hContext = dev->context;
        (void)D3DKMTDestroyContext(&destroy);
        dev->context = 0u;
    }
    if (dev->paging_queue != 0u) {
        /* The display driver interface names this one after the display driver DDI
         * structure, not after the kernel-mode thunk (d3dkmthk.h). */
        D3DDDI_DESTROYPAGINGQUEUE destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.hPagingQueue = dev->paging_queue;
        (void)D3DKMTDestroyPagingQueue(&destroy);
        dev->paging_queue = 0u;
        dev->paging_fence = 0u;
    }
    if (dev->device != 0u) {
        D3DKMT_DESTROYDEVICE destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.hDevice = dev->device;
        (void)D3DKMTDestroyDevice(&destroy);
        dev->device = 0u;
    }
    close_adapter(dev->adapter);
    dev->adapter = 0u;
    free(dev->slot_fence);
    dev->slot_fence = NULL;
    free(dev->last_ib);
    dev->last_ib = NULL;
}

bc250hsa_status bc250hsa_open(const bc250hsa_open_params* params, struct bc250hsa_device** out)
{
    struct bc250hsa_device*     dev;
    bc250hsa_status             result;
    NTSTATUS                    status;
    D3DKMT_CREATEDEVICE         create_device;
    D3DKMT_CREATEPAGINGQUEUE    create_paging;
    uint32_t                    ring_slots;
    uint32_t                    ring_slot_bytes;
    uint32_t                    va_window_gib;

    if (out == NULL) {
        return BC250HSA_EINVAL;
    }
    *out = NULL;
    if (params != NULL && !bc250hsa_struct_bytes_ok(params->struct_bytes, sizeof(*params))) {
        return BC250HSA_EINVAL;
    }
    ring_slots = (params != NULL && params->ring_slots != 0u) ? params->ring_slots
                                                             : BC250HSA_RING_SLOTS_DEFAULT;
    ring_slot_bytes = (params != NULL && params->ring_slot_bytes != 0u)
                          ? params->ring_slot_bytes
                          : BC250HSA_RING_SLOT_BYTES_DEFAULT;
    va_window_gib = (params != NULL && params->va_window_gib != 0u)
                        ? params->va_window_gib
                        : BC250HSA_VA_WINDOW_GIB_DEFAULT;
    if (ring_slots == 0u || ring_slots > 256u || ring_slot_bytes < 4096u ||
        ring_slot_bytes % 4096u != 0u || va_window_gib > 1024u) {
        return BC250HSA_EINVAL;
    }
    if (ring_slot_bytes < BC250HSA_PM4_MAX_DWORDS * 4u) {
        return BC250HSA_EINVAL;
    }

    dev = (struct bc250hsa_device*)calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return BC250HSA_ENOMEM;
    }
    InitializeCriticalSection(&dev->lock);
    dev->flags = (params != NULL) ? params->flags : 0u;
    dev->ring_slots = ring_slots;
    dev->ring_slot_bytes = ring_slot_bytes;
    dev->va_window_start = BC250HSA_VA_WINDOW_START;
    dev->va_window_end = BC250HSA_VA_WINDOW_START + ((uint64_t)va_window_gib << 30);
    /* Batching is off until the caller asks for it, which keeps the behaviour of
     * build 1 for a caller that does not know section 8.1 exists. The two numbers are
     * the defaults a caller gets when it passes 0. */
    dev->batch.struct_bytes = (uint32_t)sizeof(dev->batch);
    dev->batch.enabled = 0u;
    dev->batch.max_dispatches = BC250HSA_BATCH_DISPATCHES_DEFAULT;
    dev->batch.max_hold_us = BC250HSA_BATCH_HOLD_US_DEFAULT;
    /* The local memory limit of this part, which bc250hsa_props_read also reports. One
     * read at open in place of one property structure per dispatch. */
    dev->lds_bytes_per_workgroup = BC250HSA_LDS_BYTES_PER_WORKGROUP;

    result = find_adapter(params, dev);
    if (result != BC250HSA_OK) {
        goto failed;
    }

    /* The window this library asks MapGpuVirtualAddress for must lie inside the range
     * the kernel driver states for user mode: device.virtual_address_offset and
     * device.virtual_address_max of the capability blob (0x10000 and 2^47 on unit A).
     * A window the driver does not back is refused here, where the message names the
     * two numbers, and not later as an opaque MapGpuVirtualAddress failure. */
    if (dev->caps_valid) {
        const uint64_t driver_start = caps_u64(dev->caps, BC250HSA_CAPS_OFF_VA_OFFSET);
        const uint64_t driver_end = caps_u64(dev->caps, BC250HSA_CAPS_OFF_VA_MAX);
        if (driver_end <= driver_start || dev->va_window_start < driver_start ||
            dev->va_window_end > driver_end) {
            bc250hsa_log(BC250HSA_LOG_ERROR,
                         "the %u GiB address window at 0x%llx is outside the range the kernel"
                         " driver states for user mode (0x%llx to 0x%llx)",
                         (unsigned)va_window_gib, (unsigned long long)dev->va_window_start,
                         (unsigned long long)driver_start, (unsigned long long)driver_end);
            result = BC250HSA_EUNSUPPORTED;
            goto failed;
        }
    }

    memset(&create_device, 0, sizeof(create_device));
    create_device.hAdapter = dev->adapter;
    status = D3DKMTCreateDevice(&create_device);
    if (!NT_SUCCESS(status)) {
        result = bc250hsa_os_status("CreateDevice", status);
        goto failed;
    }
    dev->device = create_device.hDevice;

    memset(&create_paging, 0, sizeof(create_paging));
    create_paging.hDevice = dev->device;
    status = D3DKMTCreatePagingQueue(&create_paging);
    if (!NT_SUCCESS(status)) {
        result = bc250hsa_os_status("CreatePagingQueue", status);
        goto failed;
    }
    dev->paging_queue = create_paging.hPagingQueue;
    dev->paging_fence = create_paging.hSyncObject;

    if ((dev->flags & BC250HSA_OPEN_NO_GPU_SUBMIT) == 0u) {
        struct bc250hsa_context_blob     context_blob;
        D3DKMT_CREATECONTEXTVIRTUAL      create_context;
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create_fence;

        memset(&context_blob, 0, sizeof(context_blob));
        context_blob.magic = BC250HSA_CONTEXT_MAGIC;
        context_blob.version = BC250HSA_CONTEXT_VERSION;
        context_blob.size = (uint32_t)sizeof(context_blob);
        context_blob.ip_type = BC250HSA_IP_GFX;
        context_blob.node_ordinal = 0u;

        memset(&create_context, 0, sizeof(create_context));
        create_context.hDevice = dev->device;
        create_context.NodeOrdinal = 0;
        create_context.EngineAffinity = 1;
        /* The kernel driver has no hardware-queue display driver interfaces:
         * SubmitCommand is the packet path. */
        create_context.Flags.HwQueueSupported = 0;
        create_context.pPrivateDriverData = &context_blob;
        create_context.PrivateDriverDataSize = (UINT)sizeof(context_blob);
        create_context.ClientHint = D3DKMT_CLIENTHINT_UNKNOWN;
        status = D3DKMTCreateContextVirtual(&create_context);
        if (!NT_SUCCESS(status) || create_context.hContext == 0u) {
            result = bc250hsa_os_status("CreateContextVirtual", status);
            goto failed;
        }
        dev->context = create_context.hContext;

        memset(&create_fence, 0, sizeof(create_fence));
        create_fence.hDevice = dev->device;
        create_fence.Info.Type = D3DDDI_MONITORED_FENCE;
        create_fence.Info.MonitoredFence.InitialFenceValue = 0;
        status = D3DKMTCreateSynchronizationObject2(&create_fence);
        if (!NT_SUCCESS(status)) {
            result = bc250hsa_os_status("CreateSynchronizationObject2", status);
            goto failed;
        }
        dev->fence = create_fence.hSyncObject;
        dev->fence_cpu =
            (volatile uint64_t*)create_fence.Info.MonitoredFence.FenceValueCPUVirtualAddress;
        dev->fence_gpu_va = create_fence.Info.MonitoredFence.FenceValueGPUVirtualAddress;
        if (dev->fence_cpu == NULL || dev->fence_gpu_va == 0u ||
            (dev->fence_gpu_va & 7u) != 0u) {
            bc250hsa_log(BC250HSA_LOG_ERROR,
                         "the monitored fence has no usable pair of addresses");
            result = BC250HSA_EOS;
            goto failed;
        }
        dev->submit_capable = 1;

        /* The command ring, and the zeroed page that backs the private segment
         * buffer. Both are host visible and cached: the loader and the dispatch
         * builder write them with ordinary stores. */
        result = bc250hsa_alloc(dev, (uint64_t)ring_slots * ring_slot_bytes, 4096u,
                                BC250HSA_MEM_HOST, &dev->ring);
        if (result != BC250HSA_OK) {
            goto failed;
        }
        result = bc250hsa_alloc(dev, BC250HSA_ZERO_PAGE_BYTES, 256u,
                                BC250HSA_MEM_HOST | BC250HSA_MEM_ZERO, &dev->zero_page);
        if (result != BC250HSA_OK) {
            goto failed;
        }
        result = bc250hsa_buffer_resource(dev->zero_page.va, dev->zero_page.bytes,
                                          dev->private_segment_rsrc);
        if (result != BC250HSA_OK) {
            goto failed;
        }
        dev->slot_fence = (uint64_t*)calloc(ring_slots, sizeof(uint64_t));
        /* One whole slot, because a batched buffer carries many dispatches and the
         * failure report must hold all of it (section 8.1). */
        dev->last_ib_capacity = ring_slot_bytes / 4u;
        dev->last_ib = (uint32_t*)calloc(dev->last_ib_capacity, sizeof(uint32_t));
        if (dev->slot_fence == NULL || dev->last_ib == NULL) {
            result = BC250HSA_ENOMEM;
            goto failed;
        }
    }

    *out = dev;
    return BC250HSA_OK;

failed:
    device_teardown(dev);
    DeleteCriticalSection(&dev->lock);
    free(dev);
    return result;
}

void bc250hsa_close(struct bc250hsa_device* dev)
{
    if (dev == NULL) {
        return;
    }
    /* The open batch goes out first, so that a program which closed the device without
     * a synchronisation does not lose the work it asked for (section 8.1). */
    (void)bc250hsa_flush(dev, NULL);
    if (dev->submit_capable && !dev->device_lost && dev->fence_last_submitted != 0u) {
        (void)bc250hsa_wait(dev, dev->fence_last_submitted, 0u, 0u);
    }
    device_teardown(dev);
    DeleteCriticalSection(&dev->lock);
    free(dev);
}

bc250hsa_status bc250hsa_props_read(struct bc250hsa_device* dev, bc250hsa_props* out)
{
    if (dev == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!bc250hsa_struct_bytes_ok(out->struct_bytes, sizeof(*out))) {
        return BC250HSA_EINVAL;
    }
    if (!dev->caps_valid) {
        return BC250HSA_ENODEV;
    }
    memset(out->name, 0, sizeof(out->name));
    memcpy(out->name, "AMD BC-250", 10);
    out->struct_bytes = (uint32_t)sizeof(*out);
    out->gfx_ip_major = 10u;
    out->gfx_ip_minor = 1u;
    out->gfx_ip_rev = 3u;
    out->pci_bus = caps_u32(dev->caps, BC250HSA_CAPS_OFF_PCI_BUS);
    out->pci_device = caps_u32(dev->caps, BC250HSA_CAPS_OFF_PCI_DEV);
    out->pci_function = caps_u32(dev->caps, BC250HSA_CAPS_OFF_PCI_FUNC);
    out->wave_size = caps_u32(dev->caps, BC250HSA_CAPS_OFF_WAVE_FRONT_SIZE);
    out->cu_count = caps_u32(dev->caps, BC250HSA_CAPS_OFF_CU_ACTIVE);
    out->se_count = caps_u32(dev->caps, BC250HSA_CAPS_OFF_NUM_SE);
    out->max_workgroup_size = 1024u;
    out->max_workgroups_per_dim = 0xFFFFFFFFu;
    /* The local memory a workgroup may ask for on this part. It is a property of
     * gfx10.1 and not of the capability blob, which carries no local memory size. The
     * dispatch path reads dev->lds_bytes_per_workgroup, which is this same number. */
    out->lds_bytes_per_workgroup = dev->lds_bytes_per_workgroup;
    /* A property of gfx10.1 and not of the capability blob, which carries no wave
     * slot count: 32 wave32 slots per compute unit (16 per SIMD, two SIMDs). */
    out->waves_per_cu = 32u;
    out->vram_bytes = caps_u64(dev->caps, BC250HSA_CAPS_OFF_VRAM_TOTAL);
    out->visible_vram_bytes = caps_u64(dev->caps, BC250HSA_CAPS_OFF_VIS_VRAM_TOTAL);
    out->gtt_bytes = caps_u64(dev->caps, BC250HSA_CAPS_OFF_GTT_TOTAL);
    out->gfx_clock_khz = (uint32_t)caps_u64(dev->caps, BC250HSA_CAPS_OFF_MAX_ENGINE_CLOCK);
    out->mem_clock_khz = (uint32_t)caps_u64(dev->caps, BC250HSA_CAPS_OFF_MAX_MEMORY_CLOCK);
    out->mem_bus_width = caps_u32(dev->caps, BC250HSA_CAPS_OFF_VRAM_BIT_WIDTH);
    /* The capability blob carries no kernel driver version of its own. What it does
     * carry is the display-resource-manager compatibility triple the winsys reads,
     * plus the blob version, so that is what this field reports. The real kernel
     * driver version comes from its own client (tools/bc250kmd_cli.exe), which every
     * lab record pulls beside this one. */
    out->kmd_version[0] = caps_u32(dev->caps, BC250HSA_CAPS_OFF_DRM_MAJOR + 0u);
    out->kmd_version[1] = caps_u32(dev->caps, BC250HSA_CAPS_OFF_DRM_MAJOR + 4u);
    out->kmd_version[2] = caps_u32(dev->caps, BC250HSA_CAPS_OFF_DRM_MAJOR + 8u);
    out->kmd_version[3] = caps_u32(dev->caps, BC250HSA_CAPS_OFF_VERSION);
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_query_fault(struct bc250hsa_device* dev, bc250hsa_fault* out)
{
    D3DKMT_GETDEVICESTATE state;
    NTSTATUS              status;

    if (dev == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!bc250hsa_struct_bytes_ok(out->struct_bytes, sizeof(*out))) {
        return BC250HSA_EINVAL;
    }
    out->device_lost = dev->device_lost ? 1u : 0u;
    out->faulted_va = 0u;
    out->general_error = 0u;
    out->device_error = 0u;
    out->fault_flags = 0u;
    out->pipeline_stage = 0u;
    out->fence_value = bc250hsa_fence_read(dev);
    out->fence_expected = dev->fence_last_submitted;

    memset(&state, 0, sizeof(state));
    state.hDevice = dev->device;
    state.StateType = D3DKMT_DEVICESTATE_EXECUTION;
    status = D3DKMTGetDeviceState(&state);
    if (!NT_SUCCESS(status)) {
        return bc250hsa_os_status("GetDeviceState(execution)", status);
    }
    if (state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE) {
        out->device_lost = 1u;
    }
    if (state.ExecutionState == D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT) {
        state.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
        status = D3DKMTGetDeviceState(&state);
        if (NT_SUCCESS(status)) {
            out->faulted_va = state.PageFaultState.FaultedVirtualAddress;
            out->general_error = (uint32_t)state.PageFaultState.FaultErrorCode.GeneralErrorCode;
            out->device_error =
                (uint32_t)state.PageFaultState.FaultErrorCode.DeviceSpecificCode;
            out->fault_flags = (uint32_t)state.PageFaultState.PageFaultFlags;
            out->pipeline_stage = (uint32_t)state.PageFaultState.FaultedPipelineStage;
        }
    }
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_last_ib(struct bc250hsa_device* dev, const uint32_t** dwords,
                                 uint32_t* count)
{
    if (dev == NULL || dwords == NULL || count == NULL) {
        return BC250HSA_EINVAL;
    }
    if (dev->last_ib == NULL || dev->last_ib_dwords == 0u) {
        return BC250HSA_ENOTFOUND;
    }
    *dwords = dev->last_ib;
    *count = dev->last_ib_dwords;
    return BC250HSA_OK;
}
