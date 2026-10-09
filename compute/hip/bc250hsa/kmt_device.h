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
/* The local memory a workgroup may ask for on gfx10.1. It is a property of the part and
 * not of the capability blob, which carries no local memory size. bc250hsa_props_read
 * reports it and the device keeps it, so the dispatch path needs no property read. */
#define BC250HSA_LDS_BYTES_PER_WORKGROUP 65536u

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
    uint64_t           fence_last_submitted;   /* the last value handed to SubmitCommand */
    /* The last value this device promised a caller. It is fence_last_submitted, except
     * while a batch is open: then it is that batch's value, which no submission carries
     * yet (section 8.1 of the interface). bc250hsa_fence_last_submitted() reports this
     * one, because a caller means "everything this device owes" by it. */
    uint64_t           fence_last_assigned;

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
    uint32_t  last_ib_capacity;       /* dwords; one command ring slot */

    /* The local memory a workgroup may ask for, read from the capability blob at open.
     * bc250hsa_pm4_check_dispatch needs it on every dispatch, and build 1 read the whole
     * property structure for that one number. */
    uint32_t lds_bytes_per_workgroup;

    /* Batching, section 8.1 of the interface. The policy is the caller's; the state
     * below is the indirect buffer this device is building now. */
    bc250hsa_batch_policy batch;
    int      batch_open;
    uint32_t batch_slot;
    uint32_t batch_dwords;            /* dwords in the open buffer, the tail apart */
    uint32_t batch_count;             /* dispatches in the open buffer */
    uint64_t batch_fence_value;       /* the value its one completion write will hold */
    uint64_t batch_opened_us;         /* bc250hsa_now_us() when it was opened */

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

/* submit.c. A monotonic host clock in microseconds, for the time cap of a batch. */
uint64_t bc250hsa_now_us(void);
/* Submits the open batch of this device. The caller holds dev->lock. It is BC250HSA_OK
 * when nothing is open. Every call that changes memory a dispatch of the open buffer may
 * name calls it first (section 8.1 of the interface). */
bc250hsa_status bc250hsa_batch_submit_locked(struct bc250hsa_device* dev);

#endif /* BC250HSA_KMT_DEVICE_H */
