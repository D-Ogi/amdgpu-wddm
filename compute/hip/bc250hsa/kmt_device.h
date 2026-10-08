/* kmt_device.h - the Windows half of bc250hsa: the device structure that kmt_device.c,
 * kmt_memory.c and submit.c share.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, sections 3.2, 3.3 and 3.8. No host test
 * includes this file: the loader, the packer and the PM4 builder never touch a
 * display driver interface call.
 */
#ifndef BC250HSA_KMT_DEVICE_H
#define BC250HSA_KMT_DEVICE_H

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>   /* NTSTATUS; d3dkmthk.h needs it and does not include it */
#include <d3dkmthk.h>

#include "internal.h"
#include "kmt_blobs.h"

/* One allocation this device owns. bc250hsa_mem::opaque points here, and the device
 * keeps every record on a list so that bc250hsa_close() frees what the caller left. */
typedef struct bc250hsa_alloc_record {
    struct bc250hsa_alloc_record* next;
    struct bc250hsa_alloc_record* prev;
    D3DKMT_HANDLE                 handle;
    uint64_t                      va;
    uint64_t                      bytes;
    uint32_t                      flags;
    void*                         host;      /* the Lock2 mapping, or NULL */
} bc250hsa_alloc_record;

#define BC250HSA_RING_SLOTS_DEFAULT      8u
#define BC250HSA_RING_SLOT_BYTES_DEFAULT 65536u
#define BC250HSA_VA_WINDOW_GIB_DEFAULT   64u
#define BC250HSA_WAIT_SLICE_MS_DEFAULT   1000u
#define BC250HSA_WAIT_TOTAL_MS_DEFAULT   120000u
/* One zeroed page per device backs the private segment buffer, which clang always
 * requests even when PRIVATE_SEGMENT_FIXED_SIZE is 0. */
#define BC250HSA_ZERO_PAGE_BYTES         4096u

struct bc250hsa_device {
    CRITICAL_SECTION lock;
    uint32_t         flags;

    D3DKMT_HANDLE adapter;
    D3DKMT_HANDLE device;
    D3DKMT_HANDLE paging_queue;
    D3DKMT_HANDLE paging_fence;
    D3DKMT_HANDLE context;
    D3DKMT_HANDLE fence;

    volatile uint64_t* fence_cpu;     /* FenceValueCPUVirtualAddress */
    uint64_t           fence_gpu_va;  /* FenceValueGPUVirtualAddress */
    uint64_t           fence_last_submitted;

    uint64_t va_window_start;
    uint64_t va_window_end;

    bc250hsa_alloc_record* allocations;

    /* The command ring: one indirect buffer slot per submission in flight. The
     * kernel driver runs one indirect buffer at a time, so a slot is reused only
     * after its own fence value retired. */
    bc250hsa_mem ring;
    uint32_t     ring_slots;
    uint32_t     ring_slot_bytes;
    uint64_t*    slot_fence;          /* ring_slots values */
    uint32_t     next_slot;

    bc250hsa_mem zero_page;
    uint32_t     private_segment_rsrc[4];

    uint32_t* last_ib;                /* a host copy of the last submitted buffer */
    uint32_t  last_ib_dwords;

    uint8_t caps[BC250HSA_CAPS_BYTES];
    int     caps_valid;

    int device_lost;
    int submit_capable;               /* 0 with BC250HSA_OPEN_NO_GPU_SUBMIT */
};

/* kmt_device.c */
bc250hsa_status bc250hsa_os_status(const char* what, NTSTATUS status);
void            bc250hsa_mark_lost(struct bc250hsa_device* dev, const char* why);
int             bc250hsa_device_executing(struct bc250hsa_device* dev);

/* kmt_memory.c */
bc250hsa_status bc250hsa_wait_paging_fence(struct bc250hsa_device* dev, uint64_t value);
void            bc250hsa_free_all_allocations(struct bc250hsa_device* dev);

#endif /* BC250HSA_KMT_DEVICE_H */
