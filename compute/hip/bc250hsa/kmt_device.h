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
#include "../../../driver/shim/include/bc250_hip_journal.h"

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

/* The staging ring of scratch/hip-gpu-copy/CONTRACT.md section 5b. Four slots of 1 MiB:
 * 4 MiB of host memory per device that copies into a program's own buffer, and nothing
 * for a process whose destinations are all its own, which is the llama.cpp case
 * (CONTRACT.md section 2). */
#define BC250HSA_STAGE_SLOTS      4u
#define BC250HSA_STAGE_SLOT_BYTES (1024u * 1024u)

/* The three states of the coherence control of CONTRACT.md section 8. */
#define BC250HSA_CONTROL_UNTRIED 0u
#define BC250HSA_CONTROL_PASSED  1u
#define BC250HSA_CONTROL_FAILED  2u

/* The fence checks of submit.c: one submission whose fence this device reports twice, once
 * when it goes to the kernel driver and once when a wait for it ends. The device traces the
 * submissions of the coherence control, its first two ordinary submissions and its first two
 * copies (scratch/hip-gpu-copy/CONTRACT.md deviation 13.23), so that a lab round that loses
 * the device says by itself which object, which mapping and which values it read. */
#define BC250HSA_FENCE_CHECKS 8u
/* This tree keeps an unretired copy's value and holds every submission of the device behind
 * it (copy_unretired below, submit.c). tests/host/test_copy_lifecycle.c also builds against
 * older trees as negative controls, and asks for this name. */
#define BC250HSA_COPY_EXCLUSION 1u
typedef struct bc250hsa_fence_check {
    const char*   site;
    uint64_t      target;     /* the value the submission's completion write holds */
    uint64_t      before;     /* the fence mapping just before SubmitCommand */
    uint64_t      upload_id;  /* the journal upload of the buffer, 0 for none */
    D3DKMT_HANDLE context;
    uint32_t      records;    /* dispatch records the buffer carried to the journal */
} bc250hsa_fence_check;

struct bc250hsa_device {
    CRITICAL_SECTION lock;
    uint32_t         flags;

    D3DKMT_HANDLE adapter;
    D3DKMT_HANDLE device;
    D3DKMT_HANDLE paging_queue;
    D3DKMT_HANDLE paging_fence;
    D3DKMT_HANDLE context;
    D3DKMT_HANDLE fence;
    /* The second context of this device, for an indirect buffer that holds copy and fill
     * packets and no dispatch. The kernel driver binds every buffer of a context whose
     * journal is on to an uploaded dispatch record and refuses the rest with
     * STATUS_INVALID_PARAMETER, after which the operating system puts the whole device in
     * an error state (driver/kmd/hip_journal.c, HipJournalBind; WDK
     * DxgkDdiSubmitCommandVirtual). This context never uploads a record, so its journal
     * never turns on. Created on the first such buffer; copy_context_failed is 1 once
     * that creation failed, and no copy is then submitted at all. */
    D3DKMT_HANDLE copy_context;
    int           copy_context_failed;
    /* The fence value of a copy whose wait ended without establishing retirement: a timeout,
     * or a wait that Windows would not create or register. Its buffer may still run on
     * copy_context. A buffer of dev->context submitted now could run first and write a higher
     * value into the one fence, which wakes every wait for the copy while its bytes still
     * move, and the copy's late write would then move the fence backwards. So no submission of
     * this device, on either context, goes out until the fence reaches this value
     * (copy_exclusion_locked, submit.c). 0 when no copy is outstanding. Only dev->lock reads
     * or writes it. */
    uint64_t      copy_unretired;
    /* The status the last wait for that copy ended with: its own wait in
     * bc250hsa_copy_run_ops, or the wait of copy_exclusion_locked that refused a later
     * submission. bc250hsa_copy_unretired answers it to a caller that would move bytes with
     * the host processor. Only dev->lock reads or writes it. */
    bc250hsa_status copy_unretired_status;

    volatile uint64_t* fence_cpu;     /* FenceValueCPUVirtualAddress */
    uint64_t           fence_gpu_va;  /* FenceValueGPUVirtualAddress */
    /* What the monitored fence was created with, for the fence checks. */
    uint32_t           fence_type;
    uint32_t           fence_flags;
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
    bc250hsa_mem* slot_scratch;       /* independent BO per retired IB slot */

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
    /* The compute state the last dispatch of the open buffer left in the hardware, so
     * that the next one writes only what changed (section 8.8). It is cleared when a
     * buffer opens, because another context's buffer runs between two of ours. */
    bc250hsa_pm4_state batch_state;

    /* A bounded host staging buffer; only dev->lock may access it. One record
     * per HIP dispatch, retained by KMD before the matching IB is submitted. */
    uint8_t* journal_upload;
    uint32_t journal_bytes;
    uint32_t journal_count;
    int journal_required;

    uint8_t caps[BC250HSA_CAPS_BYTES];
    int     caps_valid;

    /* The GPU copy route of scratch/hip-gpu-copy/CONTRACT.md. The staging ring is the
     * destination of a copy into memory that is not ours (section 5b): four
     * host-visible, cached, GPU-addressable slots, allocated on the first call that
     * needs one and freed with the device. Each slot carries the fence value of the
     * copy that last wrote it, which is the retire rule the command ring slots use. */
    bc250hsa_mem stage[BC250HSA_STAGE_SLOTS];
    uint64_t     stage_fence[BC250HSA_STAGE_SLOTS];
    uint32_t     stage_next;
    uint32_t     stage_ready;         /* 1 once the four slots exist */
    /* The coherence control of section 8: BC250HSA_CONTROL_* below. */
    uint32_t     gpu_copy_control;
    /* The buffers of a control whose submission did not retire, and the fence value that
     * frees them. A freed GPU address may be handed out again at once, so buffers a
     * submission may still write stay allocated until that value retires; the next
     * control frees them first, and bc250hsa_close frees or quarantines them with every
     * other allocation. control_hold_fence is 0 when nothing is held. */
    bc250hsa_mem control_hold[3];
    uint64_t     control_hold_fence;
    /* 1 once the log said that a control could not run, so that a resource failure that
     * repeats on every copy is said one time. */
    uint32_t     control_unrun_said;
    /* The bound of the waits this library performs for itself, which only the GPU copy
     * needs: 0 and 0 take the library defaults, and bc250hsa_wait_policy_set puts the
     * process's own numbers here (bc250hsa.h rule 6). */
    uint32_t     wait_slice_ms;
    uint32_t     wait_total_ms;

    /* The fence checks: the traced submissions whose wait has not been reported yet, how
     * many ordinary submissions and copies are still to be traced, and 1 while the
     * coherence control runs, which traces all of its own. Only dev->lock reads or writes
     * them. */
    bc250hsa_fence_check checks[BC250HSA_FENCE_CHECKS];
    uint32_t             check_count;
    uint32_t             check_dispatch_left;
    uint32_t             check_copy_left;
    uint32_t             check_control;
    /* The last submission this device made, for the report of a removal sentinel. */
    bc250hsa_fence_check last_submit;

    /* What the GPU copy's hold of dev->lock costs other threads (CONTRACT.md deviation
     * 13.23). copy_lock_depth is above 0 while a copy, a staged copy, a fill or the coherence
     * control holds the lock, and bc250hsa_device_lock reads it without the lock to tell
     * whether a thread that had to wait waited for a copy. copy_lock_start is the tick at the
     * outermost entry. copy_lock_holds counts the outermost holds, and the summary line is
     * said at the first, second, fourth, eighth one and so on. copy_lock_waits_said counts
     * the waits said one by one, which stops at BC250HSA_COPY_LOCK_WAITS_SAID. */
    volatile LONG copy_lock_depth;
    uint64_t      copy_lock_start;
    uint64_t      copy_lock_holds;
    uint32_t      copy_lock_waits_said;

    int closing;
    int device_lost;
    int submit_capable;               /* 0 with BC250HSA_OPEN_NO_GPU_SUBMIT */
};

/* kmt_device.c */
bc250hsa_status bc250hsa_os_status(const char* what, NTSTATUS status);
void            bc250hsa_mark_lost(struct bc250hsa_device* dev, const char* why);
/* One node-0 context of this device with the context blob of the main one. Device open uses
 * it for dev->context and submit.c for dev->copy_context. */
bc250hsa_status bc250hsa_context_create(struct bc250hsa_device* dev, D3DKMT_HANDLE* out);

/* submit.c, beside the waits that read them, so that the host tests run the real
 * D3DKMTGetDeviceState query over their fake kernel. 1 unless the device reports an
 * execution state other than active; a query that fails is no evidence of a loss. */
int             bc250hsa_device_executing(struct bc250hsa_device* dev);
/* The execution state of D3DKMT_GETDEVICESTATE (D3DKMT_DEVICEEXECUTION_STATE in d3dkmthk.h:
 * 1 active, 2 reset, 3 hung, 4 stopped, 5 out of memory, 6 DMA fault, 7 DMA page fault), or
 * the NTSTATUS of a query that failed, which is negative. Read for the fence checks only; a
 * value here decides nothing. */
int32_t         bc250hsa_device_execution_state(struct bc250hsa_device* dev);

/* submit.c. Every acquisition of dev->lock in this library goes through one of these three.
 * bc250hsa_device_lock is EnterCriticalSection that measures: a thread that finds the lock
 * held while copy_lock_depth is above 0 waited for a GPU copy, and that wait is counted
 * (gpu_copy_lock_waits, bc250hsa.h section 3) and said at INFO for the first
 * BC250HSA_COPY_LOCK_WAITS_SAID of a device. bc250hsa_copy_lock_enter and _leave bracket a
 * copy's whole hold, nested holds included, and count the outermost one
 * (gpu_copy_lock_holds). _leave releases the lock. */
#define BC250HSA_COPY_LOCK_WAITS_SAID 4u
void            bc250hsa_device_lock(struct bc250hsa_device* dev);
void            bc250hsa_copy_lock_enter(struct bc250hsa_device* dev);
void            bc250hsa_copy_lock_leave(struct bc250hsa_device* dev);

/* kmt_memory.c */
bc250hsa_status bc250hsa_wait_paging_fence(struct bc250hsa_device* dev, uint64_t value);
void            bc250hsa_free_all_allocations(struct bc250hsa_device* dev);

/* submit.c. A monotonic host clock in microseconds, for the time cap of a batch. */
uint64_t bc250hsa_now_us(void);
/* Submits the open batch of this device. The caller holds dev->lock. It is BC250HSA_OK
 * when nothing is open. Every call that changes memory a dispatch of the open buffer may
 * name calls it first (section 8.1 of the interface). */
bc250hsa_status bc250hsa_batch_submit_locked(struct bc250hsa_device* dev);
/* submit.c. Puts a list of copy and fill operations in an indirect buffer of its own on
 * dev->copy_context, submits it and waits for its fence under this device's wait bound.
 * Every public GPU copy of kmt_memory.c ends with this, which is what makes the call
 * complete when it returns.
 *
 * The buffer carries no dispatch, so it carries no dispatch record either, and the kernel
 * driver refuses a buffer without a record on a context whose journal is on (see
 * copy_context above). It therefore never goes to dev->context and never joins a batch.
 * Both contexts write the one monitored fence of this device, and the scheduler orders
 * two contexts by nothing, so the fence stays in order only if the copy runs alone: it
 * submits the open batch, waits until everything this device submitted has retired,
 * submits the copy, and waits for the copy, all inside one hold of dev->lock
 * (CONTRACT.md deviation 13.23). The list goes whole or not at all, and a list that no
 * empty buffer holds is BC250HSA_ENOMEM.
 *
 * receipt is required and is written on every return (bc250hsa.h section 5.1): NOT_SUBMITTED
 * when the list was refused or its submission failed, SUBMITTED with the fence value when
 * the wait failed after the device accepted the list, RETIRED on success. A SUBMITTED list
 * leaves its value in dev->copy_unretired, which holds every later submission of the device
 * until the fence reaches it. */
bc250hsa_status bc250hsa_copy_run_ops(struct bc250hsa_device* dev, const bc250hsa_copy_op* ops,
                                      uint32_t count, bc250hsa_op_receipt* receipt);
/* submit.c. BC250HSA_OK when dev->copy_context exists or could be created now, the status
 * of the creation otherwise (BC250HSA_EOS on every call after the first failure). The caller
 * holds dev->lock. The coherence control asks before it allocates anything. */
bc250hsa_status bc250hsa_copy_context_ready_locked(struct bc250hsa_device* dev);
/* submit.c. The report of a fence mapping that reads UINT64_MAX, the removal sentinel
 * (ID3D12Fence::GetCompletedValue documents the same value for a removed device), or of an
 * execution state that is no longer active: one "fence check: device lost" line with the
 * execution state, the fence object, its mapping, the value waited for and the last
 * submission, then the device is marked lost with why. */
void bc250hsa_sentinel_lost(struct bc250hsa_device* dev, const char* why, uint64_t target);

/* A failure on a device this library marked lost is BC250HSA_EDEVICELOST, whatever Windows
 * call failed first, so that no caller mistakes it for a refusal it may work around
 * (bc250hsa.h section 5.1). */
static __inline bc250hsa_status bc250hsa_lost_status(const struct bc250hsa_device* dev,
                                                     bc250hsa_status status)
{
    return (status != BC250HSA_OK && dev->device_lost) ? BC250HSA_EDEVICELOST : status;
}
/* submit.c. bc250hsa_wait under this device's own bound. */
bc250hsa_status bc250hsa_wait_internal(struct bc250hsa_device* dev, uint64_t value);

#endif /* BC250HSA_KMT_DEVICE_H */
