/* test_copy_lifecycle.c - the GPU copy and fill of bc250hsa.h section 5.1 through the
 * production code of layer 1. kmt_memory.c and submit.c are included whole; only the
 * Windows kernel calls and CreateEventW are replaced.
 *
 * The fake kernel driver below does to these two files what the real one does: it creates
 * allocations with a host backing and a GPU address, it accepts an indirect buffer, and it
 * runs the DMA_DATA packets of that buffer when its fence value retires, not before. A case
 * decides when a value retires: at once, later from another thread, never (a timeout), or
 * never with the fence mapping reading UINT64_MAX (a removal). It can also refuse a wait
 * registration, the event of a wait, a submission or an allocation.
 *
 * Two invariants hold in every case and are checked on every kernel call:
 *   - no Unlock2, FreeGpuVirtualAddress or DestroyAllocation2 of a range that an accepted
 *     and unretired DMA_DATA packet names. A freed GPU address can be handed out again at
 *     once (CONTRACT.md section 7, BD-110), so a late write would land in somebody else's
 *     memory;
 *   - no DMA_DATA packet runs against an address that no live allocation holds.
 *
 * This is the seam the review of 2026-10-11 asked for: delayed fences, a
 * timeout, a removal and a failed wait registration, through the code that runs on unit A.
 *
 * The fake also keeps the dispatch journal of the kernel driver per context
 * (driver/kmd/hip_journal.c). A context's journal turns on at its first accepted upload, and
 * from then on HipJournalBind binds each buffer of that context to an uploaded record with
 * the same buffer address, fence address and fence value. A buffer with no such record is
 * STATUS_INVALID_PARAMETER from DxgkDdiSubmitCommandVirtual, and the operating system answers
 * that by putting the whole device in an error state: the user-mode SubmitCommand has already
 * returned success, the fence mapping reads UINT64_MAX and every later call fails. That is
 * the shape of the first lab round of 2026-10-11 (vadd: a recorded launch, then the coherence
 * control's copy, then "the fence mapping reads UINT64_MAX" with no TDR and no fault), which
 * the fake before it could not produce, because it had no journal and no device error.
 * Two more invariants follow from one fence written by two contexts: a submission never
 * carries a value below an earlier one, and no context submits while another context's
 * submission is unretired, since the scheduler orders nothing across two contexts.
 */
#define D3DKMTCreateAllocation2 FakeCreateAllocation2
#define D3DKMTMapGpuVirtualAddress FakeMapGpuVirtualAddress
#define D3DKMTMakeResident FakeMakeResident
#define D3DKMTLock2 FakeLock2
#define D3DKMTUnlock2 FakeUnlock2
#define D3DKMTFreeGpuVirtualAddress FakeFreeGpuVirtualAddress
#define D3DKMTDestroyAllocation2 FakeDestroyAllocation2
#define D3DKMTSubmitCommand FakeSubmitCommand
#define D3DKMTEscape FakeEscape
#define D3DKMTWaitForSynchronizationObjectFromCpu FakeWaitFromCpu
#define D3DKMTGetDeviceState FakeGetDeviceState
#include "../../bc250hsa/kmt_device.h"
/* windows.h is in now, so this rename reaches only the two files below. */
static HANDLE WINAPI FakeCreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manual, BOOL initial,
                                      LPCWSTR name);
#define CreateEventW FakeCreateEventW
#include "../../bc250hsa/submit.c"
#include "../../bc250hsa/kmt_memory.c"
#undef CreateEventW
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

/* --------------------------------------------------------------------------------------
 * The fake kernel driver
 * ------------------------------------------------------------------------------------ */

#define FAKE_FENCE        31u
#define FAKE_PAGING_FENCE 32u
#define MAX_ALLOCS        64u
#define MAX_PENDING       64u
#define MAX_PACKETS       16u

typedef struct fake_alloc {
    D3DKMT_HANDLE handle;
    uint64_t      va;
    uint64_t      bytes;
    uint32_t      heap;
    uint8_t*      backing;
    int           live;
    int           va_live;
} fake_alloc;

typedef struct fake_packet {
    uint64_t dst;
    uint64_t src;
    uint64_t bytes;
    uint32_t pattern;
    int      fill;
} fake_packet;

typedef struct fake_submission {
    uint64_t      fence;
    uint64_t      at_ms;
    D3DKMT_HANDLE context;
    uint32_t      count;
    fake_packet   packets[MAX_PACKETS];
} fake_submission;

/* One context and its journal owner. The main context of dev_init is the first; the device
 * creates the others through bc250hsa_context_create. */
#define MAX_CONTEXTS 4u
typedef struct fake_context {
    D3DKMT_HANDLE handle;
    int           journal_on;     /* Owner->Enabled: set by the first accepted upload */
    uint64_t      upload_id;      /* the one UPLOADED slot of this context, 0 for none */
    uint64_t      upload_ib;
    uint64_t      upload_fence_va;
    uint64_t      upload_fence;
} fake_context;

static CRITICAL_SECTION         g_fake;
static struct bc250hsa_device*  g_dev;
static fake_alloc               g_allocs[MAX_ALLOCS];
static D3DKMT_HANDLE            g_next_handle;
static uint64_t                 g_next_va;
static fake_submission          g_pending[MAX_PENDING];
static uint32_t                 g_pending_count;
static volatile uint64_t        g_fence_mem;   /* what dev->fence_cpu points at */
static uint64_t                 g_retired;     /* the value the device really reached */
static uint64_t                 g_submitted;
static int                      g_removed;
static HANDLE                   g_waiter;      /* a duplicate of the registered wait's event */
static uint64_t                 g_waiter_value;
static fake_context             g_contexts[MAX_CONTEXTS];
static uint32_t                 g_context_count;
static uint64_t                 g_next_upload;
static int                      g_device_error;   /* the operating system's error state */
/* A refused buffer puts the device in its error state a moment after SubmitCommand returned,
 * not inside it: the lab round's wait found the old value, registered, woke, and then read
 * UINT64_MAX ("the fence mapping reads UINT64_MAX after the wait"). The fake applies the
 * error at the next kernel call that touches the device or the fence. */
static int                      g_error_pending;
static int32_t                  g_exec_state;     /* what D3DKMTGetDeviceState says */
static uint32_t                 g_state_calls;
/* Queries for another device or another state type. Counted, not checked per call: the number
 * of queries follows the pump's timing, and the check count of the test should not. */
static uint32_t                 g_state_wrong;

/* The knobs of a case. */
static int      g_hold;             /* 1: a submission waits for the test, or for the pump */
static uint32_t g_delay_ms;         /* the pump retires a submission this long after it */
static int      g_submit_fail;
static int      g_alloc_fail;
static int      g_corrupt_host;     /* a copy into GTT memory leaves the stale bytes */
static uint32_t g_register_fail_at; /* the n-th fence wait registration fails, 0 never */
static uint32_t g_event_fail_at;    /* the n-th CreateEventW fails, 0 never */
static int      g_context_fail;     /* bc250hsa_context_create fails */
static int      g_error_next;       /* the next submission puts the device in its error state */

/* What happened. */
static uint32_t g_register_calls, g_event_calls, g_submits, g_creates, g_unlocks, g_free_vas;
static uint32_t g_destroys, g_violations, g_faults, g_packets_run;
/* g_side_escapes: escapes that name a context other than the main one, which never uploads
 * anything else. g_bind_refusals: buffers HipJournalBind refused. g_order_violations: a
 * value below an earlier one, or a context that submitted while another's work was out. */
static uint32_t g_side_escapes, g_uploads, g_bind_refusals, g_order_violations;
static uint32_t g_context_creates, g_side_submits;

static fake_context* context_by_handle(D3DKMT_HANDLE handle)
{
    uint32_t i;
    for (i = 0; i < g_context_count; i++) {
        if (g_contexts[i].handle == handle) { return &g_contexts[i]; }
    }
    return NULL;
}

static fake_alloc* alloc_by_handle(D3DKMT_HANDLE handle)
{
    uint32_t i;
    for (i = 0; i < MAX_ALLOCS; i++) {
        if (g_allocs[i].live && g_allocs[i].handle == handle) { return &g_allocs[i]; }
    }
    return NULL;
}

static fake_alloc* alloc_by_va(uint64_t va, uint64_t bytes)
{
    uint32_t i;
    for (i = 0; i < MAX_ALLOCS; i++) {
        const fake_alloc* a = &g_allocs[i];
        if (a->live && a->va_live && va >= a->va && bytes <= a->bytes &&
            va - a->va <= a->bytes - bytes) {
            return &g_allocs[i];
        }
    }
    return NULL;
}

static int overlaps(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes)
{
    return a < b + b_bytes && b < a + a_bytes;
}

/* Does an accepted, unretired packet name any byte of this range? The caller holds g_fake. */
static int outstanding(uint64_t va, uint64_t bytes)
{
    uint32_t i, j;
    for (i = 0; i < g_pending_count; i++) {
        for (j = 0; j < g_pending[i].count; j++) {
            const fake_packet* p = &g_pending[i].packets[j];
            if (overlaps(va, bytes, p->dst, p->bytes) ||
                (!p->fill && overlaps(va, bytes, p->src, p->bytes))) {
                return 1;
            }
        }
    }
    return 0;
}

static void run_packet(const fake_packet* p)
{
    fake_alloc* dst = alloc_by_va(p->dst, p->bytes);
    uint64_t    i;

    if (dst == NULL) { ++g_faults; return; }
    if (p->fill) {
        for (i = 0; i < p->bytes; i++) {
            dst->backing[p->dst - dst->va + i] = (uint8_t)(p->pattern >> (8u * (uint32_t)(i & 3u)));
        }
    } else {
        fake_alloc* src = alloc_by_va(p->src, p->bytes);
        if (src == NULL) { ++g_faults; return; }
        /* A page table entry that does not snoop: the host processor keeps reading the
         * lines it had, so the bytes the engine wrote never show. */
        if (g_corrupt_host && dst->heap == BC250HSA_HEAP_GTT) { ++g_packets_run; return; }
        memmove(dst->backing + (p->dst - dst->va), src->backing + (p->src - src->va),
                (size_t)p->bytes);
    }
    ++g_packets_run;
}

/* The device reaches value: the packets of every submission up to it run, in order, and a
 * registered wait for a value now reached wakes. The caller holds g_fake. */
static void retire_locked(uint64_t value)
{
    if (value > g_submitted) { value = g_submitted; }
    while (g_pending_count != 0u && g_pending[0].fence <= value) {
        uint32_t j;
        for (j = 0; j < g_pending[0].count; j++) { run_packet(&g_pending[0].packets[j]); }
        memmove(&g_pending[0], &g_pending[1], (g_pending_count - 1u) * sizeof(g_pending[0]));
        g_pending_count--;
    }
    if (value > g_retired) { g_retired = value; }
    if (!g_removed) { g_fence_mem = g_retired; }
    if (g_waiter != NULL && !g_removed && g_waiter_value <= g_retired) {
        SetEvent(g_waiter);
        CloseHandle(g_waiter);
        g_waiter = NULL;
    }
}

static void fake_retire_all(void)
{
    EnterCriticalSection(&g_fake);
    retire_locked(UINT64_MAX);
    LeaveCriticalSection(&g_fake);
}

/* The pump: another thread that retires each submission g_delay_ms after it arrived, or
 * removes the device once, which is how a fence retires during a wait. */
static volatile LONG g_pump_stop;
static volatile LONG g_pump_remove_ms;
static DWORD WINAPI pump_main(LPVOID arg)
{
    const ULONGLONG start = GetTickCount64();
    (void)arg;
    while (InterlockedCompareExchange(&g_pump_stop, 0, 0) == 0) {
        const ULONGLONG now = GetTickCount64();
        EnterCriticalSection(&g_fake);
        if (g_pump_remove_ms != 0 && now - start >= (ULONGLONG)g_pump_remove_ms) {
            g_removed = 1;
            g_fence_mem = UINT64_MAX;
        }
        while (!g_removed && g_pending_count != 0u && now - g_pending[0].at_ms >= g_delay_ms) {
            retire_locked(g_pending[0].fence);
        }
        LeaveCriticalSection(&g_fake);
        Sleep(1);
    }
    return 0;
}

static HANDLE pump_start(uint32_t delay_ms, uint32_t remove_after_ms)
{
    HANDLE thread;
    g_delay_ms = delay_ms;
    InterlockedExchange(&g_pump_stop, 0);
    InterlockedExchange(&g_pump_remove_ms, (LONG)remove_after_ms);
    thread = CreateThread(NULL, 0, pump_main, NULL, 0, NULL);
    CHECK(thread != NULL);
    return thread;
}

static void pump_stop(HANDLE thread)
{
    InterlockedExchange(&g_pump_stop, 1);
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
}

NTSTATUS APIENTRY FakeCreateAllocation2(D3DKMT_CREATEALLOCATION* create)
{
    const struct bc250hsa_alloc_blob* blob =
        (const struct bc250hsa_alloc_blob*)create->pAllocationInfo2[0].pPrivateDriverData;
    uint32_t i;
    if (g_alloc_fail) { return STATUS_NO_MEMORY; }
    for (i = 0; i < MAX_ALLOCS; i++) {
        if (!g_allocs[i].live) { break; }
    }
    CHECK(i < MAX_ALLOCS);
    if (i == MAX_ALLOCS) { return STATUS_NO_MEMORY; }
    memset(&g_allocs[i], 0, sizeof(g_allocs[i]));
    g_allocs[i].backing = (uint8_t*)calloc(1, (size_t)blob->alloc_size);
    if (g_allocs[i].backing == NULL) { return STATUS_NO_MEMORY; }
    g_allocs[i].handle = ++g_next_handle;
    g_allocs[i].bytes = blob->alloc_size;
    g_allocs[i].heap = blob->preferred_heap;
    g_allocs[i].live = 1;
    create->pAllocationInfo2[0].hAllocation = g_allocs[i].handle;
    ++g_creates;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeMapGpuVirtualAddress(D3DDDI_MAPGPUVIRTUALADDRESS* map)
{
    fake_alloc* a = alloc_by_handle(map->hAllocation);
    CHECK(a != NULL);
    if (a == NULL) { return STATUS_INVALID_PARAMETER; }
    a->va = g_next_va;
    a->va_live = 1;
    g_next_va += 0x10000000ull;   /* 256 MiB apart: no test allocation is larger */
    map->VirtualAddress = a->va;
    map->PagingFenceValue = 0u;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeMakeResident(D3DDDI_MAKERESIDENT* resident)
{
    resident->PagingFenceValue = 0u;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeLock2(D3DKMT_LOCK2* lock)
{
    fake_alloc* a = alloc_by_handle(lock->hAllocation);
    if (a == NULL) { return STATUS_INVALID_PARAMETER; }
    lock->pData = a->backing;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeUnlock2(const D3DKMT_UNLOCK2* unlock)
{
    fake_alloc* a = alloc_by_handle(unlock->hAllocation);
    ++g_unlocks;
    EnterCriticalSection(&g_fake);
    if (a != NULL && outstanding(a->va, a->bytes)) { ++g_violations; }
    LeaveCriticalSection(&g_fake);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeFreeGpuVirtualAddress(const D3DKMT_FREEGPUVIRTUALADDRESS* free_va)
{
    fake_alloc* a = alloc_by_va(free_va->BaseAddress, 1u);
    ++g_free_vas;
    EnterCriticalSection(&g_fake);
    if (outstanding(free_va->BaseAddress, free_va->Size)) { ++g_violations; }
    LeaveCriticalSection(&g_fake);
    if (a != NULL) { a->va_live = 0; }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeDestroyAllocation2(const D3DKMT_DESTROYALLOCATION2* destroy)
{
    UINT i;
    for (i = 0; i < destroy->AllocationCount; i++) {
        fake_alloc* a = alloc_by_handle(destroy->phAllocationList[i]);
        ++g_destroys;
        if (a == NULL) { continue; }
        EnterCriticalSection(&g_fake);
        if (outstanding(a->va, a->bytes)) { ++g_violations; }
        LeaveCriticalSection(&g_fake);
        free(a->backing);
        memset(a, 0, sizeof(*a));
    }
    return STATUS_SUCCESS;
}

/* The operating system's answer to a buffer the kernel driver refused: the device is in an
 * error state, the fence mapping reads the removal sentinel and a registered wait wakes.
 * Which execution state the real device then reports is what the next lab round reads; the
 * fake says one that is not active. The caller holds g_fake. */
static void device_error_locked(void)
{
    g_error_pending = 0;
    g_device_error = 1;
    g_removed = 1;
    g_fence_mem = UINT64_MAX;
    g_exec_state = D3DKMT_DEVICEEXECUTION_ERROR_DMAFAULT;
    if (g_waiter != NULL) {
        SetEvent(g_waiter);
        CloseHandle(g_waiter);
        g_waiter = NULL;
    }
}

/* The journal escape of driver/kmd/hip_journal.c: an upload of at least one dispatch record
 * for the context it names, or the cancel of that upload. */
NTSTATUS APIENTRY FakeEscape(const D3DKMT_ESCAPE* escape)
{
    BC250_HIP_JOURNAL_UPLOAD* h = (BC250_HIP_JOURNAL_UPLOAD*)escape->pPrivateDriverData;
    fake_context*             ctx;
    NTSTATUS                  result = STATUS_SUCCESS;

    CHECK(escape->Type == D3DKMT_ESCAPE_DRIVERPRIVATE && h != NULL);
    CHECK(h->Magic == BC250_HIP_JOURNAL_MAGIC && h->Command == BC250_HIP_JOURNAL_COMMAND);
    EnterCriticalSection(&g_fake);
    if (g_error_pending) { device_error_locked(); }
    ctx = context_by_handle(escape->hContext);
    CHECK(ctx != NULL);
    if (escape->hContext != g_dev->context) { ++g_side_escapes; }
    if (ctx == NULL || g_device_error) {
        result = STATUS_DEVICE_REMOVED;
    } else if (h->Operation == BC250_HIP_JOURNAL_CANCEL_OP) {
        if (ctx->upload_id == h->UploadId) { ctx->upload_id = 0u; }
        h->Status = h->NtStatus = 0u;
    } else {
        CHECK(h->Operation == BC250_HIP_JOURNAL_UPLOAD_OP && h->RecordCount != 0u);
        ++g_uploads;
        ctx->journal_on = 1;
        ctx->upload_id = ++g_next_upload;
        ctx->upload_ib = h->IbVa;
        ctx->upload_fence_va = h->FenceVa;
        ctx->upload_fence = h->FenceValue;
        h->UploadId = ctx->upload_id;
        h->Status = h->NtStatus = 0u;
    }
    LeaveCriticalSection(&g_fake);
    return result;
}

NTSTATUS APIENTRY FakeSubmitCommand(const D3DKMT_SUBMITCOMMAND* command)
{
    const struct bc250hsa_submit_blob* blob =
        (const struct bc250hsa_submit_blob*)command->pPrivateDriverData;
    const uint32_t* dwords =
        (const uint32_t*)((const uint8_t*)g_dev->ring.host + (command->Commands - g_dev->ring.va));
    const uint32_t  count = command->CommandLength / 4u;
    fake_submission s;
    fake_context*   ctx;
    uint32_t        i = 0;

    ++g_submits;
    if (g_submit_fail) { return STATUS_INVALID_PARAMETER; }
    CHECK(command->BroadcastContextCount == 1u);
    EnterCriticalSection(&g_fake);
    if (g_error_pending) { device_error_locked(); }
    if (g_device_error) {
        /* "all subsequent calls on it will fail" (WDK, DXGKDDI_SUBMITCOMMANDVIRTUAL) */
        LeaveCriticalSection(&g_fake);
        return STATUS_DEVICE_REMOVED;
    }
    ctx = context_by_handle(command->BroadcastContext[0]);
    CHECK(ctx != NULL);
    if (ctx != NULL && command->BroadcastContext[0] != g_dev->context) { ++g_side_submits; }
    /* One fence, two contexts: the values only grow, and nothing of another context is out. */
    if (blob->fence_value <= g_submitted) { ++g_order_violations; }
    for (i = 0; i < g_pending_count; i++) {
        if (g_pending[i].context != command->BroadcastContext[0]) { ++g_order_violations; }
    }
    i = 0;
    /* HipJournalBind: a context whose journal is on accepts only a buffer that an uploaded
     * record names. Anything else is STATUS_INVALID_PARAMETER in the kernel driver, which
     * the caller of SubmitCommand never sees: the device is in its error state at once. */
    if (ctx == NULL || g_error_next ||
        (ctx->journal_on &&
         (ctx->upload_id == 0u || ctx->upload_ib != command->Commands ||
          ctx->upload_fence_va != blob->fence_va || ctx->upload_fence != blob->fence_value))) {
        if (!g_error_next) { ++g_bind_refusals; }
        g_error_next = 0;
        g_error_pending = 1;
        LeaveCriticalSection(&g_fake);
        return STATUS_SUCCESS;
    }
    if (ctx->journal_on) { ctx->upload_id = 0u; }   /* the slot is bound */
    LeaveCriticalSection(&g_fake);
    memset(&s, 0, sizeof(s));
    s.fence = blob->fence_value;
    s.at_ms = GetTickCount64();
    s.context = command->BroadcastContext[0];
    while (i < count) {
        const uint32_t header = dwords[i];
        if ((header >> 30) != 3u) { i++; continue; }
        if (((header >> 8) & 0xFFu) == BC250HSA_PKT3_DMA_DATA && i + 7u <= count) {
            fake_packet* p = &s.packets[s.count];
            CHECK(s.count < MAX_PACKETS);
            if (s.count < MAX_PACKETS) {
                p->fill = (dwords[i + 1] & BC250HSA_DMA_SRC_SEL(3u)) ==
                          BC250HSA_DMA_SRC_SEL(BC250HSA_DMA_SRC_DATA);
                p->pattern = dwords[i + 2];
                p->src = dwords[i + 2] | ((uint64_t)dwords[i + 3] << 32);
                p->dst = dwords[i + 4] | ((uint64_t)dwords[i + 5] << 32);
                p->bytes = dwords[i + 6] & BC250HSA_DMA_BYTE_COUNT(~0u);
                s.count++;
            }
        }
        i += ((header >> 16) & 0x3FFFu) + 2u;
    }
    EnterCriticalSection(&g_fake);
    CHECK(g_pending_count < MAX_PENDING);
    g_submitted = s.fence;
    g_pending[g_pending_count++] = s;
    if (!g_hold) { retire_locked(s.fence); }
    LeaveCriticalSection(&g_fake);
    return STATUS_SUCCESS;
}

/* The execution state query of submit.c (bc250hsa_device_execution_state and
 * bc250hsa_device_executing), which the fence checks print: the real call runs over this,
 * with the device's own handle and the execution type. */
NTSTATUS APIENTRY FakeGetDeviceState(D3DKMT_GETDEVICESTATE* state)
{
    EnterCriticalSection(&g_fake);
    ++g_state_calls;
    if (state == NULL || g_dev == NULL || state->hDevice != g_dev->device ||
        state->StateType != D3DKMT_DEVICESTATE_EXECUTION) {
        ++g_state_wrong;
        LeaveCriticalSection(&g_fake);
        return STATUS_INVALID_PARAMETER;
    }
    if (g_error_pending) { device_error_locked(); }
    state->ExecutionState = (D3DKMT_DEVICEEXECUTION_STATE)g_exec_state;
    LeaveCriticalSection(&g_fake);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY FakeWaitFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* wait)
{
    if (wait->ObjectHandleArray[0] == FAKE_PAGING_FENCE) { return STATUS_SUCCESS; }
    CHECK(wait->ObjectHandleArray[0] == FAKE_FENCE && wait->hAsyncEvent != NULL);
    ++g_register_calls;
    if (g_register_fail_at != 0u && g_register_calls == g_register_fail_at) {
        return STATUS_NO_MEMORY;
    }
    EnterCriticalSection(&g_fake);
    if (g_waiter != NULL) {
        CloseHandle(g_waiter);   /* the wait before this one gave up on its event */
        g_waiter = NULL;
    }
    if (g_error_pending) {
        /* The wait is registered, and the error state wakes it. */
        device_error_locked();
        SetEvent(wait->hAsyncEvent);
        LeaveCriticalSection(&g_fake);
        return STATUS_SUCCESS;
    }
    if (!g_removed && g_retired >= wait->FenceValueArray[0]) {
        SetEvent(wait->hAsyncEvent);
    } else {
        CHECK(DuplicateHandle(GetCurrentProcess(), wait->hAsyncEvent, GetCurrentProcess(),
                              &g_waiter, 0, FALSE, DUPLICATE_SAME_ACCESS));
        g_waiter_value = wait->FenceValueArray[0];
    }
    LeaveCriticalSection(&g_fake);
    return STATUS_SUCCESS;
}

static HANDLE WINAPI FakeCreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manual, BOOL initial,
                                      LPCWSTR name)
{
    ++g_event_calls;
    if (g_event_fail_at != 0u && g_event_calls == g_event_fail_at) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return CreateEventW(attributes, manual, initial, name);
}

/* What kmt_device.c would provide. */
bc250hsa_status bc250hsa_os_status(const char* what, NTSTATUS status)
{ (void)what; bc250hsa_set_os_status((int32_t)status); return BC250HSA_EOS; }
void bc250hsa_mark_lost(struct bc250hsa_device* d, const char* why)
{
    /* The line kmt_device.c logs, so that a case shows which read lost the device. */
    if (!d->device_lost) { printf("  [layer 1 marks the device lost] %s\n", why); }
    d->device_lost = 1;
}
/* This file also builds against three older trees as negative controls: d77886b1, the
 * candidate of the first lab round (case_lab_shape), and e91f8693 and 223b63cc, whose failed
 * copy wait let the next launch go out (case_unretired_copy_excludes). Two names tell the
 * trees apart. BC250HSA_COPY_LOCK_WAITS_SAID came with 223b63cc, which also moved the two
 * device-state calls from kmt_device.c into submit.c, over FakeGetDeviceState here.
 * BC250HSA_COPY_EXCLUSION came with the exclusion of an unretired copy. */
#ifndef BC250HSA_COPY_LOCK_WAITS_SAID
int bc250hsa_device_executing(struct bc250hsa_device* d) { return !d->device_lost; }
#ifdef BC250HSA_FENCE_CHECKS
int32_t bc250hsa_device_execution_state(struct bc250hsa_device* d)
{ (void)d; return g_exec_state; }
#endif
#endif
#ifdef BC250HSA_FENCE_CHECKS
/* A tree from before the copy context has no such call. */
bc250hsa_status bc250hsa_context_create(struct bc250hsa_device* d, D3DKMT_HANDLE* out)
{
    (void)d;
    *out = 0u;
    ++g_context_creates;
    if (g_context_fail) { return bc250hsa_os_status("CreateContextVirtual", STATUS_NO_MEMORY); }
    EnterCriticalSection(&g_fake);
    CHECK(g_context_count < MAX_CONTEXTS);
    if (g_context_count < MAX_CONTEXTS) {
        memset(&g_contexts[g_context_count], 0, sizeof(g_contexts[0]));
        g_contexts[g_context_count].handle = 0x700u + g_context_count;
        *out = g_contexts[g_context_count].handle;
        g_context_count++;
    }
    LeaveCriticalSection(&g_fake);
    return (*out != 0u) ? BC250HSA_OK : BC250HSA_EOS;
}
#endif

/* --------------------------------------------------------------------------------------
 * Helpers of the cases
 * ------------------------------------------------------------------------------------ */

static void dev_init(struct bc250hsa_device* d)
{
    memset(d, 0, sizeof(*d));
    InitializeCriticalSection(&d->lock);
    d->adapter = 11; d->device = 12; d->context = 13; d->paging_queue = 14;
    d->paging_fence = FAKE_PAGING_FENCE; d->fence = FAKE_FENCE;
    d->submit_capable = 1; d->lds_bytes_per_workgroup = 65536;
    d->ring_slots = 8; d->ring_slot_bytes = 65536;
    d->ring.host = calloc(d->ring_slots, d->ring_slot_bytes); d->ring.va = 0x4000000000ull;
    d->slot_fence = (uint64_t*)calloc(d->ring_slots, sizeof(uint64_t));
    d->last_ib_capacity = d->ring_slot_bytes / 4;
    d->last_ib = (uint32_t*)calloc(d->last_ib_capacity, 4);
    d->fence_cpu = &g_fence_mem; d->fence_gpu_va = 0x4100000000ull;
    d->va_window_start = 0x1000000000ull; d->va_window_end = 0x3000000000ull;
    d->wait_slice_ms = 10u; d->wait_total_ms = 60u;
    g_dev = d;
    g_next_va = 0x1000000000ull;   /* a DMA_DATA address has 40 bits */
    g_pending_count = 0u;
    g_fence_mem = 0u; g_retired = 0u; g_submitted = 0u; g_removed = 0;
    g_hold = 0; g_delay_ms = 0u; g_submit_fail = 0; g_alloc_fail = 0; g_corrupt_host = 0;
    g_register_fail_at = 0u; g_event_fail_at = 0u;
    g_context_fail = 0; g_error_next = 0; g_device_error = 0; g_error_pending = 0;
    g_exec_state = D3DKMT_DEVICEEXECUTION_ACTIVE;
    memset(g_contexts, 0, sizeof(g_contexts));
    g_contexts[0].handle = d->context;
    g_context_count = 1u;
#ifdef BC250HSA_FENCE_CHECKS
    /* What device open sets (kmt_device.c). */
    d->check_dispatch_left = 2u; d->check_copy_left = 2u;
    d->fence_type = D3DDDI_MONITORED_FENCE;
#endif
}

/* quarantine: the device was lost, so nothing may be freed; the test lets go of the fake
 * backing directly, as process exit would. */
static void dev_finish(struct bc250hsa_device* d, int quarantine)
{
    uint32_t i;
    if (!quarantine) {
        fake_retire_all();
        bc250hsa_free_all_allocations(d);
    }
    CHECK(g_violations == 0u);
    CHECK(g_faults == 0u);
    CHECK(g_side_escapes == 0u);       /* a copy-only buffer carries no dispatch record */
    CHECK(g_bind_refusals == 0u);      /* no buffer the kernel driver's journal refuses */
    CHECK(g_order_violations == 0u);
    CHECK(g_state_wrong == 0u);        /* every state query names this device and EXECUTION */
    g_state_wrong = 0u;
    for (i = 0; i < MAX_ALLOCS; i++) {
        if (g_allocs[i].live) { free(g_allocs[i].backing); }
        memset(&g_allocs[i], 0, sizeof(g_allocs[i]));
    }
    if (g_waiter != NULL) { CloseHandle(g_waiter); g_waiter = NULL; }
    free(d->ring.host); free(d->slot_fence); free(d->last_ib); free(d->journal_upload);
    DeleteCriticalSection(&d->lock);
    g_violations = g_faults = 0u;
    g_side_escapes = g_bind_refusals = g_order_violations = 0u;
}

static bc250hsa_op_receipt receipt_new(void)
{
    bc250hsa_op_receipt r;
    r.struct_bytes = (uint32_t)sizeof(r);
    r.state = 99u;          /* a value no return leaves behind */
    r.fence_value = 77u;
    return r;
}

static int alloc_mem(struct bc250hsa_device* d, uint64_t bytes, uint32_t flags, bc250hsa_mem* m)
{
    memset(m, 0, sizeof(*m));
    return bc250hsa_alloc(d, bytes, 0u, flags, m) == BC250HSA_OK;
}

/* A device allocation with a host mapping, filled with a pattern that differs at every
 * position, so that a byte from the wrong offset shows. */
static int source_mem(struct bc250hsa_device* d, uint64_t bytes, bc250hsa_mem* m)
{
    void*    host = NULL;
    uint64_t i;
    if (!alloc_mem(d, bytes, BC250HSA_MEM_MAPPABLE, m)) { return 0; }
    if (bc250hsa_map(d, m, &host) != BC250HSA_OK) { return 0; }
    for (i = 0; i < bytes; i++) { ((uint8_t*)host)[i] = (uint8_t)((i * 131u + (i >> 9)) ^ 0x5Au); }
    return 1;
}

static int counters(bc250hsa_counters* c)
{
    memset(c, 0, sizeof(*c));
    c->struct_bytes = (uint32_t)sizeof(*c);
    return bc250hsa_counters_read(c) == BC250HSA_OK;
}

/* A device whose control passed, with every submission retiring at once. */
static void dev_ready(struct bc250hsa_device* d)
{
    uint32_t ready = 0u;
    dev_init(d);
    CHECK(bc250hsa_gpu_copy_ready(d, &ready) == BC250HSA_OK && ready == 1u);
}

/* The library's log. Every line is kept, so that a case can check what the fence checks
 * said, and the warnings and errors are printed, so that a failing case shows them. */
#define LOG_LINES 256u
static char     g_log[LOG_LINES][512];
static uint32_t g_log_count;

static void print_log(void* ctx, uint32_t level, const char* message)
{
    (void)ctx;
    if (g_log_count < LOG_LINES) {
        strncpy(g_log[g_log_count], message, sizeof(g_log[0]) - 1u);
        g_log[g_log_count][sizeof(g_log[0]) - 1u] = '\0';
        g_log_count++;
    }
    if (level <= BC250HSA_LOG_WARN) {
        printf("  [layer 1, level %u] %s\n", level, message);
    }
}

static void log_clear(void) { g_log_count = 0u; }

/* Did one line say both a and b? */
static int log_has(const char* a, const char* b)
{
    uint32_t i;
    for (i = 0; i < g_log_count; i++) {
        if (strstr(g_log[i], a) != NULL && (b == NULL || strstr(g_log[i], b) != NULL)) {
            return 1;
        }
    }
    printf("  no log line says \"%s\" with \"%s\"\n", a, (b != NULL) ? b : "");
    return 0;
}

/* A recorded launch of vadd, the kernel of the lab round, with the record layer 2 sends
 * (tests/host/test_submit_journal.c builds the same one). */
static bc250hsa_kernel   g_kernel;
static bc250hsa_dispatch g_launch;
static unsigned char     g_record[BC250_HIP_JOURNAL_MAX_RECORD_BYTES];
static uint32_t          g_record_bytes;
static uint64_t          g_dispatch_id;

static void launch_init(void)
{
    BC250_HIP_DISPATCH_RECORD r;

    memset(&g_kernel, 0, sizeof(g_kernel));
    memset(&g_launch, 0, sizeof(g_launch));
    g_kernel.name = "vadd"; g_kernel.descriptor_va = 0x140ABCD0C80ull;
    g_kernel.entry_va = 0x140ABCD1E00ull; g_kernel.kernarg_bytes = 28; g_kernel.kernarg_align = 16;
    g_kernel.max_flat_workgroup_size = 1024; g_kernel.sgpr_count = 9; g_kernel.vgpr_count = 8;
    g_kernel.wave_size = 32; g_kernel.workgroup_processor_mode = 1; g_kernel.user_sgpr_count = 6;
    g_kernel.compute_pgm_rsrc1 = 0xE0AF0000u; g_kernel.compute_pgm_rsrc2 = 0x8Cu;
    g_kernel.kernel_code_properties = 0x0409;
    g_launch.struct_bytes = sizeof(g_launch); g_launch.kernel = &g_kernel;
    g_launch.kernarg_va = 0x140ABCD2000ull;
    g_launch.launch.struct_bytes = sizeof(g_launch.launch); g_launch.launch.grid[0] = 4096;
    g_launch.launch.grid[1] = g_launch.launch.grid[2] = 1;
    g_launch.launch.block[0] = 256; g_launch.launch.block[1] = g_launch.launch.block[2] = 1;
    memset(&r, 0, sizeof(r));
    r.SymbolOffset = sizeof(r); r.SymbolBytes = 5;
    r.KernargOffset = 104; r.KernargBytes = 28;
    r.BindingsOffset = (r.KernargOffset + 28u + 7u) & ~7u;
    r.Bytes = r.BindingsOffset;
    r.EntryVa = g_kernel.entry_va; r.DescriptorVa = g_kernel.descriptor_va;
    r.KernargVa = g_launch.kernarg_va; r.DispatchId = 1;
    memcpy(r.Grid, g_launch.launch.grid, sizeof(r.Grid));
    memcpy(r.Block, g_launch.launch.block, sizeof(r.Block));
    memset(g_record, 0, r.Bytes);
    memcpy(g_record, &r, sizeof(r));
    memcpy(g_record + r.SymbolOffset, "vadd", 5);
    g_record_bytes = r.Bytes;
}

static bc250hsa_status send_launch(struct bc250hsa_device* d, uint64_t* fence)
{
    const uint64_t id = ++g_dispatch_id;
    memcpy(g_record + offsetof(BC250_HIP_DISPATCH_RECORD, DispatchId), &id, sizeof(id));
    return bc250hsa_dispatch_submit_recorded(d, &g_launch, g_record, g_record_bytes, fence);
}

static void batching_on(struct bc250hsa_device* d)
{
    d->batch.struct_bytes = (uint32_t)sizeof(d->batch);
    d->batch.enabled = 1u; d->batch.max_dispatches = 100u; d->batch.max_hold_us = UINT32_MAX;
}

/* --------------------------------------------------------------------------------------
 * The cases
 * ------------------------------------------------------------------------------------ */

static void case_everything_retires(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, host_dst, local;
    bc250hsa_op_receipt    r;
    bc250hsa_counters      c0, c1;
    const uint64_t         n = 6u * 1024u * 1024u + 100u;
    const uint64_t         staged = 5u * 1024u * 1024u + 3u;   /* six rounds of a four-slot ring */
    uint8_t*               page = (uint8_t*)malloc((size_t)staged + 64u);
    uint32_t               ready = 0u, creates0, destroys0;
    uint64_t               i;

    printf("copy lifecycle: everything retires at once\n");
    dev_init(&d);
    creates0 = g_creates; destroys0 = g_destroys;
    CHECK(bc250hsa_gpu_copy_ready(&d, &ready) == BC250HSA_OK && ready == 1u);
    CHECK(d.gpu_copy_control == BC250HSA_CONTROL_PASSED && d.control_hold_fence == 0u);
    CHECK(g_creates - creates0 == 2u && g_destroys - destroys0 == 2u);  /* the mapped control */
    CHECK(counters(&c0));

    CHECK(source_mem(&d, n, &src));
    CHECK(alloc_mem(&d, n, BC250HSA_MEM_HOST, &host_dst) && host_dst.host != NULL);
    memset(host_dst.host, 0xEE, (size_t)n);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, n,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == BC250HSA_OK);
    CHECK(r.state == BC250HSA_OP_RETIRED && r.fence_value == g_submitted);
    CHECK(memcmp(host_dst.host, src.host, (size_t)n) == 0);

    /* Staged, from an unaligned source offset into an unaligned destination, with guard
     * bytes on both sides that must keep their value. */
    memset(page, 0xC3, (size_t)staged + 64u);
    r = receipt_new();
    CHECK(bc250hsa_copy_from_device_gpu(&d, page + 32, &src, 7u, staged, &r) == BC250HSA_OK);
    CHECK(r.state == BC250HSA_OP_RETIRED);
    CHECK(memcmp(page + 32, (const uint8_t*)src.host + 7, (size_t)staged) == 0);
    for (i = 0; i < 32u; i++) { CHECK(page[i] == 0xC3 && page[32u + staged + i] == 0xC3); }

    /* A copy into the GPU address of a host allocation, at an interior offset. */
    memset(host_dst.host, 0xEE, (size_t)n);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 4096u + 5u, &src, 13u, 100000u, 0u, &r) ==
          BC250HSA_OK && r.state == BC250HSA_OP_RETIRED);
    CHECK(memcmp((uint8_t*)host_dst.host + 4096 + 5, (const uint8_t*)src.host + 13, 100000) == 0);
    CHECK(((uint8_t*)host_dst.host)[4096 + 4] == 0xEE && ((uint8_t*)host_dst.host)[4096 + 5 + 100000] == 0xEE);

    /* A fill whose ends are not on a dword boundary. */
    r = receipt_new();
    CHECK(bc250hsa_fill_device(&d, &host_dst, 3u, 0xAB, 3u * 4096u + 2u, &r) == BC250HSA_OK);
    CHECK(r.state == BC250HSA_OP_RETIRED);
    {
        const uint8_t* h = (const uint8_t*)host_dst.host;
        uint64_t       wrong = 0u;
        for (i = 3u; i < 3u + 3u * 4096u + 2u; i++) { wrong += (h[i] != 0xAB) ? 1u : 0u; }
        CHECK(wrong == 0u);
        /* The byte after the range still holds what the copy above wrote there. */
        CHECK(h[2] == 0xEE && h[12293] == ((const uint8_t*)src.host)[13u + 12293u - 4101u]);
    }

    /* Memory no host processor can reach needs no answer from the control. */
    CHECK(alloc_mem(&d, 65536u, 0u, &local));
    r = receipt_new();
    CHECK(bc250hsa_fill_device(&d, &local, 0u, 0x11, 65536u, &r) == BC250HSA_OK);
    CHECK(counters(&c1));
    CHECK(c1.gpu_copy_d2h - c0.gpu_copy_d2h == 1u && c1.gpu_copy_d2h_staged - c0.gpu_copy_d2h_staged == 1u);
    CHECK(c1.gpu_copy_d2d - c0.gpu_copy_d2d == 1u && c1.gpu_fill - c0.gpu_fill == 2u);

    /* NULL takes a receipt of the library's own, and a size it does not know is refused with
     * the caller's receipt untouched. */
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 4096u, 0u, NULL) == BC250HSA_OK);
    r = receipt_new();
    r.struct_bytes = 8u;
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 4096u, 0u, &r) == BC250HSA_EINVAL);
    CHECK(r.state == 99u && r.fence_value == 77u);
    /* An overlap is refused before anything is submitted, on a device that still works. */
    r = receipt_new();
    {
        const uint32_t submits0 = g_submits;
        CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 16u, &host_dst, 0u, 4096u, 0u, &r) == BC250HSA_EINVAL);
        CHECK(r.state == BC250HSA_OP_NOT_SUBMITTED && r.fence_value == 0u && g_submits == submits0);
    }
    CHECK(!d.device_lost);
    free(page);
    dev_finish(&d, 0);
}

/* R1: the control's wait waits for a fence that retires later, from another thread. */
static void case_control_delayed(void)
{
    struct bc250hsa_device d;
    HANDLE                 pump;
    uint32_t               ready = 0u;
    bc250hsa_counters      c0, c1;

    printf("copy lifecycle: the control waits for a delayed fence\n");
    dev_init(&d);
    d.wait_total_ms = 5000u;
    g_hold = 1;
    pump = pump_start(30u, 0u);
    CHECK(counters(&c0));
    CHECK(bc250hsa_gpu_copy_ready(&d, &ready) == BC250HSA_OK && ready == 1u);
    CHECK(counters(&c1) && c1.waits_other_ticks > c0.waits_other_ticks);
    CHECK(d.control_hold_fence == 0u && g_violations == 0u);
    pump_stop(pump);
    dev_finish(&d, 0);
}

/* R1: a wait of the control that cannot establish retirement keeps the buffers. kind 0 is a
 * timeout, 1 a failed wait registration, 2 a failed CreateEventW, 3 a failed allocation. */
static void case_control_inconclusive(int kind)
{
    static const char* const names[4] = { "a timeout", "a failed wait registration",
                                           "a failed CreateEventW", "a failed allocation" };
    struct bc250hsa_device d;
    bc250hsa_mem           src;
    bc250hsa_op_receipt    r;
    bc250hsa_counters      c0, c1;
    uint8_t                page[8192];
    uint32_t               ready = 7u, creates0, destroys0, unlocks0, free_vas0;
    bc250hsa_status        status;

    printf("copy lifecycle: the control after %s\n", names[kind]);
    dev_init(&d);
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(counters(&c0));
    creates0 = g_creates; destroys0 = g_destroys; unlocks0 = g_unlocks; free_vas0 = g_free_vas;
    g_hold = (kind != 3) ? 1 : 0;
    if (kind == 1) { g_register_fail_at = g_register_calls + 1u; }
    if (kind == 2) { g_event_fail_at = g_event_calls + 1u; }
    if (kind == 3) { g_alloc_fail = 1; }
    status = bc250hsa_gpu_copy_ready(&d, &ready);
    CHECK(status == ((kind == 0) ? BC250HSA_ETIMEOUT : BC250HSA_EOS) && ready == 0u);
    CHECK(d.gpu_copy_control == BC250HSA_CONTROL_UNTRIED && !d.device_lost);
    CHECK(counters(&c1) && c1.gpu_copy_control_failed == c0.gpu_copy_control_failed);
    if (kind != 3) {
        /* The submission is out and did not retire: nothing of the control is freed. */
        CHECK(d.control_hold_fence != 0u && d.control_hold_fence == g_submitted);
        CHECK(g_creates - creates0 == 2u);
        CHECK(g_destroys == destroys0 && g_unlocks == unlocks0 && g_free_vas == free_vas0);
        /* A copy now runs no second control while the first is held: it waits for the held
         * fence under the bound, which still does not retire, and answers before it submits. */
        g_register_fail_at = 0u; g_event_fail_at = 0u;
        r = receipt_new();
        status = bc250hsa_copy_from_device_gpu(&d, page, &src, 0u, sizeof(page), &r);
        CHECK(status == BC250HSA_ETIMEOUT && r.state == BC250HSA_OP_NOT_SUBMITTED);
        CHECK(g_creates - creates0 == 2u && g_destroys == destroys0);
        /* A genuine retirement later: the next call frees the held buffers first and then
         * asks the question again. */
        fake_retire_all();
    } else {
        CHECK(d.control_hold_fence == 0u);
    }
    g_hold = 0; g_alloc_fail = 0;
    CHECK(bc250hsa_gpu_copy_ready(&d, &ready) == BC250HSA_OK && ready == 1u);
    CHECK(d.control_hold_fence == 0u && d.gpu_copy_control == BC250HSA_CONTROL_PASSED);
    if (kind != 3) { CHECK(g_destroys - destroys0 == 4u); }
    CHECK(g_violations == 0u);
    dev_finish(&d, 0);
}

/* R1: the device is removed while the control waits. */
static void case_control_removed(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, host_dst;
    bc250hsa_op_receipt    r;
    HANDLE                 pump;
    uint32_t               ready = 7u, destroys0;

    printf("copy lifecycle: removal during the control\n");
    dev_init(&d);
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, BC250HSA_MEM_HOST, &host_dst));
    destroys0 = g_destroys;
    d.wait_total_ms = 5000u;
    g_hold = 1;
    pump = pump_start(100000u, 30u);
    CHECK(bc250hsa_gpu_copy_ready(&d, &ready) == BC250HSA_EDEVICELOST && ready == 0u);
    pump_stop(pump);
    CHECK(d.device_lost && d.control_hold_fence != 0u && g_destroys == destroys0);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 8192u,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == BC250HSA_EDEVICELOST);
    CHECK(r.state == BC250HSA_OP_NOT_SUBMITTED);
    dev_finish(&d, 1);
}

/* R2: the device accepted the copy, and the wait for it cannot establish retirement. kind 0
 * is a timeout, 1 a failed wait registration, 2 a failed CreateEventW, 3 a removal. The
 * receipt must say SUBMITTED with the value, and the late write must still land. */
static void case_direct_unretired(int kind)
{
    static const char* const names[4] = { "a timeout", "a failed wait registration",
                                           "a failed CreateEventW", "a removal" };
    static const bc250hsa_status want[4] = { BC250HSA_ETIMEOUT, BC250HSA_EOS, BC250HSA_EOS,
                                             BC250HSA_EDEVICELOST };
    struct bc250hsa_device d;
    bc250hsa_mem           src, host_dst;
    bc250hsa_op_receipt    r;
    bc250hsa_counters      c0, c1;
    HANDLE                 pump = NULL;

    printf("copy lifecycle: a direct copy after %s\n", names[kind]);
    dev_ready(&d);
    CHECK(source_mem(&d, 1u << 20, &src));
    CHECK(alloc_mem(&d, 1u << 20, BC250HSA_MEM_HOST, &host_dst));
    memset(host_dst.host, 0xEE, 1u << 20);
    CHECK(counters(&c0));
    g_hold = 1;
    if (kind == 1) { g_register_fail_at = g_register_calls + 1u; }
    if (kind == 2) { g_event_fail_at = g_event_calls + 1u; }
    if (kind == 3) { d.wait_total_ms = 5000u; pump = pump_start(100000u, 30u); }
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 1u << 20,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == want[kind]);
    if (pump != NULL) { pump_stop(pump); }
    CHECK(r.state == BC250HSA_OP_SUBMITTED && r.fence_value == g_submitted && r.fence_value != 0u);
    CHECK(((uint8_t*)host_dst.host)[0] == 0xEE);          /* nothing arrived yet */
    CHECK(counters(&c1) && c1.gpu_copy_d2h == c0.gpu_copy_d2h && c1.gpu_copy_d2h_ticks == c0.gpu_copy_d2h_ticks);
    CHECK(d.device_lost == (kind == 3));
    if (kind != 3) {
        /* The work was accepted, so it runs: a host-processor copy done now would have been
         * overwritten by this late write, or have overwritten it. */
        fake_retire_all();
        CHECK(memcmp(host_dst.host, src.host, 1u << 20) == 0);
    }
    dev_finish(&d, kind == 3);
}

/* R2: the third round of a staged copy fails its wait. The rounds before it retired and
 * wrote the caller's buffer; the round that failed keeps its slot until its value retires. */
static void case_staged_round_fails(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src;
    bc250hsa_op_receipt    r;
    HANDLE                 pump;
    const uint64_t         n = 5u * 1024u * 1024u;
    uint8_t*               page = (uint8_t*)malloc((size_t)n);
    uint32_t               slot, held = 0u;

    printf("copy lifecycle: a staged round that fails its wait\n");
    dev_ready(&d);
    CHECK(source_mem(&d, n, &src));
    memset(page, 0xEE, (size_t)n);
    d.wait_total_ms = 5000u;
    g_hold = 1;
    pump = pump_start(5u, 0u);
    /* The staging ring is allocated by the first round; every round waits once. */
    g_register_fail_at = g_register_calls + 3u;
    r = receipt_new();
    CHECK(bc250hsa_copy_from_device_gpu(&d, page, &src, 0u, n, &r) == BC250HSA_EOS);
    CHECK(r.state == BC250HSA_OP_SUBMITTED && r.fence_value != 0u);
    CHECK(memcmp(page, src.host, 2u * 1024u * 1024u) == 0);
    CHECK(page[2u * 1024u * 1024u] == 0xEE);
    for (slot = 0; slot < BC250HSA_STAGE_SLOTS; slot++) {
        held += (d.stage_fence[slot] == r.fence_value) ? 1u : 0u;
    }
    CHECK(held == 1u);
    /* The pump retires the round later, and the next copy reuses the ring safely. */
    g_register_fail_at = 0u;
    r = receipt_new();
    CHECK(bc250hsa_copy_from_device_gpu(&d, page, &src, 0u, n, &r) == BC250HSA_OK);
    CHECK(r.state == BC250HSA_OP_RETIRED && memcmp(page, src.host, (size_t)n) == 0);
    pump_stop(pump);
    free(page);
    dev_finish(&d, 0);
}

/* R2: a fill with edge bytes whose wait times out keeps its staging slot. */
static void case_fill_unretired(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           host_dst;
    bc250hsa_op_receipt    r;
    uint32_t               slot, held = 0u;

    printf("copy lifecycle: a fill whose wait times out\n");
    dev_ready(&d);
    CHECK(alloc_mem(&d, 65536u, BC250HSA_MEM_HOST, &host_dst));
    g_hold = 1;
    r = receipt_new();
    CHECK(bc250hsa_fill_device(&d, &host_dst, 1u, 0x42, 1000u, &r) == BC250HSA_ETIMEOUT);
    CHECK(r.state == BC250HSA_OP_SUBMITTED && r.fence_value == g_submitted);
    for (slot = 0; slot < BC250HSA_STAGE_SLOTS; slot++) {
        held += (d.stage_fence[slot] == r.fence_value) ? 1u : 0u;
    }
    CHECK(held == 1u);
    fake_retire_all();
    g_hold = 0;
    r = receipt_new();
    CHECK(bc250hsa_fill_device(&d, &host_dst, 1u, 0x43, 1000u, &r) == BC250HSA_OK);
    CHECK(((uint8_t*)host_dst.host)[1] == 0x43 && ((uint8_t*)host_dst.host)[1000] == 0x43);
    CHECK(((uint8_t*)host_dst.host)[0] == 0u && ((uint8_t*)host_dst.host)[1001] == 0u);
    dev_finish(&d, 0);
}

/* R2: a submission that fails marks the device lost, and every call says so by name. */
static void case_submit_fails(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, host_dst;
    bc250hsa_op_receipt    r;
    uint8_t                page[8192];

    printf("copy lifecycle: a submission that fails\n");
    dev_ready(&d);
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, BC250HSA_MEM_HOST, &host_dst));
    g_submit_fail = 1;
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 8192u,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == BC250HSA_EDEVICELOST);
    CHECK(r.state == BC250HSA_OP_NOT_SUBMITTED && d.device_lost);
    r = receipt_new();
    CHECK(bc250hsa_copy_from_device_gpu(&d, page, &src, 0u, sizeof(page), &r) == BC250HSA_EDEVICELOST);
    CHECK(r.state == BC250HSA_OP_NOT_SUBMITTED);
    r = receipt_new();
    CHECK(bc250hsa_fill_device(&d, &host_dst, 0u, 1u, 4096u, &r) == BC250HSA_EDEVICELOST);
    CHECK(r.state == BC250HSA_OP_NOT_SUBMITTED);
    dev_finish(&d, 1);
}

/* R4: with a control that failed, nothing the host processor can read is written by the
 * engine, whatever the caller calls the copy, and no packet is built for it. */
static void case_alias_gate(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, host_dst, mapped, local;
    bc250hsa_op_receipt    r;
    bc250hsa_counters      c0, c1;
    uint8_t                page[8192];
    uint32_t               ready = 7u, submits0, destroys0;

    printf("copy lifecycle: the coherence gate follows the destination\n");
    dev_init(&d);
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, BC250HSA_MEM_HOST, &host_dst));
    CHECK(source_mem(&d, 65536u, &mapped));
    CHECK(alloc_mem(&d, 65536u, 0u, &local) && local.host == NULL);
    memset(host_dst.host, 0xEE, 65536u);
    CHECK(counters(&c0));
    destroys0 = g_destroys;
    g_corrupt_host = 1;
    CHECK(bc250hsa_gpu_copy_ready(&d, &ready) == BC250HSA_OK && ready == 0u);
    CHECK(d.gpu_copy_control == BC250HSA_CONTROL_FAILED && d.control_hold_fence == 0u);
    CHECK(g_destroys - destroys0 == 2u);
    CHECK(counters(&c1) && c1.gpu_copy_control_failed - c0.gpu_copy_control_failed == 1u);
    g_corrupt_host = 0;
    submits0 = g_submits;

    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 4096u + 5u, &src, 0u, 1000u, 0u, &r) ==
          BC250HSA_EUNSUPPORTED && r.state == BC250HSA_OP_NOT_SUBMITTED);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &mapped, 0u, &src, 0u, 1000u, 0u, &r) ==
          BC250HSA_EUNSUPPORTED && r.state == BC250HSA_OP_NOT_SUBMITTED);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &local, 0u, &src, 0u, 1000u,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == BC250HSA_EUNSUPPORTED);
    r = receipt_new();
    CHECK(bc250hsa_fill_device(&d, &host_dst, 0u, 1u, 4096u, &r) == BC250HSA_EUNSUPPORTED);
    r = receipt_new();
    CHECK(bc250hsa_copy_from_device_gpu(&d, page, &src, 0u, sizeof(page), &r) == BC250HSA_EUNSUPPORTED);
    CHECK(r.state == BC250HSA_OP_NOT_SUBMITTED);
    CHECK(g_submits == submits0);
    CHECK(((uint8_t*)host_dst.host)[4096 + 5] == 0xEE);
    /* Memory no host processor can reach is not gated. */
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &local, 0u, &src, 0u, 1000u, 0u, &r) == BC250HSA_OK);
    CHECK(r.state == BC250HSA_OP_RETIRED && g_submits == submits0 + 1u);
    CHECK(!d.device_lost);
    dev_finish(&d, 0);
}

/* The first lab round of 2026-10-11 (scratch/hip-gpu-copy/results-2026-10-11, arm01-vadd):
 * a recorded launch turns the main context's journal on, hipMemcpy waits for the device, and
 * the first copy whose destination the host processor reads runs the coherence control. Build
 * d77886b1 put the control's copy, a buffer with no dispatch record, on the main context; the
 * kernel driver refused it, the device went into its error state, and the control's wait
 * read UINT64_MAX with no TDR and no fault. Twice round, so that a launch after the copies
 * is bound again, with batching off and on. */
static void case_lab_shape(int batch)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, host_dst;
    bc250hsa_op_receipt    r;
    uint8_t                page[8192];
    uint64_t               fence = 0u;
    uint32_t               round, uploads0 = g_uploads, creates0 = g_context_creates;

    printf("copy lifecycle: a recorded launch, then the control and device-to-host copies"
           " (batching %s)\n", batch ? "on" : "off");
    dev_init(&d);
    if (batch) { batching_on(&d); }
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, BC250HSA_MEM_HOST, &host_dst));
    log_clear();
    for (round = 0; round < 2u; round++) {
        CHECK(send_launch(&d, &fence) == BC250HSA_OK && fence != 0u);
        /* hipMemcpy waits for the device before it copies (hip_memory.cpp, wait_for_device). */
        CHECK(bc250hsa_wait(&d, fence, 0u, 0u) == BC250HSA_OK);
        memset(host_dst.host, 0xEE, 8192u);
        r = receipt_new();
        CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 8192u,
                                             BC250HSA_COPY_HOST_READS_DST, &r) == BC250HSA_OK);
        CHECK(r.state == BC250HSA_OP_RETIRED && memcmp(host_dst.host, src.host, 8192u) == 0);
        r = receipt_new();
        CHECK(bc250hsa_copy_from_device_gpu(&d, page, &src, 0u, sizeof(page), &r) == BC250HSA_OK);
        CHECK(r.state == BC250HSA_OP_RETIRED && memcmp(page, src.host, sizeof(page)) == 0);
        r = receipt_new();
        CHECK(bc250hsa_fill_device(&d, &host_dst, 1u, 0x42, 1000u, &r) == BC250HSA_OK);
        CHECK(((uint8_t*)host_dst.host)[1] == 0x42 && ((uint8_t*)host_dst.host)[1000] == 0x42);
    }
    CHECK(!d.device_lost && g_fence_mem != UINT64_MAX && !g_device_error);
    CHECK(d.gpu_copy_control == BC250HSA_CONTROL_PASSED);
    CHECK(g_uploads - uploads0 == 2u);
#ifdef BC250HSA_FENCE_CHECKS
    CHECK(d.copy_context != 0u && d.copy_context != d.context);
    CHECK(g_context_creates - creates0 == 1u);
    /* The fence checks say what the lab round could not: the object, the mapping, the
     * context, the journal, and the values before the submission and after the wait. */
    CHECK(log_has("fence check: control: submit on the copy context",
                  "journal on, 0 dispatch records, upload 0); fence handle 0x1f type"));
    CHECK(log_has("fence check: control: the wait for", "ended ok; fence handle 0x1f cpu va"));
    CHECK(log_has("fence check: dispatch: submit on the main context",
                  "journal on, 1 dispatch records, upload"));
    CHECK(log_has("fence check: dispatch: the wait for", "value after the wait"));
    CHECK(log_has("fence check: copy: submit on the copy context", "value before submit"));
    CHECK(log_has("fence check: copy context 0x701 created beside the main context 0xd", NULL));
#else
    (void)creates0;
#endif
    dev_finish(&d, 0);
}

/* One fence, two contexts, and fences that retire later from another thread: a launch left in
 * the open batch with no wait by the caller, then a copy, then a launch and a copy again. The
 * library must keep the two contexts from having work out at the same time, or a value of one
 * would wake a wait for the other's (FakeSubmitCommand counts both kinds of violation). */
static void case_two_contexts_one_fence(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, local;
    bc250hsa_op_receipt    r;
    HANDLE                 pump;
    bc250hsa_counters      c0, c1;
    uint64_t               fence = 0u, last = 0u;
    uint32_t               round;

    printf("copy lifecycle: launches and copies on one fence, retired later\n");
    dev_init(&d);
    batching_on(&d);
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, 0u, &local) && local.host == NULL);
    d.wait_total_ms = 5000u;
    g_hold = 1;
    pump = pump_start(5u, 0u);
    CHECK(counters(&c0));
    for (round = 0; round < 3u; round++) {
        CHECK(send_launch(&d, &fence) == BC250HSA_OK && fence > last);
        r = receipt_new();
        CHECK(bc250hsa_copy_device_to_device(&d, &local, 0u, &src, 0u, 4096u, 0u, &r) ==
              BC250HSA_OK);
        CHECK(r.state == BC250HSA_OP_RETIRED && r.fence_value > fence);
        last = r.fence_value;
    }
    CHECK(bc250hsa_wait(&d, last, 0u, 0u) == BC250HSA_OK && !d.device_lost);
    /* Each copy found its launch still in the open batch and submitted it early. */
    CHECK(counters(&c1) && c1.gpu_copy_forced_flushes - c0.gpu_copy_forced_flushes == 3u);
    pump_stop(pump);
    dev_finish(&d, 0);
}

#ifdef BC250HSA_FENCE_CHECKS
/* The copy context could not be created: no copy is submitted anywhere, the control cannot ask
 * its question, the device is not lost, the creation is tried once, and launches go on. */
static void case_copy_context_fails(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, local;
    bc250hsa_op_receipt    r;
    uint64_t               fence = 0u;
    uint32_t               ready = 7u, submits0, creates0 = g_context_creates;

    printf("copy lifecycle: the copy context cannot be created\n");
    dev_init(&d);
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, 0u, &local));
    CHECK(send_launch(&d, &fence) == BC250HSA_OK && bc250hsa_wait(&d, fence, 0u, 0u) == BC250HSA_OK);
    g_context_fail = 1;
    submits0 = g_submits;
    CHECK(bc250hsa_gpu_copy_ready(&d, &ready) == BC250HSA_EOS && ready == 0u);
    CHECK(d.gpu_copy_control == BC250HSA_CONTROL_UNTRIED && !d.device_lost);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &local, 0u, &src, 0u, 4096u, 0u, &r) == BC250HSA_EOS);
    CHECK(r.state == BC250HSA_OP_NOT_SUBMITTED && r.fence_value == 0u);
    CHECK(g_submits == submits0 && g_context_creates - creates0 == 1u && d.copy_context == 0u);
    CHECK(send_launch(&d, &fence) == BC250HSA_OK && bc250hsa_wait(&d, fence, 0u, 0u) == BC250HSA_OK);
    CHECK(!d.device_lost);
    dev_finish(&d, 0);
}

/* The report of a loss. The device goes into its error state at a copy's submission, as it
 * did at the control's in the lab round; the call says so by name, and one line carries the
 * execution state, the fence and the last submission. */
static void case_loss_report(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, host_dst;
    bc250hsa_op_receipt    r;
    uint32_t               state_calls0;

    printf("copy lifecycle: the device goes into its error state at a copy\n");
    dev_ready(&d);
    state_calls0 = g_state_calls;
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, BC250HSA_MEM_HOST, &host_dst));
    log_clear();
    g_error_next = 1;
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 4096u,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == BC250HSA_EDEVICELOST);
    CHECK(r.state == BC250HSA_OP_SUBMITTED && r.fence_value != 0u && d.device_lost);
    CHECK(log_has("fence check: device lost: execution state 6; fence handle 0x1f type 5",
                  "reads 18446744073709551615"));
#ifdef BC250HSA_COPY_LOCK_WAITS_SAID
    /* The 6 is the answer of the device-state query itself, not a number of the test. */
    CHECK(g_state_calls - state_calls0 != 0u);
#else
    (void)state_calls0;
#endif
    CHECK(log_has("fence check: device lost", "on the copy context 0x701, 0 records, upload 0"));
    CHECK(log_has("fence check: copy: submit on the copy context", NULL));
    dev_finish(&d, 1);
}

/* Review R1 of e91f8693: a copy whose wait ends without retirement keeps the device to itself
 * after the call returns, until the fence shows that it retired. kind 0 is a timeout, 1 a
 * failed wait registration, 2 a failed CreateEventW; batch 1 turns batching on. The fake
 * counts a buffer of one context submitted while the other context's work is out
 * (g_order_violations), which is the reorder the exclusion prevents, and a value that does
 * not grow. The case runs on any tree with a copy context, and e91f8693 fails it. Its last
 * part reads the exclusion on a lost device (ABI 1.8 and later), and bde0ff88 fails that. */
static volatile LONG            g_late_done;
static volatile bc250hsa_status g_late_status;
static uint64_t                 g_late_fence;

static DWORD WINAPI late_launch(LPVOID arg)
{
    uint64_t fence = 0u;
    g_late_status = send_launch((struct bc250hsa_device*)arg, &fence);
    g_late_fence = fence;
    InterlockedExchange(&g_late_done, 1);
    return 0;
}

/* The re-wait for the unretired copy fails the way the copy's own wait did. */
static void fail_next_wait(int kind)
{
    g_register_fail_at = (kind == 1) ? g_register_calls + 1u : 0u;
    g_event_fail_at = (kind == 2) ? g_event_calls + 1u : 0u;
}

static void case_unretired_copy_excludes(int kind, int batch)
{
    static const char* const names[3] = { "a timeout", "a failed wait registration",
                                           "a failed CreateEventW" };
    static const bc250hsa_status want[3] = { BC250HSA_ETIMEOUT, BC250HSA_EOS, BC250HSA_EOS };
    struct bc250hsa_device d;
    bc250hsa_mem           src, host_dst, local;
    bc250hsa_op_receipt    r;
    bc250hsa_status        status;
    HANDLE                 late;
    uint64_t               fence = 0u, copy_value;
    uint32_t               submits0, violations0;
#ifdef BC250HSA_COPY_EXCLUSION
    bc250hsa_counters      c0, c1;
#endif
#if BC250HSA_ABI_VERSION_MINOR >= 8
    uint64_t               pending;
#endif

    printf("copy lifecycle: a launch after a copy whose wait ended in %s, batching %s\n",
           names[kind], batch ? "on" : "off");
    dev_ready(&d);
    if (batch) { batching_on(&d); }
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, BC250HSA_MEM_HOST, &host_dst));
    CHECK(alloc_mem(&d, 65536u, 0u, &local));
    memset(host_dst.host, 0xEE, 65536u);
    /* A recorded launch first, so the main context's journal is on, as in llama.cpp. */
    CHECK(send_launch(&d, &fence) == BC250HSA_OK && bc250hsa_wait(&d, fence, 0u, 0u) == BC250HSA_OK);
#ifdef BC250HSA_COPY_EXCLUSION
    CHECK(counters(&c0));
#endif

    /* The copy: the device accepts it, and its wait ends without retirement. */
    g_hold = 1;
    fail_next_wait(kind);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 65536u,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == want[kind]);
    CHECK(r.state == BC250HSA_OP_SUBMITTED && r.fence_value == g_submitted && !d.device_lost);
    copy_value = r.fence_value;
    submits0 = g_submits;
    violations0 = g_order_violations;
#if BC250HSA_ABI_VERSION_MINOR >= 8
    /* Layer 2's read of the same state, before a host-processor copy: it names the copy and the
     * status of its wait, and it neither waits nor submits. */
    pending = 0u;
    CHECK(bc250hsa_copy_unretired(&d, &pending) == want[kind] && pending == copy_value);
    CHECK(g_submits == submits0 && d.copy_unretired == copy_value);
#endif

    /* The same thread launches at once. The wait for the copy fails again, so the launch is
     * refused with that wait's status, and nothing goes out: no submission, no fence value
     * and no open batch. */
    fail_next_wait(kind);
    log_clear();
    fence = 7u;
    status = send_launch(&d, &fence);
    if (status == BC250HSA_OK) {
        printf("  a launch went out while the copy with fence value %llu was unretired"
               " (fence value %llu)\n", (unsigned long long)copy_value, (unsigned long long)fence);
    }
    CHECK(status == want[kind] && fence == 0u);
    CHECK(g_submits == submits0 && g_order_violations == violations0 && !d.batch_open);
    /* Another copy is refused the same way, with nothing submitted. */
    fail_next_wait(kind);
    r = receipt_new();
    status = bc250hsa_copy_device_to_device(&d, &local, 0u, &src, 0u, 4096u, 0u, &r);
    CHECK(status == want[kind] && r.state == BC250HSA_OP_NOT_SUBMITTED && g_submits == submits0);
    CHECK(g_order_violations == violations0 && !d.device_lost);
#ifdef BC250HSA_COPY_EXCLUSION
    CHECK(d.copy_unretired == copy_value);
    CHECK(log_has("copy exclusion: refused a dispatch: the copy with fence value",
                  "nothing of this device is submitted until it has"));
    CHECK(log_has("copy exclusion: refused a copy: the copy with fence value", NULL));
#endif
    fail_next_wait(-1);

    /* Another thread launches. It waits for the copy under the device's bound, and the copy
     * retires late, after that thread has waited 30 ms: the launch then goes out after the
     * copy, with the next value. */
    d.wait_total_ms = 5000u;
    InterlockedExchange(&g_late_done, 0);
    g_late_status = BC250HSA_EINVAL;
    g_late_fence = 0u;
    late = CreateThread(NULL, 0, late_launch, &d, 0, NULL);
    CHECK(late != NULL);
    Sleep(30);
    if (InterlockedCompareExchange(&g_late_done, 0, 0) != 0) {
        printf("  a launch on another thread returned %d with fence value %llu while the copy"
               " with fence value %llu was unretired\n", (int)g_late_status,
               (unsigned long long)g_late_fence, (unsigned long long)copy_value);
    }
    CHECK(InterlockedCompareExchange(&g_late_done, 0, 0) == 0 && g_submits == submits0);
    fake_retire_all();                       /* the late retirement */
    if (late != NULL) {
        CHECK(WaitForSingleObject(late, 10000) == WAIT_OBJECT_0);
        CloseHandle(late);
    }
    CHECK(g_late_status == BC250HSA_OK && g_late_fence == copy_value + 1u);
    g_hold = 0;
    fake_retire_all();                       /* the launch, when it went out with g_hold on */
    CHECK(bc250hsa_wait(&d, g_late_fence, 0u, 0u) == BC250HSA_OK);
    CHECK(memcmp(host_dst.host, src.host, 65536u) == 0);     /* the copy's late write landed */
#ifdef BC250HSA_COPY_EXCLUSION
    CHECK(d.copy_unretired == 0u);
    CHECK(counters(&c1));
    CHECK(c1.gpu_copy_unretired - c0.gpu_copy_unretired == 1u);
    CHECK(c1.gpu_copy_exclusion_refusals - c0.gpu_copy_exclusion_refusals == 2u);
    CHECK(c1.gpu_copy_late_retirements - c0.gpu_copy_late_retirements == 1u);
    CHECK(log_has("copy exclusion: the copy with fence value", "retired late"));
#endif
#if BC250HSA_ABI_VERSION_MINOR >= 8
    CHECK(bc250hsa_copy_unretired(&d, &pending) == BC250HSA_OK && pending == 0u);
#endif

    /* Submissions go on as before: a launch and a copy, both retired at once. */
    CHECK(send_launch(&d, &fence) == BC250HSA_OK && fence == g_late_fence + 1u);
    CHECK(bc250hsa_wait(&d, fence, 0u, 0u) == BC250HSA_OK);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &local, 0u, &src, 0u, 4096u, 0u, &r) == BC250HSA_OK);
    CHECK(r.state == BC250HSA_OP_RETIRED && !d.device_lost);
#if BC250HSA_ABI_VERSION_MINOR >= 8
    /* A second copy whose wait fails, which the fence then passes with no submission between:
     * layer 2's read finds it retired and ends the exclusion itself. */
    d.wait_total_ms = 60u;
    CHECK(counters(&c0));
    g_hold = 1;
    fail_next_wait(kind);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 65536u,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == want[kind]);
    fail_next_wait(-1);
    CHECK(r.state == BC250HSA_OP_SUBMITTED && d.copy_unretired == r.fence_value);
    fake_retire_all();
    g_hold = 0;
    log_clear();
    CHECK(bc250hsa_copy_unretired(&d, &pending) == BC250HSA_OK && pending == 0u);
    CHECK(d.copy_unretired == 0u);
    CHECK(counters(&c1) && c1.gpu_copy_late_retirements - c0.gpu_copy_late_retirements == 1u);
    CHECK(log_has("copy exclusion: the copy with fence value", "host-processor copies go on"));

    /* Review R3 of bde0ff88: a loss is the end, whatever the fence reads. A third copy whose wait
     * fails, then a later call marks the device lost. The read answers EDEVICELOST and keeps the
     * copy's value while the fence is below it, after the fence has reached it, and with no copy
     * unretired at all; it retires nothing and says no "go on" line. A finite fence value of a
     * lost device proves no retirement. */
    CHECK(counters(&c0));
    g_hold = 1;
    fail_next_wait(kind);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &host_dst, 0u, &src, 0u, 65536u,
                                         BC250HSA_COPY_HOST_READS_DST, &r) == want[kind]);
    fail_next_wait(-1);
    copy_value = r.fence_value;
    CHECK(r.state == BC250HSA_OP_SUBMITTED && d.copy_unretired == copy_value);
    bc250hsa_mark_lost(&d, "a later call found the device lost");
    submits0 = g_submits;
    pending = 0u;
    CHECK(bc250hsa_copy_unretired(&d, &pending) == BC250HSA_EDEVICELOST && pending == copy_value);
    fake_retire_all();                       /* a finite fence that has reached the copy */
    g_hold = 0;
    log_clear();
    pending = 0u;
    status = bc250hsa_copy_unretired(&d, &pending);
    if (status != BC250HSA_EDEVICELOST) {
        printf("  the read answered %d on a lost device whose fence reached the copy with fence"
               " value %llu\n", (int)status, (unsigned long long)copy_value);
    }
    CHECK(status == BC250HSA_EDEVICELOST && pending == copy_value);
    CHECK(d.copy_unretired == copy_value);
    d.copy_unretired = 0u;                   /* no copy unretired, and the device still lost */
    pending = 7u;
    status = bc250hsa_copy_unretired(&d, &pending);
    if (status != BC250HSA_EDEVICELOST) {
        printf("  the read answered %d on a lost device with no copy unretired\n", (int)status);
    }
    CHECK(status == BC250HSA_EDEVICELOST && pending == 0u);
    CHECK(counters(&c1) && c1.gpu_copy_late_retirements == c0.gpu_copy_late_retirements);
    CHECK(g_log_count == 0u);                /* no "retired late ... go on" line, no other */
    CHECK(g_submits == submits0);
    dev_finish(&d, 1);
#else
    dev_finish(&d, 0);
#endif
}

#ifdef BC250HSA_COPY_LOCK_WAITS_SAID
/* What a copy's hold of the device lock costs another thread (CONTRACT.md deviation 13.23).
 * The copy holds dev->lock from the submission of the open batch to its own retirement. A
 * thread that calls layer 1 meanwhile waits for it, and gpu_copy_lock_waits must say so; a
 * process that calls layer 1 from one thread must read 0 waits however much it copies. */
static volatile bc250hsa_status g_side_status;
static uint64_t                 g_side_ticks;
static uint32_t                 g_side_pending_at_return;

static DWORD WINAPI side_call(LPVOID arg)
{
    struct bc250hsa_device* d = (struct bc250hsa_device*)arg;
    const ULONGLONG         give_up = GetTickCount64() + 5000u;
    uint64_t                start;

    /* Until the copy holds the lock and its buffer is out on the copy context. */
    for (;;) {
        int out;
        EnterCriticalSection(&g_fake);
        out = (g_pending_count != 0u && g_pending[g_pending_count - 1u].context != d->context);
        LeaveCriticalSection(&g_fake);
        if ((InterlockedCompareExchange(&d->copy_lock_depth, 0, 0) > 0 && out) ||
            GetTickCount64() > give_up) {
            break;
        }
        SwitchToThread();   /* not Sleep(1), which can take a whole 15.6 ms timer tick */
    }
    /* Any call that takes the device lock; a launch or an event record takes it the same way. */
    start = bc250hsa_ticks();
    g_side_status = bc250hsa_wait_policy_set(d, d->wait_slice_ms, d->wait_total_ms);
    g_side_ticks = bc250hsa_ticks() - start;
    EnterCriticalSection(&g_fake);
    g_side_pending_at_return = g_pending_count;
    LeaveCriticalSection(&g_fake);
    return 0;
}

static void case_copy_lock(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           src, local;
    bc250hsa_op_receipt    r;
    bc250hsa_counters      c;
    HANDLE                 pump, side;
    uint32_t               i;

    printf("copy lifecycle: what a copy's hold of the device lock costs another thread\n");
    log_clear();
    dev_ready(&d);
    /* The control is the device's first hold. */
    CHECK(d.copy_lock_holds == 1u && d.copy_lock_depth == 0);
    CHECK(log_has("fence check: copy lock: hold 1 of this device", NULL));
    CHECK(source_mem(&d, 65536u, &src));
    CHECK(alloc_mem(&d, 65536u, 0u, &local));
    d.wait_total_ms = 5000u;

    /* One thread: three holds and no wait, which is the answer for llama.cpp, vadd and
     * hipbench, and the summary line at the device's 2nd and 4th hold. */
    bc250hsa_counters_reset();
    log_clear();
    for (i = 0; i < 3u; i++) {
        r = receipt_new();
        CHECK(bc250hsa_copy_device_to_device(&d, &local, 0u, &src, 0u, 4096u, 0u, &r) ==
              BC250HSA_OK);
    }
    CHECK(counters(&c) && c.gpu_copy_lock_holds == 3u && c.gpu_copy_lock_waits == 0u);
    CHECK(c.gpu_copy_lock_wait_ticks == 0u && c.gpu_copy_lock_ticks_max <= c.gpu_copy_lock_ticks);
    CHECK(log_has("fence check: copy lock: hold 2 of this device; the process's 1 holds",
                  "other threads waited for them 0 times"));
    CHECK(log_has("fence check: copy lock: hold 4 of this device; the process's 3 holds", NULL));
    CHECK(d.copy_lock_holds == 4u && d.copy_lock_depth == 0);

    /* Two threads: the copy's fence retires 60 ms after its submission, and the other thread
     * takes the device lock meanwhile. It gets the lock only once the copy retired. */
    bc250hsa_counters_reset();
    log_clear();
    g_hold = 1;
    pump = pump_start(60u, 0u);
    g_side_status = BC250HSA_EINVAL;
    g_side_ticks = 0u;
    g_side_pending_at_return = 99u;
    side = CreateThread(NULL, 0, side_call, &d, 0, NULL);
    CHECK(side != NULL);
    r = receipt_new();
    CHECK(bc250hsa_copy_device_to_device(&d, &local, 0u, &src, 0u, 4096u, 0u, &r) == BC250HSA_OK);
    CHECK(r.state == BC250HSA_OP_RETIRED);
    if (side != NULL) {
        CHECK(WaitForSingleObject(side, 10000) == WAIT_OBJECT_0);
        CloseHandle(side);
    }
    pump_stop(pump);
    g_hold = 0;
    CHECK(g_side_status == BC250HSA_OK && g_side_pending_at_return == 0u);
    CHECK(counters(&c) && c.gpu_copy_lock_holds == 1u && c.gpu_copy_lock_waits == 1u);
    CHECK(c.gpu_copy_lock_wait_ticks >= c.ticks_per_second / 50u);   /* 20 ms of the 60 */
    CHECK(c.gpu_copy_lock_wait_ticks_max == c.gpu_copy_lock_wait_ticks);
    /* The wait lies inside the hold, but the waiter reads the clock after it wakes up, which is
     * after the holder's last reading: 10 ms for that wake-up (one run measured 180 us). */
    CHECK(c.gpu_copy_lock_wait_ticks <= c.gpu_copy_lock_ticks + c.ticks_per_second / 100u);
    CHECK(g_side_ticks >= c.gpu_copy_lock_wait_ticks);
    CHECK(log_has("fence check: copy lock: a thread waited", "that a GPU copy held"));
    if (c.ticks_per_second != 0u) {
        printf("  one copy held the device lock %llu us; the other thread waited %llu us of it\n",
               (unsigned long long)(c.gpu_copy_lock_ticks * 1000000u / c.ticks_per_second),
               (unsigned long long)(c.gpu_copy_lock_wait_ticks * 1000000u / c.ticks_per_second));
    }
    dev_finish(&d, 0);
}
#endif
#endif

int main(void)
{
    int kind;
    InitializeCriticalSection(&g_fake);
    bc250hsa_set_log(print_log, NULL, BC250HSA_LOG_INFO);
    g_next_handle = 0x100u;
    launch_init();
    case_lab_shape(0);
    case_lab_shape(1);
    case_two_contexts_one_fence();
#ifdef BC250HSA_FENCE_CHECKS
    case_copy_context_fails();
    case_loss_report();
    for (kind = 0; kind < 6; kind++) { case_unretired_copy_excludes(kind % 3, kind / 3); }
#ifdef BC250HSA_COPY_LOCK_WAITS_SAID
    case_copy_lock();
#endif
#endif
    case_everything_retires();
    case_control_delayed();
    for (kind = 0; kind < 4; kind++) { case_control_inconclusive(kind); }
    case_control_removed();
    for (kind = 0; kind < 4; kind++) { case_direct_unretired(kind); }
    case_staged_round_fails();
    case_fill_unretired();
    case_submit_fails();
    case_alias_gate();
    DeleteCriticalSection(&g_fake);
    printf("copy lifecycle: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
