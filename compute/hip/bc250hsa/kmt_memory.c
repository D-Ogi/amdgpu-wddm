/* kmt_memory.c - allocation, GPU address, residency and the host mapping.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, section 3.3. The call order is our own
 * Vulkan winsys's (the Mesa fork, branch amdgpu-wddm/b19-icd,
 * src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c), which ran on unit A:
 *
 *   D3DKMTCreateAllocation2 -> D3DKMTMapGpuVirtualAddress -> D3DKMTMakeResident
 *   -> D3DKMTWaitForSynchronizationObjectFromCpu on the paging fence
 *
 * The paging fence wait after the map and the residency is not optional: both calls
 * are asynchronous, and an under-wait lets the GPU touch an unmapped page, which this
 * part cannot recover from (fact M53, no working GPU reset).
 */
#include <stdlib.h>

#include "kmt_device.h"

/* Every operation on a device paging queue gets a unique value of the queue's
 * monitored fence, and zero when it completed at once. A monitored fence value only
 * grows, so waiting for the largest value returned waits for all of them; a later
 * zero must never replace an earlier value. */
static void require_paging_fence(uint64_t* required, uint64_t value)
{
    if (value > *required) {
        *required = value;
    }
}

bc250hsa_status bc250hsa_wait_paging_fence(struct bc250hsa_device* dev, uint64_t value)
{
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;
    NTSTATUS                                   status;

    if (value == 0u) {
        return BC250HSA_OK;
    }
    memset(&wait, 0, sizeof(wait));
    wait.hDevice = dev->device;
    wait.ObjectCount = 1;
    wait.ObjectHandleArray = &dev->paging_fence;
    wait.FenceValueArray = &value;
    status = D3DKMTWaitForSynchronizationObjectFromCpu(&wait);
    return bc250hsa_os_status("WaitForSynchronizationObjectFromCpu(paging)", status);
}

/* The natural alignment of an allocation, as the winsys computes it: 4096 at least,
 * raised to 64 KiB at 64 KiB and to 256 KiB at 256 KiB for device local memory. */
static uint64_t natural_alignment(uint64_t bytes, uint32_t flags)
{
    uint64_t alignment = 4096u;
    if ((flags & BC250HSA_MEM_EXEC) != 0u) {
        /* The base must be 4096-aligned, and the entry address must be 256-byte
         * aligned because COMPUTE_PGM_LO holds the address shifted right by 8. The
         * linker already aligns .text to 256 inside the object. */
        return 4096u;
    }
    if ((flags & (BC250HSA_MEM_HOST | BC250HSA_MEM_HOST_WC)) == 0u) {
        if (bytes >= 0x10000u) { alignment = 0x10000u; }
        if (bytes >= 0x40000u) { alignment = 0x40000u; }
    }
    return alignment;
}

static int wants_host_mapping(uint32_t flags)
{
    return (flags & (BC250HSA_MEM_HOST | BC250HSA_MEM_HOST_WC | BC250HSA_MEM_EXEC)) != 0u;
}

static int is_device_local(uint32_t flags)
{
    return (flags & (BC250HSA_MEM_HOST | BC250HSA_MEM_HOST_WC | BC250HSA_MEM_EXEC)) == 0u;
}

static void record_insert(struct bc250hsa_device* dev, bc250hsa_alloc_record* r)
{
    r->prev = NULL;
    r->next = dev->allocations;
    if (dev->allocations != NULL) {
        dev->allocations->prev = r;
    }
    dev->allocations = r;
}

static void record_remove(struct bc250hsa_device* dev, bc250hsa_alloc_record* r)
{
    if (r->prev != NULL) { r->prev->next = r->next; } else { dev->allocations = r->next; }
    if (r->next != NULL) { r->next->prev = r->prev; }
    r->next = NULL;
    r->prev = NULL;
}

static void destroy_allocation(struct bc250hsa_device* dev, bc250hsa_alloc_record* r)
{
    if (r->host != NULL) {
        D3DKMT_UNLOCK2 unlock;
        memset(&unlock, 0, sizeof(unlock));
        unlock.hDevice = dev->device;
        unlock.hAllocation = r->handle;
        (void)D3DKMTUnlock2(&unlock);
        r->host = NULL;
    }
    if (r->va != 0u) {
        D3DKMT_FREEGPUVIRTUALADDRESS free_va;
        memset(&free_va, 0, sizeof(free_va));
        free_va.hAdapter = dev->adapter;
        free_va.BaseAddress = r->va;
        free_va.Size = r->bytes;
        (void)D3DKMTFreeGpuVirtualAddress(&free_va);
        r->va = 0u;
    }
    if (r->handle != 0u) {
        D3DKMT_DESTROYALLOCATION2 destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.hDevice = dev->device;
        destroy.phAllocationList = &r->handle;
        destroy.AllocationCount = 1;
        (void)D3DKMTDestroyAllocation2(&destroy);
        r->handle = 0u;
    }
}

void bc250hsa_free_all_allocations(struct bc250hsa_device* dev)
{
    /* The staging ring of the GPU copy and the held buffers of a coherence control are on
     * this list like every other allocation, so the loop below releases them. The handles
     * are cleared first, so that nothing reads a freed record through dev->stage or
     * dev->control_hold. The caller proved retirement first (bc250hsa_close), or this is
     * an open that failed before anything was submitted. */
    memset(dev->stage, 0, sizeof(dev->stage));
    memset(dev->stage_fence, 0, sizeof(dev->stage_fence));
    dev->stage_ready = 0u;
    memset(dev->control_hold, 0, sizeof(dev->control_hold));
    dev->control_hold_fence = 0u;
    while (dev->allocations != NULL) {
        bc250hsa_alloc_record* r = dev->allocations;
        record_remove(dev, r);
        destroy_allocation(dev, r);
        free(r);
    }
}

static bc250hsa_status map_host(struct bc250hsa_device* dev, bc250hsa_alloc_record* r,
                                void** out)
{
    D3DKMT_LOCK2 lock;
    NTSTATUS     status;

    if (r->host != NULL) {
        *out = r->host;
        return BC250HSA_OK;
    }
    memset(&lock, 0, sizeof(lock));
    lock.hDevice = dev->device;
    lock.hAllocation = r->handle;
    status = D3DKMTLock2(&lock);
    if (!NT_SUCCESS(status)) {
        bc250hsa_set_os_status((int32_t)status);
        bc250hsa_log(BC250HSA_LOG_WARN, "Lock2 of allocation 0x%x failed 0x%08lx", r->handle,
                     (unsigned long)status);
        /* OPEN for the lab: whether D3DKMTLock2 works on a device-local allocation
         * on this part. Until a trial answers it, a device-local allocation reports
         * no host pointer. */
        return BC250HSA_EUNSUPPORTED;
    }
    r->host = lock.pData;
    *out = r->host;
    return BC250HSA_OK;
}

/* This process's budget and use of one segment group, for the log of a placement. The
 * carve-out is one part of the GPU memory: the rest is system memory that the GPU reaches
 * through the non-local group (fact M827: on an 8 GiB carve-out the LOCAL budget is
 * 7.2 GiB and the NON_LOCAL budget 3.5 GiB). */
static int query_budget(struct bc250hsa_device* dev, D3DKMT_MEMORY_SEGMENT_GROUP group,
                        D3DKMT_QUERYVIDEOMEMORYINFO* query)
{
    memset(query, 0, sizeof(*query));
    query->hProcess = NULL;
    query->hAdapter = dev->adapter;
    query->MemorySegmentGroup = group;
    query->PhysicalAdapterIndex = 0;
    return NT_SUCCESS(D3DKMTQueryVideoMemoryInfo(query));
}

static void log_budget(struct bc250hsa_device* dev, D3DKMT_MEMORY_SEGMENT_GROUP group,
                       const char* name)
{
    D3DKMT_QUERYVIDEOMEMORYINFO query;

    if (!query_budget(dev, group, &query)) {
        return;
    }
    bc250hsa_log(BC250HSA_LOG_INFO,
                 "%s segment group: budget %llu, used %llu, reserved %llu, available for "
                 "reservation %llu bytes",
                 name, (unsigned long long)query.Budget, (unsigned long long)query.CurrentUsage,
                 (unsigned long long)query.CurrentReservation,
                 (unsigned long long)query.AvailableForReservation);
}

bc250hsa_status bc250hsa_memory_budget_read(struct bc250hsa_device* dev,
                                            bc250hsa_memory_budget* out)
{
    D3DKMT_QUERYVIDEOMEMORYINFO query;

    if (dev == NULL || out == NULL || out->struct_bytes != (uint32_t)sizeof(*out)) {
        return BC250HSA_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->struct_bytes = (uint32_t)sizeof(*out);
    if (query_budget(dev, D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL, &query)) {
        out->valid |= BC250HSA_BUDGET_LOCAL;
        out->local_budget = query.Budget;
        out->local_used = query.CurrentUsage;
    }
    if (query_budget(dev, D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL, &query)) {
        out->valid |= BC250HSA_BUDGET_NON_LOCAL;
        out->non_local_budget = query.Budget;
        out->non_local_used = query.CurrentUsage;
    }
    return out->valid != 0u ? BC250HSA_OK : BC250HSA_EUNSUPPORTED;
}

/* Whether `bytes` more of device-local memory stay inside this process's local budget. The
 * check comes before the residency: a MakeResident past the budget can return success and
 * fail later on the paging queue, and VidMm then puts the whole device in error with
 * execution state 5 (D3DKMT_DEVICEEXECUTION_ERROR_OUTOFMEMORY), as unit A did for Qwen3-8B
 * Q8_0 on an 8 GiB carve-out. A query that fails answers yes, and the residency decides. */
static int local_budget_allows(struct bc250hsa_device* dev, uint64_t bytes)
{
    D3DKMT_QUERYVIDEOMEMORYINFO query;

    if (!query_budget(dev, D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL, &query)) {
        return 1;
    }
    return query.CurrentUsage <= query.Budget && bytes <= query.Budget - query.CurrentUsage;
}

/* CreateAllocation2, MapGpuVirtualAddress and MakeResident of one allocation on one heap,
 * and the paging fence wait. On a failure the record holds what was made, and the caller
 * destroys it.
 *
 * MakeResident does not set MustSucceed. With MustSucceed, a request that VidMm cannot
 * satisfy puts the device in error (d3dukmdt.h, D3DDDI_MAKERESIDENT_FLAGS), and the flag
 * is only valid together with CantTrimFurther. One hipMalloc past the carve-out then took
 * the device down before the first launch: Qwen3-8B Q8_0 (8071 MiB) on unit A, fence
 * value UINT64_MAX at the first kernel argument lease. Without the flag a refusal is a
 * status that the caller can answer with the other heap. */
static bc250hsa_status create_resident(struct bc250hsa_device* dev, bc250hsa_alloc_record* record,
                                       uint64_t phys_bytes, uint64_t phys_alignment,
                                       uint32_t flags, uint32_t heap, uint32_t gem_flags)
{
    struct bc250hsa_alloc_blob         blob;
    struct bc250hsa_create_alloc_pdata pdata;
    D3DDDI_ALLOCATIONINFO2             info;
    D3DKMT_CREATEALLOCATION            create;
    D3DDDI_MAPGPUVIRTUALADDRESS        map;
    D3DDDI_MAKERESIDENT                resident;
    NTSTATUS                           status;
    uint64_t                           paging_fence = 0;

    memset(&blob, 0, sizeof(blob));
    blob.magic = BC250HSA_ALLOC_MAGIC;
    blob.version = BC250HSA_ALLOC_VERSION;
    blob.size = (uint32_t)sizeof(blob);
    blob.alloc_size = phys_bytes;
    blob.phys_alignment = phys_alignment;
    blob.preferred_heap = heap;
    blob.va_size = phys_bytes;
    blob.gem_flags = gem_flags;

    memset(&pdata, 0, sizeof(pdata));
    pdata.adapter_id = 0;
    pdata.flags = 0x80u;
    pdata.pdata_size = (uint32_t)sizeof(blob);
    pdata.checksum = bc250hsa_pdata_checksum((const uint32_t*)&pdata,
                                             (uint32_t)(sizeof(pdata) / 4u));

    memset(&info, 0, sizeof(info));
    info.pPrivateDriverData = &blob;
    info.PrivateDriverDataSize = (UINT)sizeof(blob);
    info.VidPnSourceId = 0xFFFFFFFFu;
    info.Priority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;

    memset(&create, 0, sizeof(create));
    create.hDevice = dev->device;
    create.pPrivateDriverData = &pdata;
    create.PrivateDriverDataSize = (UINT)sizeof(pdata);
    create.NumAllocations = 1;
    create.pAllocationInfo2 = &info;
    create.Flags.CreateResource = 1;
    create.Flags.NonSecure = 1;

    status = D3DKMTCreateAllocation2(&create);
    if (!NT_SUCCESS(status)) {
        return bc250hsa_os_status("CreateAllocation2", status);
    }
    record->handle = info.hAllocation;
    record->bytes = phys_bytes;
    record->flags = flags;

    memset(&map, 0, sizeof(map));
    map.hPagingQueue = dev->paging_queue;
    map.BaseAddress = 0;
    map.MinimumAddress = dev->va_window_start;
    map.MaximumAddress = dev->va_window_end;
    map.hAllocation = record->handle;
    map.SizeInPages = phys_bytes / 4096u;
    map.Protection.Write = 1;
    /* The shader image is fetched as instructions. Our page table writer does honour
     * DXGK_PTE.NoExecute (driver/shim/bc250_pte.c clears AMDGPU_PTE_EXECUTABLE for
     * it), and RADV maps its shader buffers with Write alone and executes them on
     * this driver, so dxgkrnl does not derive NoExecute from this field today. The
     * bit states the intent at no cost, for the day it does. */
    if ((flags & BC250HSA_MEM_EXEC) != 0u) {
        map.Protection.Execute = 1;
    }
    status = D3DKMTMapGpuVirtualAddress(&map);
    if (!NT_SUCCESS(status)) {
        return bc250hsa_os_status("MapGpuVirtualAddress", status);
    }
    record->va = map.VirtualAddress;
    require_paging_fence(&paging_fence, map.PagingFenceValue);

    memset(&resident, 0, sizeof(resident));
    resident.hPagingQueue = dev->paging_queue;
    resident.NumAllocations = 1;
    resident.AllocationList = &record->handle;
    status = D3DKMTMakeResident(&resident);
    if (!NT_SUCCESS(status)) {
        return bc250hsa_os_status("MakeResident", status);
    }
    require_paging_fence(&paging_fence, resident.PagingFenceValue);

    return bc250hsa_wait_paging_fence(dev, paging_fence);
}

bc250hsa_status bc250hsa_alloc(struct bc250hsa_device* dev, uint64_t bytes, uint64_t alignment,
                               uint32_t flags, bc250hsa_mem* out)
{
    bc250hsa_alloc_record*             record;
    bc250hsa_status                    result;
    uint64_t                           phys_alignment;
    uint64_t                           phys_bytes;
    uint32_t                           gem_flags = 0u;

    if (dev == NULL || out == NULL || bytes == 0u) {
        return BC250HSA_EINVAL;
    }
    if (alignment != 0u && !bc250hsa_is_power_of_two_u64(alignment)) {
        return BC250HSA_EINVAL;
    }
    if ((flags & (BC250HSA_MEM_HOST | BC250HSA_MEM_HOST_WC)) ==
        (BC250HSA_MEM_HOST | BC250HSA_MEM_HOST_WC)) {
        return BC250HSA_EINVAL;
    }
    /* Code is never write combined: the loader writes it with ordinary stores and
     * the instruction fetch must see them. */
    if ((flags & BC250HSA_MEM_EXEC) != 0u && (flags & BC250HSA_MEM_HOST_WC) != 0u) {
        return BC250HSA_EINVAL;
    }
    if (dev->device_lost) {
        return BC250HSA_EDEVICELOST;
    }

    phys_alignment = natural_alignment(bytes, flags);
    if (alignment > phys_alignment) {
        phys_alignment = alignment;
    }
    phys_bytes = bc250hsa_align_up_u64(bytes, phys_alignment);
    if (phys_bytes % 4096u != 0u) {
        phys_bytes = bc250hsa_align_up_u64(phys_bytes, 4096u);
    }

    record = (bc250hsa_alloc_record*)calloc(1, sizeof(*record));
    if (record == NULL) {
        return BC250HSA_ENOMEM;
    }

    if (wants_host_mapping(flags) || (flags & BC250HSA_MEM_MAPPABLE) != 0u) {
        gem_flags |= BC250HSA_GEM_CPU_ACCESS_REQUIRED;
    } else {
        gem_flags |= BC250HSA_GEM_NO_CPU_ACCESS;
    }
    if ((flags & BC250HSA_MEM_HOST_WC) != 0u) {
        gem_flags |= BC250HSA_GEM_CPU_GTT_USWC;
    }

    if (!is_device_local(flags)) {
        result = create_resident(dev, record, phys_bytes, phys_alignment, flags,
                                 BC250HSA_HEAP_GTT, gem_flags);
        if (result != BC250HSA_OK) {
            /* System memory has no second heap, so the caller gets the failure. The two
             * budgets tell whether the non-local group was full (BD-277: a code object of
             * an 8B model run did not load at a 12 GiB carve-out). */
            bc250hsa_log(BC250HSA_LOG_INFO,
                         "system memory allocation of %llu bytes (flags 0x%x) failed (%s)",
                         (unsigned long long)phys_bytes, (unsigned)flags,
                         bc250hsa_status_string(result));
            log_budget(dev, D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL, "local");
            log_budget(dev, D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL, "non-local");
        }
    } else {
        /* Device-local memory goes to the carve-out while the local budget holds it, and
         * to system memory after that, as VidMm does for D3D12 resources past the local
         * budget. A residency that VidMm refuses at once inside the budget also takes the
         * second heap. The system memory copy is write combined: device-local memory has
         * no cached host view either, so the contract of the allocation stays the same. */
        result = BC250HSA_ENOMEM;
        if (local_budget_allows(dev, phys_bytes)) {
            result = create_resident(dev, record, phys_bytes, phys_alignment, flags,
                                     BC250HSA_HEAP_VRAM, gem_flags);
        }
        if (result != BC250HSA_OK) {
            destroy_allocation(dev, record);
            memset(record, 0, sizeof(*record));
            bc250hsa_log(BC250HSA_LOG_INFO,
                         "device-local allocation of %llu bytes is placed in system memory: the "
                         "carve-out does not hold it (%s)",
                         (unsigned long long)phys_bytes, bc250hsa_status_string(result));
            log_budget(dev, D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL, "local");
            log_budget(dev, D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL, "non-local");
            result = create_resident(dev, record, phys_bytes, phys_alignment, flags,
                                     BC250HSA_HEAP_GTT, gem_flags | BC250HSA_GEM_CPU_GTT_USWC);
        }
    }
    if (result != BC250HSA_OK) {
        goto failed;
    }

    memset(out, 0, sizeof(*out));
    out->va = record->va;
    out->bytes = phys_bytes;
    out->flags = flags;
    out->opaque = record;

    if (wants_host_mapping(flags)) {
        void* host = NULL;
        result = map_host(dev, record, &host);
        if (result != BC250HSA_OK) {
            goto failed;
        }
        out->host = host;
    }
    if ((flags & BC250HSA_MEM_ZERO) != 0u) {
        if (out->host == NULL) {
            result = BC250HSA_EUNSUPPORTED;
            goto failed;
        }
        memset(out->host, 0, (size_t)phys_bytes);
        bc250hsa_write_barrier();
    }

    bc250hsa_device_lock(dev);
    record_insert(dev, record);
    LeaveCriticalSection(&dev->lock);
    return BC250HSA_OK;

failed:
    destroy_allocation(dev, record);
    free(record);
    memset(out, 0, sizeof(*out));
    return result;
}

bc250hsa_status bc250hsa_free(struct bc250hsa_device* dev, bc250hsa_mem* mem)
{
    bc250hsa_alloc_record* record;

    if (dev == NULL || mem == NULL) {
        return BC250HSA_EINVAL;
    }
    record = (bc250hsa_alloc_record*)mem->opaque;
    if (record == NULL) {
        return BC250HSA_EINVAL;
    }
    bc250hsa_device_lock(dev);
    /* A dispatch of an open batch may name this range. Section 8.1 of the interface
     * makes every call that changes such memory a flush point. */
    (void)bc250hsa_batch_submit_locked(dev);
    record_remove(dev, record);
    LeaveCriticalSection(&dev->lock);
    destroy_allocation(dev, record);
    free(record);
    memset(mem, 0, sizeof(*mem));
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_map(struct bc250hsa_device* dev, bc250hsa_mem* mem, void** out)
{
    bc250hsa_alloc_record* record;
    bc250hsa_status        status;
    void*                  host = NULL;

    if (dev == NULL || mem == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    record = (bc250hsa_alloc_record*)mem->opaque;
    if (record == NULL) {
        return BC250HSA_EINVAL;
    }
    if (is_device_local(mem->flags) && (mem->flags & BC250HSA_MEM_MAPPABLE) == 0u) {
        return BC250HSA_EUNSUPPORTED;
    }
    /* A flush point of section 8.1: a caller that maps a range reads or writes it next,
     * and a dispatch of the open batch may name it. */
    (void)bc250hsa_flush(dev, NULL);
    status = map_host(dev, record, &host);
    if (status != BC250HSA_OK) {
        return status;
    }
    mem->host = host;
    *out = host;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_unmap(struct bc250hsa_device* dev, bc250hsa_mem* mem)
{
    bc250hsa_alloc_record* record;
    D3DKMT_UNLOCK2         unlock;
    NTSTATUS               status;

    if (dev == NULL || mem == NULL) {
        return BC250HSA_EINVAL;
    }
    record = (bc250hsa_alloc_record*)mem->opaque;
    if (record == NULL) {
        return BC250HSA_EINVAL;
    }
    if (record->host == NULL) {
        return BC250HSA_OK;
    }
    /* The same flush point: the mapping a dispatch of the open batch may still need
     * goes away here. */
    (void)bc250hsa_flush(dev, NULL);
    memset(&unlock, 0, sizeof(unlock));
    unlock.hDevice = dev->device;
    unlock.hAllocation = record->handle;
    status = D3DKMTUnlock2(&unlock);
    if (!NT_SUCCESS(status)) {
        /* The lock and the mapping are then still valid, and the record keeps them,
         * so a later map returns the same pointer and the destroy retries (BD-045). */
        return bc250hsa_os_status("Unlock2", status);
    }
    record->host = NULL;
    mem->host = NULL;
    return BC250HSA_OK;
}

void bc250hsa_write_barrier(void)
{
    MemoryBarrier();
}

bc250hsa_status bc250hsa_copy_to_device(struct bc250hsa_device* dev, const bc250hsa_mem* dst,
                                        uint64_t dst_offset, const void* src, uint64_t bytes)
{
    if (dev == NULL || dst == NULL || src == NULL) {
        return BC250HSA_EINVAL;
    }
    if (dst->host == NULL) {
        /* A copy engine and a copy kernel are later work, named in the design:
         * node 1 is the copy engine and is not a user-mode queue. */
        return BC250HSA_EUNSUPPORTED;
    }
    if (dst_offset > dst->bytes || bytes > dst->bytes - dst_offset) {
        return BC250HSA_EINVAL;
    }
    /* A flush point: a dispatch of an open batch may read this range, and in the order
     * of the caller it reads what was there before this copy (section 8.1). The caller
     * still owns the wait; this call only makes sure the work it asked for first is on
     * its way to the device. */
    (void)bc250hsa_flush(dev, NULL);
    memcpy((uint8_t*)dst->host + dst_offset, src, (size_t)bytes);
    bc250hsa_write_barrier();
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_copy_from_device(struct bc250hsa_device* dev, void* dst,
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
    /* The same flush point in the other direction: this copy reads what the work the
     * caller already asked for wrote. The wait stays the caller's. */
    (void)bc250hsa_flush(dev, NULL);
    memcpy(dst, (const uint8_t*)src->host + src_offset, (size_t)bytes);
    return BC250HSA_OK;
}

/* --------------------------------------------------------------------------
 * The GPU copy and fill (scratch/hip-gpu-copy/CONTRACT.md)
 *
 * A device-to-host copy through the host mapping is an uncached read stream:
 * MEASURED 0.21 GB/s out of an allocation that takes writes at 15.6 GB/s
 * (evidence/m16/perf-rerun-2026-10-09). The three calls below move the bytes with
 * the command processor instead, as PM4 DMA_DATA packets in an indirect buffer of their
 * own on the device's copy context (submit.c, bc250hsa_copy_run_ops). Such a buffer
 * carries no dispatch record, and the kernel driver refuses a buffer without one on a
 * context whose journal is on, so it never goes to the main context (CONTRACT.md
 * deviation 13.23).
 *
 * Ownership: a range that a submission names stays in use until the fence value of that
 * submission retires, and a wait that failed does not say that it retired (CONTRACT.md
 * section 7, BD-110). Every call below therefore reports a receipt (bc250hsa.h section
 * 5.1), a staging slot keeps the fence value of the round that last wrote it, and the
 * buffers of a coherence control whose wait failed are held by the device and not freed.
 * A freed GPU address may be handed out again at once, which is why "free it anyway" is
 * not an option for memory a submission may still write.
 *
 * Locking: a staged copy, a fill with partial bytes and the coherence control hold
 * dev->lock for the whole operation, waits included. A Windows critical section is
 * recursive for the thread that owns it, so the nested bc250hsa_wait and
 * bc250hsa_copy_run_ops take and release it again without deadlocking; this is the same
 * pattern take_slot already uses (submit.c:187-216).
 *
 * What that costs another thread: a staged copy of n bytes is ceil(n / 1 MiB) rounds and
 * each round submits and waits, so a 64 MiB copy holds the lock across 64 bounded waits,
 * and another thread's bc250hsa_flush, dispatch, or wait for a value that is not yet
 * submitted queues behind all of them. That is stated in bc250hsa.h section 5.1 and in
 * CONTRACT.md deviation 13.12, and it is accepted here because the hot path of CONTRACT.md
 * section 5a does not stage at all: it is one packet into the caller's own allocation, with
 * no slot and no loop. Its one wait is inside the lock as well: the main context and the
 * copy context write the one monitored fence of this device, so a copy runs only when
 * everything submitted before it has retired, and nothing is submitted after it until it
 * retired (CONTRACT.md deviation 13.23). Layer 2 waits for the stream before every copy,
 * so the first of those two waits finds its value there. A copy whose own wait fails opens
 * the lock with the copy still out, so its value stays in dev->copy_unretired and the next
 * submission of any kind waits for it first (submit.c, copy_exclusion_locked).
 *
 * Each of these holds goes through bc250hsa_copy_lock_enter and _leave (submit.c), and every
 * other acquisition of dev->lock through bc250hsa_device_lock, so the counters
 * gpu_copy_lock_* (bc250hsa.h section 3) and the "fence check: copy lock" lines say what the
 * holds cost other threads, and 0 waits is a measured answer and not an assumption.
 * ------------------------------------------------------------------------ */

static bc250hsa_status range_ok(const bc250hsa_mem* mem, uint64_t offset, uint64_t bytes)
{
    if (mem == NULL || mem->va == 0u) {
        return BC250HSA_EINVAL;
    }
    if (offset > mem->bytes || bytes > mem->bytes - offset) {
        return BC250HSA_EINVAL;
    }
    return BC250HSA_OK;
}

/* The receipt of one call of section 5.1: the caller's, once its size is known, or a local
 * one when the caller passed NULL. It starts as "nothing submitted". NULL with *status
 * BC250HSA_EINVAL refuses a size this library does not know, and that receipt is left as
 * it was (bc250hsa.h rule 4). */
static bc250hsa_op_receipt* receipt_open(bc250hsa_op_receipt* given, bc250hsa_op_receipt* local,
                                         bc250hsa_status* status)
{
    if (given != NULL && !bc250hsa_struct_bytes_ok(given->struct_bytes, sizeof(*given))) {
        *status = BC250HSA_EINVAL;
        return NULL;
    }
    if (given == NULL) {
        memset(local, 0, sizeof(*local));
        local->struct_bytes = (uint32_t)sizeof(*local);
        given = local;
    }
    given->state = BC250HSA_OP_NOT_SUBMITTED;
    given->fence_value = 0u;
    *status = BC250HSA_OK;
    return given;
}

/* Can the host processor read this allocation? The answer comes from the allocation and
 * not from the operation the caller names: a host mapping in the handle, or a kind of
 * memory that has or may get one. The record's flags are the allocation's own, so a
 * handle whose flags a caller changed cannot hide them. */
static int cpu_readable(const bc250hsa_mem* mem)
{
    const bc250hsa_alloc_record* record = (const bc250hsa_alloc_record*)mem->opaque;
    uint32_t                     flags = mem->flags;

    if (mem->host != NULL) {
        return 1;
    }
    if (record != NULL) {
        flags |= record->flags;
    }
    return (flags & (BC250HSA_MEM_HOST | BC250HSA_MEM_HOST_WC | BC250HSA_MEM_EXEC |
                     BC250HSA_MEM_MAPPABLE)) != 0u;
}

/* The coherence gate of bc250hsa.h section 5.1 for one destination: nothing to ask when
 * the host processor cannot read it and the caller does not say it will; otherwise the
 * control of section 8 must have passed. dst may be NULL when host_reads is set. It runs
 * before anything is submitted, so a refusal here leaves the receipt at
 * BC250HSA_OP_NOT_SUBMITTED. */
static bc250hsa_status gate_destination(struct bc250hsa_device* dev, const bc250hsa_mem* dst,
                                        int host_reads)
{
    bc250hsa_status status;
    uint32_t        ready = 0u;

    if (!host_reads && !cpu_readable(dst)) {
        return BC250HSA_OK;
    }
    status = bc250hsa_gpu_copy_ready(dev, &ready);
    if (status != BC250HSA_OK) {
        return status;
    }
    return (ready != 0u) ? BC250HSA_OK : BC250HSA_EUNSUPPORTED;
}

/* The four staging slots, allocated on the first call that needs one. The caller holds
 * dev->lock. */
static bc250hsa_status stage_ensure(struct bc250hsa_device* dev)
{
    uint32_t i;

    if (dev->stage_ready != 0u) {
        return (dev->stage[0].va != 0u) ? BC250HSA_OK : BC250HSA_ENOMEM;
    }
    for (i = 0; i < BC250HSA_STAGE_SLOTS; i++) {
        bc250hsa_status status = bc250hsa_alloc(dev, BC250HSA_STAGE_SLOT_BYTES, 4096u,
                                                BC250HSA_MEM_HOST, &dev->stage[i]);
        if (status != BC250HSA_OK) {
            /* A slot that could not be allocated leaves the ones before it in place.
             * The ring works with fewer slots, and even one slot copies every size,
             * because a round is bounded by the slot size. */
            memset(&dev->stage[i], 0, sizeof(dev->stage[i]));
            break;
        }
        if (dev->stage[i].host == NULL) {
            (void)bc250hsa_free(dev, &dev->stage[i]);
            memset(&dev->stage[i], 0, sizeof(dev->stage[i]));
            break;
        }
        dev->stage_fence[i] = 0u;
    }
    dev->stage_ready = 1u;
    dev->stage_next = 0u;
    if (dev->stage[0].va == 0u) {
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "no staging slot of %u bytes could be allocated; a copy into memory that"
                     " is not ours keeps the host-processor path",
                     BC250HSA_STAGE_SLOT_BYTES);
        return BC250HSA_ENOMEM;
    }
    return BC250HSA_OK;
}

/* How many slots the ring really has. */
static uint32_t stage_count(const struct bc250hsa_device* dev)
{
    uint32_t i;
    for (i = 0; i < BC250HSA_STAGE_SLOTS; i++) {
        if (dev->stage[i].va == 0u) {
            break;
        }
    }
    return i;
}

/* The next slot whose last copy has retired. The caller holds dev->lock. */
static bc250hsa_status stage_take(struct bc250hsa_device* dev, uint32_t* slot_out)
{
    const uint32_t  slots = stage_count(dev);
    bc250hsa_status status;
    uint32_t        slot = 0u;

    if (slots == 0u) {
        return BC250HSA_ENOMEM;
    }
    status = bc250hsa_stage_pick(dev->stage_fence, slots, bc250hsa_fence_read(dev),
                                 dev->stage_next, &slot);
    if (status == BC250HSA_EBUSY) {
        /* Every slot is still in flight. Wait for the one this ring would take next,
         * which is the oldest, under this device's bound. */
        status = bc250hsa_wait_internal(dev, dev->stage_fence[slot]);
        if (status != BC250HSA_OK) {
            return (status == BC250HSA_ETIMEOUT) ? BC250HSA_EBUSY : status;
        }
        status = bc250hsa_stage_pick(dev->stage_fence, slots, bc250hsa_fence_read(dev),
                                     dev->stage_next, &slot);
    }
    if (status != BC250HSA_OK) {
        return status;
    }
    dev->stage_next = (slot + 1u) % slots;
    *slot_out = slot;
    return BC250HSA_OK;
}

/* --------------------------------------------------------------------------
 * The coherence control of CONTRACT.md section 8
 * ------------------------------------------------------------------------ */

#define CONTROL_BYTES 4096u
/* The three buffers of one control, in the order control_run allocates them. */
#define CONTROL_HOST   0u
#define CONTROL_DEVICE 1u
#define CONTROL_SEED   2u

static void control_pattern(uint32_t* dwords, uint32_t count, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        dwords[i] = seed ^ (i * 0x01010101u);
    }
}

/* One pass: put a pattern in the device buffer, dirty the host buffer's cache lines
 * with a filler, copy device to host with the command processor, wait, compare. The
 * caller holds dev->lock.
 *
 * *mismatch becomes 1 only when a copy retired and its bytes came back wrong, which is the
 * one coherence answer. Every other failure leaves it 0, and the receipt says whether a
 * submission of this pass is still outstanding. */
static bc250hsa_status control_pass(struct bc250hsa_device* dev, const bc250hsa_mem* bufs,
                                    void* device_host, uint32_t seed, uint32_t filler,
                                    uint64_t* first_bad, int* mismatch,
                                    bc250hsa_op_receipt* receipt)
{
    const bc250hsa_mem* host_buf = &bufs[CONTROL_HOST];
    const bc250hsa_mem* device_buf = &bufs[CONTROL_DEVICE];
    const bc250hsa_mem* seed_buf = &bufs[CONTROL_SEED];
    uint32_t            want[CONTROL_BYTES / 4u];
    const uint32_t      count = CONTROL_BYTES / 4u;
    bc250hsa_copy_op    op;
    bc250hsa_status     status;
    volatile uint32_t   sink = 0u;
    uint32_t            i;

    control_pattern(want, count, seed);
    if (device_host != NULL) {
        memcpy(device_host, want, CONTROL_BYTES);
        bc250hsa_write_barrier();
    } else {
        /* The device buffer has no host mapping on this part. The pattern goes in with
         * the same packet the control is about to measure, from a host buffer, so the
         * control still compares what the command processor wrote. */
        memcpy(seed_buf->host, want, CONTROL_BYTES);
        bc250hsa_write_barrier();
        op.kind = BC250HSA_COPY_OP_COPY;
        op.pattern = 0u;
        op.src_va = seed_buf->va;
        op.dst_va = device_buf->va;
        op.bytes = CONTROL_BYTES;
        status = bc250hsa_copy_run_ops(dev, &op, 1u, receipt);
        if (status != BC250HSA_OK) {
            return status;
        }
    }

    /* The arm that catches a page table entry that does not snoop: the host buffer's
     * lines are in this processor's cache, and dirty, before the device writes them.
     * A cold read would be served from memory and would hide the fault. */
    memset(host_buf->host, (int)(filler & 0xFFu), CONTROL_BYTES);
    for (i = 0; i < count; i++) {
        sink ^= ((const uint32_t*)host_buf->host)[i];
    }
    (void)sink;

    op.kind = BC250HSA_COPY_OP_COPY;
    op.pattern = 0u;
    op.src_va = device_buf->va;
    op.dst_va = host_buf->va;
    op.bytes = CONTROL_BYTES;
    status = bc250hsa_copy_run_ops(dev, &op, 1u, receipt);
    if (status != BC250HSA_OK) {
        return status;
    }
    for (i = 0; i < count; i++) {
        if (((const uint32_t*)host_buf->host)[i] != want[i]) {
            *first_bad = (uint64_t)i * 4u;
            *mismatch = 1;
            return BC250HSA_EUNSUPPORTED;
        }
    }
    return BC250HSA_OK;
}

static void control_free(struct bc250hsa_device* dev, bc250hsa_mem* bufs)
{
    uint32_t i;
    for (i = 0; i < 3u; i++) {
        if (bufs[i].va != 0u) {
            (void)bc250hsa_free(dev, &bufs[i]);
        }
        memset(&bufs[i], 0, sizeof(bufs[i]));
    }
}

/* The buffers an earlier control left held: freed once their fence value retired, and
 * waited for under this device's bound when it has not. A removal sentinel is not a
 * retired value. A failed wait keeps them held and returns its status, and no new control
 * runs while they are held. The caller holds dev->lock. */
static bc250hsa_status control_release_held(struct bc250hsa_device* dev)
{
    bc250hsa_status status;
    uint64_t        observed;

    if (dev->control_hold_fence == 0u) {
        return BC250HSA_OK;
    }
    observed = bc250hsa_fence_read(dev);
    if (observed == UINT64_MAX) {
        bc250hsa_sentinel_lost(dev, "the fence mapping reads UINT64_MAX; the held control"
                                    " buffers stay allocated", dev->control_hold_fence);
        return BC250HSA_EDEVICELOST;
    }
    if (observed < dev->control_hold_fence) {
        status = bc250hsa_wait_internal(dev, dev->control_hold_fence);
        if (status != BC250HSA_OK) {
            return bc250hsa_lost_status(dev, status);
        }
    }
    control_free(dev, dev->control_hold);
    dev->control_hold_fence = 0u;
    return BC250HSA_OK;
}

/* The whole control, once per device. The caller holds dev->lock.
 *
 * It answers BC250HSA_OK when the question was settled, whichever way: the device state is
 * then BC250HSA_CONTROL_PASSED or BC250HSA_CONTROL_FAILED and stays so for the life of the
 * device. FAILED comes from one thing only, bytes that came back wrong after a copy that
 * retired. Any other failure means the control could not ask the question: an allocation,
 * a mapping, a submission or a wait failed, or the device was lost. None of those says
 * anything about the page table entry, so the state stays UNTRIED, the counter
 * gpu_copy_control_failed is not raised, and the status goes back to the caller, which
 * returns it before it submits anything of its own.
 *
 * The buffers are freed when no submission of the control is outstanding, and held by the
 * device otherwise (control_release_held, bc250hsa.h section 5.1). */
static bc250hsa_status control_run(struct bc250hsa_device* dev)
{
    bc250hsa_mem        bufs[3];
    bc250hsa_op_receipt receipt;
    void*               device_host = NULL;
    bc250hsa_status     status;
    uint64_t            first_bad = 0u;
    int                 mismatch = 0;

    status = control_release_held(dev);
    if (status != BC250HSA_OK) {
        return status;
    }
    /* The control's copies go where every copy goes, so a copy context that cannot be
     * created is a control that cannot ask its question; nothing is allocated for it. */
    status = bc250hsa_copy_context_ready_locked(dev);
    if (status != BC250HSA_OK) {
        if (!dev->control_unrun_said) {
            dev->control_unrun_said = 1u;
            bc250hsa_log(BC250HSA_LOG_WARN,
                         "the GPU device-to-host control could not run (%s): no copy context",
                         bc250hsa_status_string(status));
        }
        return status;
    }
    memset(bufs, 0, sizeof(bufs));
    memset(&receipt, 0, sizeof(receipt));
    receipt.struct_bytes = (uint32_t)sizeof(receipt);

    status = bc250hsa_alloc(dev, CONTROL_BYTES, 4096u, BC250HSA_MEM_HOST, &bufs[CONTROL_HOST]);
    if (status == BC250HSA_OK) {
        status = bc250hsa_alloc(dev, CONTROL_BYTES, 4096u, BC250HSA_MEM_MAPPABLE,
                                &bufs[CONTROL_DEVICE]);
    }
    if (status == BC250HSA_OK && bufs[CONTROL_HOST].host != NULL) {
        if (bc250hsa_map(dev, &bufs[CONTROL_DEVICE], &device_host) != BC250HSA_OK) {
            device_host = NULL;
            status = bc250hsa_alloc(dev, CONTROL_BYTES, 4096u, BC250HSA_MEM_HOST,
                                    &bufs[CONTROL_SEED]);
            if (status == BC250HSA_OK && bufs[CONTROL_SEED].host == NULL) {
                status = BC250HSA_EUNSUPPORTED;
            }
        }
    } else if (status == BC250HSA_OK) {
        status = BC250HSA_EUNSUPPORTED;
    }
    /* Every submission and wait of the control is traced (submit.c, the fence checks). */
    dev->check_control = 1u;
    if (status == BC250HSA_OK) {
        status = control_pass(dev, bufs, device_host, 0xA5A5A5A5u, 0x00u, &first_bad,
                              &mismatch, &receipt);
    }
    if (status == BC250HSA_OK) {
        /* The second pattern, so that a pass cannot come from the buffer having held
         * the right bytes already. */
        status = control_pass(dev, bufs, device_host, 0x5A5A5A5Au, 0xFFu, &first_bad,
                              &mismatch, &receipt);
    }
    dev->check_control = 0u;
    status = bc250hsa_lost_status(dev, status);

    if (status == BC250HSA_OK) {
        dev->gpu_copy_control = BC250HSA_CONTROL_PASSED;
        bc250hsa_log(BC250HSA_LOG_INFO,
                     "the GPU device-to-host copy is enabled: two patterns of %u bytes came"
                     " back through a cached host mapping with no cache maintenance",
                     CONTROL_BYTES);
    } else if (mismatch) {
        dev->gpu_copy_control = BC250HSA_CONTROL_FAILED;
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_CONTROL_FAILED, 1u);
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "the GPU device-to-host control failed (first differing offset %llu);"
                     " every copy into memory the host processor reads keeps the"
                     " host-processor path",
                     (unsigned long long)first_bad);
        status = BC250HSA_OK;
    } else {
        dev->gpu_copy_control = BC250HSA_CONTROL_UNTRIED;
        if (!dev->control_unrun_said) {
            dev->control_unrun_said = 1u;
            bc250hsa_log(BC250HSA_LOG_WARN,
                         "the GPU device-to-host control could not run (%s); that is not a"
                         " coherence answer, so this copy returns the status and the next"
                         " one tries the control again",
                         bc250hsa_status_string(status));
        }
    }

    if (receipt.state == BC250HSA_OP_SUBMITTED) {
        memcpy(dev->control_hold, bufs, sizeof(bufs));
        dev->control_hold_fence = receipt.fence_value;
        bc250hsa_log(BC250HSA_LOG_WARN,
                     "the control buffers stay allocated until fence value %llu retires",
                     (unsigned long long)receipt.fence_value);
    } else {
        control_free(dev, bufs);
    }
    return status;
}

bc250hsa_status bc250hsa_gpu_copy_ready(struct bc250hsa_device* dev, uint32_t* ready)
{
    bc250hsa_status status = BC250HSA_OK;

    if (dev == NULL || ready == NULL) {
        return BC250HSA_EINVAL;
    }
    *ready = 0u;
    if (!dev->submit_capable) {
        return BC250HSA_EUNSUPPORTED;
    }
    /* A lost device is said by name and never as "not ready". The two answers reach layer 2
     * as different statuses and it treats them differently: it keeps a device loss and it
     * falls back to a host-processor read on an unsupported route, and that read would be a
     * read of a mapping the device no longer owns (CONTRACT.md section 7, device loss). */
    if (dev->device_lost) {
        return BC250HSA_EDEVICELOST;
    }
    bc250hsa_device_lock(dev);
    if (dev->gpu_copy_control == BC250HSA_CONTROL_UNTRIED) {
        /* The control is GPU copies and their waits under this hold, so the counters of the
         * device lock see it as one copy hold. _leave releases the inner level only. */
        bc250hsa_copy_lock_enter(dev);
        status = control_run(dev);
        bc250hsa_copy_lock_leave(dev);
    }
    if (status == BC250HSA_OK && dev->device_lost) {
        status = BC250HSA_EDEVICELOST;
    }
    *ready = (status == BC250HSA_OK && dev->gpu_copy_control == BC250HSA_CONTROL_PASSED)
                 ? 1u : 0u;
    LeaveCriticalSection(&dev->lock);
    return status;
}

/* --------------------------------------------------------------------------
 * The three entry points
 * ------------------------------------------------------------------------ */

bc250hsa_status bc250hsa_copy_device_to_device(struct bc250hsa_device* dev,
                                              const bc250hsa_mem* dst, uint64_t dst_offset,
                                              const bc250hsa_mem* src, uint64_t src_offset,
                                              uint64_t bytes, uint32_t flags,
                                              bc250hsa_op_receipt* receipt)
{
    const uint64_t      start = bc250hsa_ticks();
    bc250hsa_op_receipt local;
    bc250hsa_copy_op    op;
    bc250hsa_status     status;

    receipt = receipt_open(receipt, &local, &status);
    if (receipt == NULL) {
        return status;
    }
    if (dev == NULL) {
        return BC250HSA_EINVAL;
    }
    if (bytes == 0u) {
        return BC250HSA_OK;
    }
    status = range_ok(dst, dst_offset, bytes);
    if (status != BC250HSA_OK) {
        return status;
    }
    status = range_ok(src, src_offset, bytes);
    if (status != BC250HSA_OK) {
        return status;
    }
    if ((flags & ~BC250HSA_COPY_HOST_READS_DST) != 0u) {
        return BC250HSA_EINVAL;
    }
    /* The gate follows the destination, with or without the flag: a device-to-device copy
     * into the GPU address of a host allocation (hipHostGetDevicePointer) is a write the
     * host processor reads through its own mapping afterwards. */
    status = gate_destination(dev, dst, (flags & BC250HSA_COPY_HOST_READS_DST) != 0u);
    if (status != BC250HSA_OK) {
        return status;
    }
    op.kind = BC250HSA_COPY_OP_COPY;
    op.pattern = 0u;
    op.src_va = src->va + src_offset;
    op.dst_va = dst->va + dst_offset;
    op.bytes = bytes;
    status = bc250hsa_copy_run_ops(dev, &op, 1u, receipt);
    if (status != BC250HSA_OK) {
        return status;
    }
    /* The counter follows the caller's word, the gate does not: the flag says where the
     * bytes are going, and the five counters are read by that. */
    if ((flags & BC250HSA_COPY_HOST_READS_DST) != 0u) {
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2H, 1u);
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2H_BYTES, bytes);
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2H_TICKS, bc250hsa_ticks() - start);
    } else {
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2D, 1u);
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2D_BYTES, bytes);
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2D_TICKS, bc250hsa_ticks() - start);
    }
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_copy_from_device_gpu(struct bc250hsa_device* dev, void* dst,
                                              const bc250hsa_mem* src, uint64_t src_offset,
                                              uint64_t bytes, bc250hsa_op_receipt* receipt)
{
    const uint64_t      start = bc250hsa_ticks();
    bc250hsa_op_receipt local;
    bc250hsa_status     status;
    uint64_t            done = 0u;

    receipt = receipt_open(receipt, &local, &status);
    if (receipt == NULL) {
        return status;
    }
    if (dev == NULL || dst == NULL) {
        return BC250HSA_EINVAL;
    }
    if (bytes == 0u) {
        return BC250HSA_OK;
    }
    status = range_ok(src, src_offset, bytes);
    if (status != BC250HSA_OK) {
        return status;
    }
    /* The destination is the program's own memory, which the host processor reads by
     * definition, through a staging slot that is ours and cached. */
    status = gate_destination(dev, NULL, 1);
    if (status != BC250HSA_OK) {
        return status;
    }

    bc250hsa_copy_lock_enter(dev);
    status = stage_ensure(dev);
    while (status == BC250HSA_OK && done < bytes) {
        const uint64_t      round = bc250hsa_stage_round(bytes - done, BC250HSA_STAGE_SLOT_BYTES);
        bc250hsa_op_receipt round_receipt;
        bc250hsa_copy_op    op;
        uint32_t            slot = 0u;

        status = stage_take(dev, &slot);
        if (status != BC250HSA_OK) {
            break;
        }
        memset(&round_receipt, 0, sizeof(round_receipt));
        round_receipt.struct_bytes = (uint32_t)sizeof(round_receipt);
        op.kind = BC250HSA_COPY_OP_COPY;
        op.pattern = 0u;
        op.src_va = src->va + src_offset + done;
        op.dst_va = dev->stage[slot].va;
        op.bytes = round;
        status = bc250hsa_copy_run_ops(dev, &op, 1u, &round_receipt);
        /* The slot stays taken until this value retires, whether the round's wait worked
         * or not: a submission that was accepted and then not waited out still writes it.
         * A round the device never accepted leaves 0, which frees the slot. */
        dev->stage_fence[slot] = round_receipt.fence_value;
        receipt->fence_value = round_receipt.fence_value;
        if (status != BC250HSA_OK) {
            /* The rounds before this one retired, so this round is the whole of what may
             * still be outstanding. */
            receipt->state = round_receipt.state;
            break;
        }
        /* The slot is host-visible and cached, so this read runs at memory speed and
         * not at the 0.21 GB/s of an uncached read of device memory. */
        memcpy((uint8_t*)dst + done, dev->stage[slot].host, (size_t)round);
        done += round;
    }
    bc250hsa_copy_lock_leave(dev);
    if (status != BC250HSA_OK) {
        if (receipt->state == BC250HSA_OP_NOT_SUBMITTED) {
            receipt->fence_value = 0u;
        }
        return bc250hsa_lost_status(dev, status);
    }
    receipt->state = BC250HSA_OP_RETIRED;
    bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2H_STAGED, 1u);
    bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2H_STAGED_BYTES, bytes);
    bc250hsa_count_add(BC250HSA_C_GPU_COPY_D2H_STAGED_TICKS, bc250hsa_ticks() - start);
    return BC250HSA_OK;
}

/* The partial bytes at either end of a fill. bc250hsa_pm4_append_fill needs a 4-byte
 * aligned address and size, so one to three bytes at an end of the range cannot ride in a
 * fill packet. They ride in a copy packet instead: the host processor writes them into a
 * staging slot, and one DMA_DATA of one to three bytes carries them to the destination. A
 * copy has no alignment rule on this part (CONTRACT.md section 7, the alignment hazard).
 *
 * Why not the dword around them: reading that dword back would be a device-to-host read of
 * device memory by the host processor, which is the one access the coherence control of
 * section 8 gates, and a fill does not run that control. Writing a staging slot is the
 * other direction, which every dispatch's kernarg buffer already proves, so this path has
 * no coherence question of its own and nothing stale can be written back.
 *
 * The caller holds dev->lock and owns the slot until the fence this list is submitted
 * under retires. Eight bytes of the slot are enough: the head takes the first four and the
 * tail the next four, and both hold the same value. */
static bc250hsa_status fill_edges(struct bc250hsa_device* dev, const bc250hsa_fill_plan* plan,
                                  uint8_t value, bc250hsa_copy_op* ops, uint32_t* op_count,
                                  uint32_t* slot_out)
{
    bc250hsa_status status;
    uint32_t        slot = 0u;

    status = stage_ensure(dev);
    if (status != BC250HSA_OK) {
        return status;
    }
    status = stage_take(dev, &slot);
    if (status != BC250HSA_OK) {
        return status;
    }
    memset(dev->stage[slot].host, (int)value, 8u);
    bc250hsa_write_barrier();
    if (plan->head_bytes != 0u) {
        ops[*op_count].kind = BC250HSA_COPY_OP_COPY;
        ops[*op_count].pattern = 0u;
        ops[*op_count].src_va = dev->stage[slot].va;
        ops[*op_count].dst_va = plan->head_va;
        ops[*op_count].bytes = plan->head_bytes;
        (*op_count)++;
    }
    if (plan->tail_bytes != 0u) {
        ops[*op_count].kind = BC250HSA_COPY_OP_COPY;
        ops[*op_count].pattern = 0u;
        ops[*op_count].src_va = dev->stage[slot].va + 4u;
        ops[*op_count].dst_va = plan->tail_va;
        ops[*op_count].bytes = plan->tail_bytes;
        (*op_count)++;
    }
    *slot_out = slot;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_fill_device(struct bc250hsa_device* dev, const bc250hsa_mem* dst,
                                     uint64_t dst_offset, uint8_t value, uint64_t bytes,
                                     bc250hsa_op_receipt* receipt)
{
    const uint64_t      start = bc250hsa_ticks();
    bc250hsa_op_receipt local;
    bc250hsa_copy_op    ops[3];
    bc250hsa_fill_plan  plan;
    uint32_t            op_count = 0u;
    uint32_t            slot = 0u;
    int                 have_slot = 0;
    bc250hsa_status     status;

    receipt = receipt_open(receipt, &local, &status);
    if (receipt == NULL) {
        return status;
    }
    if (dev == NULL) {
        return BC250HSA_EINVAL;
    }
    if (bytes == 0u) {
        return BC250HSA_OK;
    }
    status = range_ok(dst, dst_offset, bytes);
    if (status != BC250HSA_OK) {
        return status;
    }
    /* A destination the host processor can read may be read later with no further copy,
     * and then this fill's bytes are bytes the command processor wrote and the host
     * processor reads. That is the access the control of section 8 answers, so the fill asks
     * it too. A destination with no way to the host processor needs no answer. */
    status = gate_destination(dev, dst, 0);
    if (status != BC250HSA_OK) {
        return status;
    }
    /* Three parts: one to three head bytes, a 4-byte aligned interior the command
     * processor fills, and one to three tail bytes. The split is a pure decision and is
     * tested against a memset oracle with no device (tests/host/test_pm4.c). */
    status = bc250hsa_fill_split(dst->va + dst_offset, bytes, &plan);
    if (status != BC250HSA_OK) {
        return status;
    }

    bc250hsa_copy_lock_enter(dev);
    if (plan.head_bytes != 0u || plan.tail_bytes != 0u) {
        status = fill_edges(dev, &plan, value, ops, &op_count, &slot);
        have_slot = (status == BC250HSA_OK) ? 1 : 0;
    }
    if (status == BC250HSA_OK && plan.interior_bytes != 0u) {
        ops[op_count].kind = BC250HSA_COPY_OP_FILL;
        ops[op_count].pattern = (uint32_t)value * 0x01010101u;
        ops[op_count].src_va = 0u;
        ops[op_count].dst_va = plan.interior_va;
        ops[op_count].bytes = plan.interior_bytes;
        op_count++;
    }
    if (status == BC250HSA_OK && op_count != 0u) {
        status = bc250hsa_copy_run_ops(dev, ops, op_count, receipt);
    }
    if (have_slot) {
        /* The slot stays taken until this value retires, whether the list ran or not: a
         * submission that was sent and then not waited out still reads it. A list the
         * device never accepted leaves 0, which frees the slot. */
        dev->stage_fence[slot] = receipt->fence_value;
    }
    bc250hsa_copy_lock_leave(dev);
    if (status != BC250HSA_OK) {
        return bc250hsa_lost_status(dev, status);
    }
    bc250hsa_count_add(BC250HSA_C_GPU_FILL, 1u);
    bc250hsa_count_add(BC250HSA_C_GPU_FILL_BYTES, bytes);
    bc250hsa_count_add(BC250HSA_C_GPU_FILL_TICKS, bc250hsa_ticks() - start);
    return BC250HSA_OK;
}

/* --------------------------------------------------------------------------
 * The allocator seam
 * ------------------------------------------------------------------------ */

static bc250hsa_status device_alloc_shim(void* ctx, uint64_t bytes, uint64_t alignment,
                                         uint32_t flags, bc250hsa_mem* out)
{
    return bc250hsa_alloc((struct bc250hsa_device*)ctx, bytes, alignment, flags, out);
}

static void device_free_shim(void* ctx, bc250hsa_mem* mem)
{
    (void)bc250hsa_free((struct bc250hsa_device*)ctx, mem);
}

bc250hsa_status bc250hsa_device_allocator(struct bc250hsa_device* dev, bc250hsa_allocator* out)
{
    if (dev == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    out->ctx = dev;
    out->alloc = device_alloc_shim;
    out->free = device_free_shim;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_module_load(struct bc250hsa_device* dev, const void* image,
                                     size_t image_bytes, struct bc250hsa_module** out)
{
    bc250hsa_allocator alloc;
    bc250hsa_status    status;

    status = bc250hsa_device_allocator(dev, &alloc);
    if (status != BC250HSA_OK) {
        return status;
    }
    /* A flush point, and the reason the light barrier may leave the instruction cache
     * alone: no code object is loaded while one indirect buffer is being built, so the
     * acquire at the head of that buffer is the only instruction cache invalidate it
     * needs (section 8.1 and pm4_regs.h). */
    (void)bc250hsa_flush(dev, NULL);
    return bc250hsa_module_load_alloc(&alloc, image, image_bytes, out);
}
