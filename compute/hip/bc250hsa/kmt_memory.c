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

bc250hsa_status bc250hsa_alloc(struct bc250hsa_device* dev, uint64_t bytes, uint64_t alignment,
                               uint32_t flags, bc250hsa_mem* out)
{
    struct bc250hsa_alloc_blob         blob;
    struct bc250hsa_create_alloc_pdata pdata;
    D3DDDI_ALLOCATIONINFO2             info;
    D3DKMT_CREATEALLOCATION            create;
    D3DDDI_MAPGPUVIRTUALADDRESS        map;
    D3DDDI_MAKERESIDENT                resident;
    bc250hsa_alloc_record*             record;
    bc250hsa_status                    result;
    NTSTATUS                           status;
    uint64_t                           paging_fence = 0;
    uint64_t                           phys_alignment;
    uint64_t                           phys_bytes;

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

    memset(&blob, 0, sizeof(blob));
    blob.magic = BC250HSA_ALLOC_MAGIC;
    blob.version = BC250HSA_ALLOC_VERSION;
    blob.size = (uint32_t)sizeof(blob);
    blob.alloc_size = phys_bytes;
    blob.phys_alignment = phys_alignment;
    blob.preferred_heap = is_device_local(flags) ? BC250HSA_HEAP_VRAM : BC250HSA_HEAP_GTT;
    blob.va_size = phys_bytes;
    if (wants_host_mapping(flags) || (flags & BC250HSA_MEM_MAPPABLE) != 0u) {
        blob.gem_flags |= BC250HSA_GEM_CPU_ACCESS_REQUIRED;
    } else {
        blob.gem_flags |= BC250HSA_GEM_NO_CPU_ACCESS;
    }
    if ((flags & BC250HSA_MEM_HOST_WC) != 0u) {
        blob.gem_flags |= BC250HSA_GEM_CPU_GTT_USWC;
    }

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
        result = bc250hsa_os_status("CreateAllocation2", status);
        free(record);
        return result;
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
        result = bc250hsa_os_status("MapGpuVirtualAddress", status);
        goto failed;
    }
    record->va = map.VirtualAddress;
    require_paging_fence(&paging_fence, map.PagingFenceValue);

    memset(&resident, 0, sizeof(resident));
    resident.hPagingQueue = dev->paging_queue;
    resident.NumAllocations = 1;
    resident.AllocationList = &record->handle;
    resident.Flags.MustSucceed = 1;
    status = D3DKMTMakeResident(&resident);
    if (!NT_SUCCESS(status)) {
        result = bc250hsa_os_status("MakeResident", status);
        goto failed;
    }
    require_paging_fence(&paging_fence, resident.PagingFenceValue);

    result = bc250hsa_wait_paging_fence(dev, paging_fence);
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

    EnterCriticalSection(&dev->lock);
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
    EnterCriticalSection(&dev->lock);
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
