/* submit.c - the command ring, the submission and the bounded wait.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, sections 3.2 and 3.8.
 *
 * One constraint from the kernel driver, quoted in driver/kmd/wddm.c: "One IB is
 * already the ring's whole capacity". A second submission that arrives before the
 * first fence waits inside the display driver interface. This file therefore
 * serialises submissions per device and keeps a ring of command buffer slots, each
 * slot retired by its own fence value before it is written again.
 *
 * The wait is sliced. When another process hangs the engine, this device's work waits
 * behind it until the scheduler resets the engine and resubmits, which took 14.6
 * seconds in lab trial D1 of kernel driver 0.7.216.17 with TdrDelay 10. A single long
 * wait turned that healthy wait into a lost device (defect K225), so the slice
 * default stays 1000 ms and the total default stays 120000 ms, and a short trial
 * passes a smaller total by hand.
 */
#include <stdlib.h>

#include "kmt_device.h"
#include "pm4_regs.h"

uint64_t bc250hsa_fence_read(struct bc250hsa_device* dev)
{
    if (dev == NULL || dev->fence_cpu == NULL) {
        return 0u;
    }
    return *dev->fence_cpu;
}

uint64_t bc250hsa_fence_last_submitted(struct bc250hsa_device* dev)
{
    return (dev == NULL) ? 0u : dev->fence_last_assigned;
}

uint64_t bc250hsa_now_us(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER        now;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart == 0) {
            return 0u;
        }
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart / frequency.QuadPart) * 1000000) +
           (uint64_t)(((now.QuadPart % frequency.QuadPart) * 1000000) / frequency.QuadPart);
}

/* Ticks of bc250hsa_ticks in microseconds, for a log line. */
static uint64_t ticks_us(uint64_t ticks)
{
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return 0u;
    }
    return (ticks / (uint64_t)frequency.QuadPart) * 1000000u +
           ((ticks % (uint64_t)frequency.QuadPart) * 1000000u) / (uint64_t)frequency.QuadPart;
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

int32_t bc250hsa_device_execution_state(struct bc250hsa_device* dev)
{
    D3DKMT_GETDEVICESTATE state;
    NTSTATUS              status;

    memset(&state, 0, sizeof(state));
    state.hDevice = dev->device;
    state.StateType = D3DKMT_DEVICESTATE_EXECUTION;
    status = D3DKMTGetDeviceState(&state);
    if (!NT_SUCCESS(status)) {
        return (int32_t)status;
    }
    return (int32_t)state.ExecutionState;
}

/* --------------------------------------------------------------------------------
 * The device lock and what a GPU copy's hold of it costs (CONTRACT.md deviation 13.23)
 *
 * A GPU copy holds dev->lock from the submission of the open batch to the retirement of its
 * own value, because the main context and the copy context write one fence and the
 * scheduler orders nothing across two contexts. Layer 2 releases its process lock around
 * the copy (CONTRACT.md 13.19), but a thread that enters this library on the same device
 * meanwhile, for a launch, an event record, a flush or an allocation, waits for the copy;
 * layer 2 holds its process lock across those calls, so a third thread can then wait for
 * that one. The cost is therefore 0 for a process that calls layer 1 from one thread,
 * which llama.cpp, vadd and hipbench do; otherwise a call waits for the copy in progress,
 * and for more only when several threads copy at once. These functions measure it instead
 * of assuming it: every hold, every wait for a hold, and a summary line at the 1st, 2nd,
 * 4th, 8th ... hold of a device.
 * ------------------------------------------------------------------------------ */

void bc250hsa_device_lock(struct bc250hsa_device* dev)
{
    uint64_t start;
    uint64_t waited;
    LONG     copy;

    if (TryEnterCriticalSection(&dev->lock)) {
        return;
    }
    /* Read before the wait: the copy that holds the lock now. A copy that starts while this
     * thread already waits is not counted, which errs towards fewer waits by one at most. */
    copy = InterlockedCompareExchange(&dev->copy_lock_depth, 0, 0);
    start = bc250hsa_ticks();
    EnterCriticalSection(&dev->lock);
    if (copy <= 0) {
        return;
    }
    waited = bc250hsa_ticks() - start;
    bc250hsa_count_add(BC250HSA_C_GPU_COPY_LOCK_WAITS, 1u);
    bc250hsa_count_add(BC250HSA_C_GPU_COPY_LOCK_WAIT_TICKS, waited);
    bc250hsa_count_max(BC250HSA_C_GPU_COPY_LOCK_WAIT_TICKS_MAX, waited);
    if (dev->copy_lock_waits_said < BC250HSA_COPY_LOCK_WAITS_SAID) {
        dev->copy_lock_waits_said++;
        bc250hsa_log(BC250HSA_LOG_INFO,
                     "fence check: copy lock: a thread waited %llu us for the device lock that a"
                     " GPU copy held (said for the first %u waits of this device;"
                     " gpu_copy_lock_waits counts every one)",
                     (unsigned long long)ticks_us(waited), BC250HSA_COPY_LOCK_WAITS_SAID);
    }
}

void bc250hsa_copy_lock_enter(struct bc250hsa_device* dev)
{
    bc250hsa_device_lock(dev);
    if (InterlockedIncrement(&dev->copy_lock_depth) == 1) {
        dev->copy_lock_start = bc250hsa_ticks();
    }
}

void bc250hsa_copy_lock_leave(struct bc250hsa_device* dev)
{
    if (InterlockedDecrement(&dev->copy_lock_depth) == 0) {
        const uint64_t held = bc250hsa_ticks() - dev->copy_lock_start;
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_LOCK_HOLDS, 1u);
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_LOCK_TICKS, held);
        bc250hsa_count_max(BC250HSA_C_GPU_COPY_LOCK_TICKS_MAX, held);
        dev->copy_lock_holds++;
        if ((dev->copy_lock_holds & (dev->copy_lock_holds - 1u)) == 0u) {
            bc250hsa_counters c;
            memset(&c, 0, sizeof(c));
            c.struct_bytes = (uint32_t)sizeof(c);
            if (bc250hsa_counters_read(&c) == BC250HSA_OK) {
                /* The numbers after the first are the process's. The waits are the ones that
                 * ended before this hold did: a thread that waits for this very hold is
                 * counted when it gets the lock, after this line. */
                bc250hsa_log(BC250HSA_LOG_INFO,
                             "fence check: copy lock: hold %llu of this device; the process's"
                             " %llu holds took %llu us in all, the longest %llu us; other"
                             " threads waited for them %llu times, %llu us in all, the longest"
                             " %llu us",
                             (unsigned long long)dev->copy_lock_holds,
                             (unsigned long long)c.gpu_copy_lock_holds,
                             (unsigned long long)ticks_us(c.gpu_copy_lock_ticks),
                             (unsigned long long)ticks_us(c.gpu_copy_lock_ticks_max),
                             (unsigned long long)c.gpu_copy_lock_waits,
                             (unsigned long long)ticks_us(c.gpu_copy_lock_wait_ticks),
                             (unsigned long long)ticks_us(c.gpu_copy_lock_wait_ticks_max));
            }
        }
    }
    LeaveCriticalSection(&dev->lock);
}

/* --------------------------------------------------------------------------------
 * The fence checks (scratch/hip-gpu-copy/CONTRACT.md deviation 13.23)
 *
 * A device that its fence mapping reports removed, UINT64_MAX, with no TDR, no fault and
 * no reset, stopped the first lab round of the GPU copy (2026-10-11). The mapping, the
 * object behind it and the submission before the reading were not in the record, so the
 * cause had to be found offline. These lines put them there: the submissions of the
 * coherence control, the first two ordinary ones and the first two copies are reported as
 * they go out and again when a wait covers them, and a loss (the removal sentinel, or an
 * execution state that is not active) is reported with the last submission and the
 * execution state of the device.
 * ------------------------------------------------------------------------------ */

static const char* context_name(const struct bc250hsa_device* dev, D3DKMT_HANDLE context)
{
    if (context != 0u && context == dev->context) { return "main"; }
    if (context != 0u && context == dev->copy_context) { return "copy"; }
    return "unknown";
}

/* One traced submission as it goes to the kernel driver. The caller holds dev->lock. */
static void check_submitted(struct bc250hsa_device* dev, const bc250hsa_fence_check* c)
{
    bc250hsa_log(BC250HSA_LOG_INFO,
                 "fence check: %s: submit on the %s context 0x%x (journal %s, %u dispatch"
                 " records, upload %llu); fence handle 0x%x type %u flags 0x%x cpu va %p;"
                 " target %llu, value before submit %llu",
                 c->site, context_name(dev, c->context), (unsigned)c->context,
                 dev->journal_required ? "on" : "off", c->records,
                 (unsigned long long)c->upload_id, (unsigned)dev->fence, dev->fence_type,
                 dev->fence_flags, (const void*)dev->fence_cpu,
                 (unsigned long long)c->target, (unsigned long long)c->before);
    if (dev->check_count < BC250HSA_FENCE_CHECKS) {
        dev->checks[dev->check_count++] = *c;
    }
}

/* The second half of every traced submission that a wait for value covers. Most waits find
 * nothing to report, and they do not take the lock for that. A wait never queues for the
 * lock for a report either: when another thread holds it (a staged copy holds it across
 * its waits), the report waits for a later wait, which says the value it found then. A
 * wait inside the lock, the control's and every copy's, always reports, because the
 * lock is recursive for the thread that holds it. */
static void check_waited(struct bc250hsa_device* dev, uint64_t value, bc250hsa_status result)
{
    uint32_t i = 0u;

    if (*(volatile uint32_t*)&dev->check_count == 0u) {
        return;
    }
    if (!TryEnterCriticalSection(&dev->lock)) {
        return;
    }
    while (i < dev->check_count) {
        const bc250hsa_fence_check c = dev->checks[i];
        if (c.target > value) {
            i++;
            continue;
        }
        bc250hsa_log((result == BC250HSA_OK) ? BC250HSA_LOG_INFO : BC250HSA_LOG_ERROR,
                     "fence check: %s: the wait for %llu ended %s; fence handle 0x%x cpu va %p;"
                     " target %llu, value after the wait %llu; execution state %d",
                     c.site, (unsigned long long)value, bc250hsa_status_string(result),
                     (unsigned)dev->fence, (const void*)dev->fence_cpu,
                     (unsigned long long)c.target,
                     (unsigned long long)bc250hsa_fence_read(dev),
                     (int)bc250hsa_device_execution_state(dev));
        dev->checks[i] = dev->checks[dev->check_count - 1u];
        dev->check_count--;
    }
    LeaveCriticalSection(&dev->lock);
}

void bc250hsa_sentinel_lost(struct bc250hsa_device* dev, const char* why, uint64_t target)
{
    bc250hsa_device_lock(dev);
    if (!dev->device_lost) {
        const bc250hsa_fence_check* s = &dev->last_submit;
        /* The execution state goes first: the log line is cut at 512 bytes (status.c), and
         * that state is the one value the first round did not record. Its numbers are
         * D3DKMT_DEVICEEXECUTION_STATE (kmt_device.h). */
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "fence check: device lost: execution state %d; fence handle 0x%x"
                     " type %u flags 0x%x cpu va %p gpu va 0x%llx reads %llu, wanted %llu;"
                     " last submission %llu on the %s context 0x%x, %u records, upload %llu,"
                     " value before it %llu; journal %s; copy context 0x%x",
                     (int)bc250hsa_device_execution_state(dev), (unsigned)dev->fence,
                     dev->fence_type, dev->fence_flags, (const void*)dev->fence_cpu,
                     (unsigned long long)dev->fence_gpu_va,
                     (unsigned long long)bc250hsa_fence_read(dev), (unsigned long long)target,
                     (unsigned long long)s->target, context_name(dev, s->context),
                     (unsigned)s->context, s->records, (unsigned long long)s->upload_id,
                     (unsigned long long)s->before, dev->journal_required ? "on" : "off",
                     (unsigned)dev->copy_context);
    }
    bc250hsa_mark_lost(dev, why);
    LeaveCriticalSection(&dev->lock);
}

/* The wait itself. bc250hsa_wait below times it; *fast says that it ended on the fence
 * mapping, which is the wait waits_fast counts. */
static bc250hsa_status wait_counted(struct bc250hsa_device* dev, uint64_t value,
                                    uint32_t slice_ms, uint32_t total_ms, int* fast)
{
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;
    HANDLE                                     event;
    NTSTATUS                                   status;
    uint64_t                                   waited_ms = 0;
    uint64_t                                   observed;
    bc250hsa_status                            result = BC250HSA_ETIMEOUT;

    if (slice_ms == 0u) { slice_ms = BC250HSA_WAIT_SLICE_MS_DEFAULT; }
    if (total_ms == 0u) { total_ms = BC250HSA_WAIT_TOTAL_MS_DEFAULT; }
    if (total_ms < slice_ms) { total_ms = slice_ms; }

    bc250hsa_count_add(BC250HSA_C_WAITS, 1u);
    if (dev->device_lost) {
        return BC250HSA_EDEVICELOST;
    }
    /* A value this device promised and has not submitted yet is in the open batch. The
     * fence can never reach it, so the batch goes out first: this is the flush point
     * that makes every wait of a caller correct with batching on, whether the caller
     * knows about batching or not (section 8.1).
     *
     * The lock is taken and released here and not held across the wait. take_slot()
     * calls this function with the lock already held, and a critical section is
     * re-entrant for the thread that owns it; at that point no batch is open, because
     * the caller submitted it before it asked for a slot. */
    if (value > dev->fence_last_submitted) {
        bc250hsa_device_lock(dev);
        result = bc250hsa_batch_submit_locked(dev);
        LeaveCriticalSection(&dev->lock);
        if (result != BC250HSA_OK) {
            return result;
        }
        result = BC250HSA_ETIMEOUT;
    }

    /* Step 1: the fence's host mapping. Most waits end here. */
    observed = bc250hsa_fence_read(dev);
    if (observed == UINT64_MAX) {
        bc250hsa_sentinel_lost(dev, "the fence mapping reads UINT64_MAX", value);
        return BC250HSA_EDEVICELOST;
    }
    if (observed >= value) {
        bc250hsa_count_add(BC250HSA_C_WAITS_FAST, 1u);
        *fast = 1;
        return BC250HSA_OK;
    }

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (event == NULL) {
        return BC250HSA_EOS;
    }
    memset(&wait, 0, sizeof(wait));
    wait.hDevice = dev->device;
    wait.ObjectCount = 1;
    wait.ObjectHandleArray = &dev->fence;
    wait.FenceValueArray = &value;
    wait.hAsyncEvent = event;
    status = D3DKMTWaitForSynchronizationObjectFromCpu(&wait);
    if (!NT_SUCCESS(status)) {
        CloseHandle(event);
        return bc250hsa_os_status("WaitForSynchronizationObjectFromCpu", status);
    }

    for (;;) {
        const uint32_t this_slice =
            (uint32_t)((total_ms - waited_ms < slice_ms) ? (total_ms - waited_ms) : slice_ms);
        const DWORD    woke = WaitForSingleObject(event, this_slice);

        if (woke == WAIT_OBJECT_0) {
            result = BC250HSA_OK;
            break;
        }
        if (woke != WAIT_TIMEOUT) {
            result = BC250HSA_EOS;
            break;
        }
        waited_ms += this_slice;
        observed = bc250hsa_fence_read(dev);
        /* Stop early only on a real loss. A wait that runs past one slice is not
         * one: it is this device's work queued behind another process's reset. */
        if (observed == UINT64_MAX) {
            bc250hsa_sentinel_lost(dev, "the fence mapping reads UINT64_MAX during the wait",
                                   value);
            result = BC250HSA_EDEVICELOST;
            break;
        }
        if (!bc250hsa_device_executing(dev)) {
            bc250hsa_sentinel_lost(dev, "the fence or the execution state reports a loss",
                                   value);
            result = BC250HSA_EDEVICELOST;
            break;
        }
        if (observed >= value) {
            result = BC250HSA_OK;
            break;
        }
        if (waited_ms >= total_ms) {
            result = BC250HSA_ETIMEOUT;
            break;
        }
        if (waited_ms == slice_ms) {
            bc250hsa_log(BC250HSA_LOG_WARN,
                         "fence value %llu pending after %llu ms, the device is still active",
                         (unsigned long long)value, (unsigned long long)waited_ms);
        }
    }
    CloseHandle(event);

    if (result == BC250HSA_OK) {
        observed = bc250hsa_fence_read(dev);
        if (observed == UINT64_MAX) {
            bc250hsa_sentinel_lost(dev, "the fence mapping reads UINT64_MAX after the wait",
                                   value);
            return BC250HSA_EDEVICELOST;
        }
        if (observed < value) {
            /* The operating system object woke, and the value the packet writes is
             * not there. That is the display driver interface and the packet
             * disagreeing, which is a device problem, not a timeout. */
            bc250hsa_mark_lost(dev, "the wait woke below the value the packet writes");
            return BC250HSA_EDEVICELOST;
        }
        return BC250HSA_OK;
    }
    if (result == BC250HSA_ETIMEOUT) {
        bc250hsa_count_add(BC250HSA_C_WAITS_TIMED_OUT, 1u);
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "timeout after %llu ms waiting for fence value %llu; the device still runs"
                     " and the submission is still in flight",
                     (unsigned long long)waited_ms, (unsigned long long)value);
    }
    return result;
}

bc250hsa_status bc250hsa_wait(struct bc250hsa_device* dev, uint64_t value, uint32_t slice_ms,
                              uint32_t total_ms)
{
    uint64_t        start;
    uint64_t        ticks;
    int             fast = 0;
    bc250hsa_status result;

    if (dev == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!dev->submit_capable) {
        return BC250HSA_EUNSUPPORTED;
    }
    /* Every wait that waits counts is timed, whichever way it ends, so the two totals
     * cover the same waits as the count. Two clock reads and two interlocked adds, and no
     * lock: the maximum is a compare-and-swap. */
    start = bc250hsa_ticks();
    result = wait_counted(dev, value, slice_ms, total_ms, &fast);
    ticks = bc250hsa_ticks() - start;
    check_waited(dev, value, result);
    if (fast) {
        bc250hsa_count_add(BC250HSA_C_WAITS_FAST_TICKS, ticks);
        bc250hsa_count_max(BC250HSA_C_WAITS_FAST_TICKS_MAX, ticks);
    } else {
        bc250hsa_count_add(BC250HSA_C_WAITS_OTHER_TICKS, ticks);
        bc250hsa_count_max(BC250HSA_C_WAITS_OTHER_TICKS_MAX, ticks);
    }
    return result;
}

/* A free command ring slot. A slot is free when the fence value it was last used with
 * has retired. Nothing is ever overwritten under the command processor. */
static bc250hsa_status take_slot(struct bc250hsa_device* dev, uint32_t* slot_out)
{
    uint32_t tried;

    for (tried = 0; tried < dev->ring_slots; tried++) {
        const uint32_t slot = (dev->next_slot + tried) % dev->ring_slots;
        const uint64_t observed = bc250hsa_fence_read(dev);
        if (observed == UINT64_MAX) {
            bc250hsa_sentinel_lost(dev, "removed device cannot retire an IB or scratch slot",
                                   dev->slot_fence[slot]);
            return BC250HSA_EDEVICELOST;
        }
        if (dev->slot_fence[slot] == 0u || observed >= dev->slot_fence[slot]) {
            dev->next_slot = (slot + 1u) % dev->ring_slots;
            *slot_out = slot;
            return BC250HSA_OK;
        }
    }
    /* Every slot is still in flight. Wait for the oldest one under the library
     * default bound, which is the same rule the interface states. */
    {
        const uint32_t        slot = dev->next_slot;
        const bc250hsa_status status = bc250hsa_wait(dev, dev->slot_fence[slot], 0u, 0u);
        if (status != BC250HSA_OK) {
            return (status == BC250HSA_ETIMEOUT) ? BC250HSA_EBUSY : status;
        }
        dev->next_slot = (slot + 1u) % dev->ring_slots;
        *slot_out = slot;
        return BC250HSA_OK;
    }
}

/* The environment of the indirect buffer this device builds: node 0 is the graphics ring
 * (fact M50: this part has no compute IP), the fence is this device's monitored fence,
 * and the barrier between two dispatches is the policy's. */
static void fill_env(struct bc250hsa_device* dev, bc250hsa_pm4_env* env, uint32_t flags,
                     uint64_t fence_value)
{
    memset(env, 0, sizeof(*env));
    env->struct_bytes = (uint32_t)sizeof(*env);
    env->flags = flags | BC250HSA_DISPATCH_GFX_RING;
    if (dev->batch.light_barrier != 0u) {
        env->flags |= BC250HSA_DISPATCH_LIGHT_BARRIER;
    }
    env->fence_va = dev->fence_gpu_va;
    env->fence_value = fence_value;
    memcpy(env->private_segment_rsrc, dev->private_segment_rsrc,
           sizeof(env->private_segment_rsrc));
    env->ib_pad_dwords = BC250HSA_IB_PAD_DWORDS;
}

static void journal_reset(struct bc250hsa_device* dev)
{
    dev->journal_count = 0u;
    dev->journal_bytes = (uint32_t)sizeof(BC250_HIP_JOURNAL_UPLOAD);
}

static void journal_append(struct bc250hsa_device* dev, const void* record, uint32_t bytes)
{
    if (record == NULL) { return; }
    memcpy(dev->journal_upload + dev->journal_bytes, record, bytes);
    dev->journal_bytes += bytes;
    dev->journal_count++;
}

/* This is a software-only context escape. The kernel validates and copies the
 * complete batch before SubmitCommand can make any of its dispatches runnable. */
static bc250hsa_status journal_upload(struct bc250hsa_device* dev, uint64_t ib_va,
                                       uint64_t fence_value, uint64_t* upload_id)
{
    BC250_HIP_JOURNAL_UPLOAD* header = (BC250_HIP_JOURNAL_UPLOAD*)dev->journal_upload;
    D3DKMT_ESCAPE escape;
    NTSTATUS status;
    *upload_id = 0u;
    if (dev->journal_count == 0u) { return BC250HSA_OK; }
    memset(header, 0, sizeof(*header));
    header->Magic = BC250_HIP_JOURNAL_MAGIC;
    header->Command = BC250_HIP_JOURNAL_COMMAND;
    header->Status = UINT32_MAX;
    header->AbiVersion = BC250_HIP_JOURNAL_ABI;
    header->TotalBytes = dev->journal_bytes;
    header->RecordCount = dev->journal_count;
    header->IbVa = ib_va;
    header->FenceVa = dev->fence_gpu_va;
    header->FenceValue = fence_value;
    memset(&escape, 0, sizeof(escape));
    escape.hAdapter = dev->adapter;
    escape.hDevice = dev->device;
    escape.hContext = dev->context;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.Flags.NoAdapterSynchronization = 1;
    escape.pPrivateDriverData = header;
    escape.PrivateDriverDataSize = dev->journal_bytes;
    status = D3DKMTEscape(&escape);
    if (!NT_SUCCESS(status)) { return bc250hsa_os_status("HIP journal upload", status); }
    if (header->Status != 0u || header->NtStatus != 0u || header->UploadId == 0u ||
        header->AbiVersion != BC250_HIP_JOURNAL_ABI) {
        bc250hsa_log(BC250HSA_LOG_ERROR, "HIP journal refused: status=%u ntstatus=0x%x",
                       header->Status, header->NtStatus);
        return BC250HSA_EUNSUPPORTED;
    }
    *upload_id = header->UploadId;
    return BC250HSA_OK;
}

static void journal_cancel(struct bc250hsa_device* dev, uint64_t upload_id)
{
    BC250_HIP_JOURNAL_UPLOAD header;
    D3DKMT_ESCAPE escape;
    NTSTATUS status;
    if (upload_id == 0u) { return; }
    memset(&header, 0, sizeof(header));
    header.Magic = BC250_HIP_JOURNAL_MAGIC;
    header.Command = BC250_HIP_JOURNAL_COMMAND;
    header.Status = UINT32_MAX;
    header.AbiVersion = BC250_HIP_JOURNAL_ABI;
    header.TotalBytes = sizeof(header);
    header.Operation = BC250_HIP_JOURNAL_CANCEL_OP;
    header.UploadId = upload_id;
    memset(&escape, 0, sizeof(escape));
    escape.hAdapter = dev->adapter;
    escape.hDevice = dev->device;
    escape.hContext = dev->context;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.Flags.NoAdapterSynchronization = 1;
    escape.pPrivateDriverData = &header;
    escape.PrivateDriverDataSize = sizeof(header);
    status = D3DKMTEscape(&escape);
    if (!NT_SUCCESS(status) || header.Status != 0u || header.NtStatus != 0u) {
        bc250hsa_mark_lost(dev, "HIP journal could not cancel a refused submission");
    }
}

/* --------------------------------------------------------------------------------
 * The exclusion of an unretired copy (CONTRACT.md deviation 13.23, review R1 of e91f8693)
 *
 * A copy runs alone: nothing of dev->context is out when it is submitted, and dev->lock is
 * held until it retires. A wait that ends without retirement, a timeout or a Windows call
 * that failed, ends that hold while the copy may still be in flight. Its receipt keeps the
 * caller's ranges, but a receipt stops no other submitter: a buffer of the main context
 * submitted now could run first and write N+1 into the one fence, which wakes every wait for
 * the copy's N while its bytes still move, and the copy's late write of N would then move the
 * fence backwards. Command ring slots, staging slots and the control's held buffers all
 * retire by that fence.
 *
 * So the copy's value stays in dev->copy_unretired, and every submission of this device, on
 * either context, first establishes that it retired: the fence reads it, or a wait for it
 * under the device's own bound succeeds. Otherwise the submission is refused with the status
 * of that wait, before anything is submitted, a fence value is handed out or a batch opened.
 * A failed wait is not a retirement; only the fence is. A loss stays the end it always was.
 * The caller holds dev->lock. what names the submission for the log.
 * ------------------------------------------------------------------------------ */
static bc250hsa_status copy_exclusion_locked(struct bc250hsa_device* dev, const char* what)
{
    const uint64_t  pending = dev->copy_unretired;
    uint64_t        observed;
    bc250hsa_status result = BC250HSA_OK;

    if (pending == 0u) {
        return BC250HSA_OK;
    }
    observed = bc250hsa_fence_read(dev);
    if (observed == UINT64_MAX) {
        bc250hsa_sentinel_lost(dev, "the fence mapping reads UINT64_MAX while a copy is unretired",
                               pending);
        return BC250HSA_EDEVICELOST;
    }
    if (observed < pending) {
        /* The rest of the copy's own hold: a thread that waits for it waits for a copy. */
        bc250hsa_copy_lock_enter(dev);
        result = bc250hsa_wait_internal(dev, pending);
        bc250hsa_copy_lock_leave(dev);
        observed = bc250hsa_fence_read(dev);
    }
    if (result != BC250HSA_OK) {
        dev->copy_unretired_status = result;
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_EXCLUSION_REFUSALS, 1u);
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "copy exclusion: refused %s: the copy with fence value %llu has not retired"
                     " (the wait for it ended %s; the fence reads %llu), and nothing of this"
                     " device is submitted until it has",
                     what, (unsigned long long)pending, bc250hsa_status_string(result),
                     (unsigned long long)observed);
        return bc250hsa_lost_status(dev, result);
    }
    dev->copy_unretired = 0u;
    bc250hsa_count_add(BC250HSA_C_GPU_COPY_LATE_RETIREMENTS, 1u);
    bc250hsa_log(BC250HSA_LOG_WARN,
                 "copy exclusion: the copy with fence value %llu retired late (the fence reads"
                 " %llu); submissions go on, the first is %s",
                 (unsigned long long)pending, (unsigned long long)observed, what);
    return BC250HSA_OK;
}

/* The same state, read for a caller that moves bytes with the host processor (bc250hsa.h
 * section 5.1): layer 2 asks before every host-processor copy or fill, because that work is no
 * submission and copy_exclusion_locked never sees it. The read takes dev->lock, never waits
 * and never submits, so it is one compare while no copy is unretired. A copy whose value the
 * fence has reached is retired here, as the next submission would retire it, but only on a
 * device that is not lost: a loss is the end whatever the fence reads (review R3 of bde0ff88),
 * and a finite fence value of a lost device proves no retirement. */
bc250hsa_status bc250hsa_copy_unretired(struct bc250hsa_device* dev, uint64_t* pending)
{
    uint64_t        value;
    uint64_t        observed;
    bc250hsa_status result = BC250HSA_OK;

    if (pending != NULL) {
        *pending = 0u;
    }
    if (dev == NULL || pending == NULL) {
        return BC250HSA_EINVAL;
    }
    bc250hsa_device_lock(dev);
    value = dev->copy_unretired;
    if (dev->device_lost) {
        /* The copy's value stays, retired or not, for the caller's line. */
        *pending = value;
        result = BC250HSA_EDEVICELOST;
    } else if (value != 0u) {
        observed = bc250hsa_fence_read(dev);
        if (observed == UINT64_MAX) {
            bc250hsa_sentinel_lost(dev, "the fence mapping reads UINT64_MAX while a copy is unretired",
                                   value);
            *pending = value;
            result = BC250HSA_EDEVICELOST;
        } else if (observed < value) {
            *pending = value;
            result = bc250hsa_lost_status(dev, dev->copy_unretired_status);
        } else {
            dev->copy_unretired = 0u;
            bc250hsa_count_add(BC250HSA_C_GPU_COPY_LATE_RETIREMENTS, 1u);
            bc250hsa_log(BC250HSA_LOG_WARN,
                         "copy exclusion: the copy with fence value %llu retired late (the fence"
                         " reads %llu); submissions and host-processor copies go on, the first is"
                         " a host-processor copy or fill",
                         (unsigned long long)value, (unsigned long long)observed);
        }
    }
    LeaveCriticalSection(&dev->lock);
    return result;
}

/* Hands one finished indirect buffer to the kernel driver, on context. The caller holds
 * dev->lock and has written dwords dwords into the slot. The private blob names one
 * indirect buffer, which is the one shape the kernel driver runs (driver/kmd/wddm.c
 * refuses a blob whose single_ib is 0). copy is 1 for a buffer of copy and fill packets
 * alone, which only the fence checks read.
 *
 * The journal rule of the kernel driver, enforced here before the call that would break
 * it: once a context has uploaded one dispatch record, HipJournalBind
 * (driver/kmd/hip_journal.c) binds every later buffer of that context to an uploaded
 * record and answers STATUS_INVALID_PARAMETER for a buffer with none. That status from
 * DxgkDdiSubmitCommandVirtual makes the operating system "put the calling device in an
 * error state and all subsequent calls on it will fail" (WDK, DXGKDDI_SUBMITCOMMANDVIRTUAL,
 * windows-driver-docs-ddi 7515063), and the user-mode SubmitCommand has returned success
 * by then: the device is lost a moment later and its fence reads UINT64_MAX. A buffer with
 * no record therefore never goes to dev->context once the journal is on, and this check
 * refuses it with nothing submitted. dev->journal_required turns on at the first recorded
 * dispatch, before its upload, so it is never later than the kernel driver's own switch. */
static bc250hsa_status submit_ib(struct bc250hsa_device* dev, uint32_t slot, uint32_t dwords,
                                 uint64_t fence_value, D3DKMT_HANDLE context, int copy)
{
    struct bc250hsa_submit_blob blob;
    D3DKMT_SUBMITCOMMAND        command;
    NTSTATUS                    status;
    bc250hsa_status              recorded;
    bc250hsa_fence_check        check;
    uint64_t                    upload_id;
    uint64_t                    start;
    const uint64_t              slot_va = dev->ring.va + (uint64_t)slot * dev->ring_slot_bytes;
    const uint32_t*             slot_host =
        (const uint32_t*)((const uint8_t*)dev->ring.host + (size_t)slot * dev->ring_slot_bytes);

    if (dwords > dev->last_ib_capacity) {
        return BC250HSA_ENOMEM;
    }
    /* Every caller settled an unretired copy before it handed out this fence value
     * (dispatch_submit, copy_submit_locked), so nothing is pending here. This is the last
     * line of that rule, for a path that one day forgets it. */
    if (dev->copy_unretired != 0u) {
        recorded = copy_exclusion_locked(dev, copy ? "a copy-only buffer" : "a dispatch buffer");
        if (recorded != BC250HSA_OK) {
            return recorded;
        }
    }
#ifndef BC250HSA_NEGATIVE_CONTROL_COPY_ON_MAIN_CONTEXT
    if (context == dev->context && dev->journal_required && dev->journal_count == 0u) {
        /* Never silent: every refusal is an error line that names the context and the
         * buffer, and journal_guard_refusals counts it. A correct build never gets here. */
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        bc250hsa_count_add(BC250HSA_C_JOURNAL_GUARD_REFUSALS, 1u);
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "journal guard: refused an indirect buffer with no dispatch record for the"
                     " main context 0x%x, whose journal is on: %s, fence value %llu, ring slot"
                     " %u at 0x%llx, %u dwords, %u dispatches open; last submission %llu, copy"
                     " context 0x%x. The kernel driver would refuse it and the device would be"
                     " lost, so nothing is submitted",
                     (unsigned)context, copy ? "a copy-only buffer" : "a dispatch buffer",
                     (unsigned long long)fence_value, slot, (unsigned long long)slot_va,
                     dwords, dev->batch_count, (unsigned long long)dev->fence_last_submitted,
                     (unsigned)dev->copy_context);
        return BC250HSA_EUNSUPPORTED;
    }
#endif
    memcpy(dev->last_ib, slot_host, (size_t)dwords * 4u);
    dev->last_ib_dwords = dwords;

    memset(&blob, 0, sizeof(blob));
    blob.magic = BC250HSA_SUBMIT_MAGIC;
    blob.version = BC250HSA_SUBMIT_VERSION;
    blob.size = BC250HSA_SUBMIT_PREFIX_BYTES + (uint32_t)sizeof(blob.ib[0]);
    blob.ip_type = BC250HSA_IP_GFX;
    blob.num_ibs = 1u;
    blob.fence_va = dev->fence_gpu_va;
    blob.fence_value = fence_value;
    blob.ib[0].va_start = slot_va;
    blob.ib[0].ib_bytes = dwords * 4u;
    blob.ib[0].ip_type = BC250HSA_IP_GFX;

    memset(&command, 0, sizeof(command));
    command.Commands = slot_va;
    command.CommandLength = dwords * 4u;
    command.pPrivateDriverData = &blob;
    command.PrivateDriverDataSize = blob.size;
    command.BroadcastContextCount = 1;
    command.BroadcastContext[0] = context;

    /* Host stores sit in the write buffers until a fence. The system call drains
     * them too; this makes the order obvious. The journal escape names dev->context, the
     * one context that ever uploads a record. */
    bc250hsa_write_barrier();
    recorded = journal_upload(dev, slot_va, fence_value, &upload_id);
    if (recorded != BC250HSA_OK) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return recorded;
    }
    memset(&check, 0, sizeof(check));
    check.target = fence_value;
    check.upload_id = upload_id;
    check.context = context;
    check.records = dev->journal_count;
    check.before = bc250hsa_fence_read(dev);
    /* The kernel transition alone: what is around it (the journal upload, the copy of
     * the buffer for bc250hsa_last_ib) is not inside these ticks. */
    start = bc250hsa_ticks();
    status = D3DKMTSubmitCommand(&command);
    bc250hsa_count_add(BC250HSA_C_SUBMIT_TICKS, bc250hsa_ticks() - start);
    bc250hsa_count_add(BC250HSA_C_SUBMIT_CALLS, 1u);
    if (!NT_SUCCESS(status)) {
        journal_cancel(dev, upload_id);
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return bc250hsa_os_status("SubmitCommand", status);
    }

    dev->fence_last_submitted = fence_value;
    dev->slot_fence[slot] = fence_value;
    bc250hsa_count_add(BC250HSA_C_SUBMISSIONS, 1u);

    check.site = "submission";
    dev->last_submit = check;
    if (dev->check_control != 0u) {
        check.site = "control";
        check_submitted(dev, &check);
    } else if (copy && dev->check_copy_left != 0u) {
        dev->check_copy_left--;
        check.site = "copy";
        check_submitted(dev, &check);
    } else if (!copy && dev->check_dispatch_left != 0u) {
        dev->check_dispatch_left--;
        check.site = "dispatch";
        check_submitted(dev, &check);
    }
    return BC250HSA_OK;
}

/* The dword capacity a batch of this device may fill under one policy's max_ib_dwords,
 * the completion write and the padding left aside. It takes the number and not the
 * device's policy, so that bc250hsa_batch_policy_set can test a policy before it is the
 * device's. */
static uint32_t dword_cap_for(const struct bc250hsa_device* dev, uint32_t max_ib_dwords)
{
    const uint32_t slot_dwords = dev->ring_slot_bytes / 4u;
    const uint32_t reserve = BC250HSA_RELEASE_MEM_DWORDS + BC250HSA_IB_PAD_DWORDS;
    uint32_t       cap = slot_dwords;

    if (max_ib_dwords != 0u && max_ib_dwords < cap) {
        cap = max_ib_dwords;
    }
    return (cap > reserve) ? (cap - reserve) : 0u;
}

static uint32_t batch_dword_cap(const struct bc250hsa_device* dev)
{
    return dword_cap_for(dev, dev->batch.max_ib_dwords);
}

bc250hsa_status bc250hsa_batch_submit_locked(struct bc250hsa_device* dev)
{
    bc250hsa_pm4_env    env;
    bc250hsa_pm4_writer w;
    bc250hsa_status     result;
    uint32_t*           slot_host;
    const uint32_t      slot = dev->batch_slot;

    if (!dev->batch_open) {
        return BC250HSA_OK;
    }
    slot_host = (uint32_t*)((uint8_t*)dev->ring.host + (size_t)slot * dev->ring_slot_bytes);
    fill_env(dev, &env, 0u, dev->batch_fence_value);
    bc250hsa_pm4_writer_init(&w, slot_host, dev->ring_slot_bytes / 4u, dev->batch_dwords);
    result = bc250hsa_pm4_ib_tail(&w, &env);
    if (result == BC250HSA_OK && w.overflow) {
        result = BC250HSA_ENOMEM;
    }
    if (result == BC250HSA_OK) {
        result = submit_ib(dev, slot, w.count, dev->batch_fence_value, dev->context, 0);
    }

    if (result == BC250HSA_OK) {
        if (dev->batch_count > 1u) {
            bc250hsa_count_add(BC250HSA_C_BATCHES_SUBMITTED, 1u);
            bc250hsa_count_add(BC250HSA_C_DISPATCHES_BATCHED, dev->batch_count);
        }
    } else {
        /* The dispatches of this buffer already have their fence value, and a caller
         * holds it. Nothing can deliver them now, and returning the failure to whoever
         * happens to call next would hide it, so the device is lost by name, and the
         * status says so too: a raw BC250HSA_EOS or BC250HSA_ENOMEM here would read as a
         * refusal a caller may work around (bc250hsa.h section 5.1). The raw status goes
         * to the log. */
        dev->slot_fence[slot] = 0u;
        bc250hsa_log(BC250HSA_LOG_ERROR, "the submission of a batched indirect buffer failed: %s",
                     bc250hsa_status_string(result));
        bc250hsa_mark_lost(dev, "the submission of a batched indirect buffer failed");
        result = BC250HSA_EDEVICELOST;
    }
    dev->batch_open = 0;
    dev->batch_count = 0;
    dev->batch_dwords = 0;
    journal_reset(dev);
    return result;
}

bc250hsa_status bc250hsa_flush(struct bc250hsa_device* dev, uint64_t* fence_value_out)
{
    bc250hsa_status result;

    if (dev == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!dev->submit_capable) {
        if (fence_value_out != NULL) {
            *fence_value_out = 0u;
        }
        return BC250HSA_OK;
    }
    bc250hsa_device_lock(dev);
    result = bc250hsa_batch_submit_locked(dev);
    if (fence_value_out != NULL) {
        *fence_value_out = dev->fence_last_submitted;
    }
    LeaveCriticalSection(&dev->lock);
    return result;
}

bc250hsa_status bc250hsa_batch_policy_set(struct bc250hsa_device* dev,
                                          const bc250hsa_batch_policy* policy)
{
    bc250hsa_status result;

    if (dev == NULL || policy == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!bc250hsa_struct_bytes_ok(policy->struct_bytes, sizeof(*policy))) {
        return BC250HSA_EINVAL;
    }
    if (policy->max_dispatches > BC250HSA_BATCH_DISPATCHES_MAX) {
        return BC250HSA_EINVAL;
    }
    bc250hsa_device_lock(dev);
    /* The policy of a dispatch is the policy its indirect buffer was opened with. */
    result = bc250hsa_batch_submit_locked(dev);
    if (result == BC250HSA_OK) {
        /* The whole policy is tested before any of it is the device's. A refusal that
         * left half of it applied would enable batching with a buffer too small for one
         * dispatch, and every later dispatch would answer BC250HSA_ENOMEM while the
         * caller had been told the policy was refused. */
        bc250hsa_batch_policy wanted = *policy;

        wanted.struct_bytes = (uint32_t)sizeof(wanted);
        if (wanted.max_dispatches == 0u) {
            wanted.max_dispatches = BC250HSA_BATCH_DISPATCHES_DEFAULT;
        }
        if (wanted.max_hold_us == 0u) {
            wanted.max_hold_us = BC250HSA_BATCH_HOLD_US_DEFAULT;
        }
        if (dword_cap_for(dev, wanted.max_ib_dwords) < BC250HSA_PM4_MAX_DWORDS) {
            result = BC250HSA_EINVAL;
        } else {
            dev->batch = wanted;
        }
    }
    LeaveCriticalSection(&dev->lock);
    return result;
}

bc250hsa_status bc250hsa_batch_policy_get(struct bc250hsa_device* dev,
                                          bc250hsa_batch_policy* out)
{
    if (dev == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!bc250hsa_struct_bytes_ok(out->struct_bytes, sizeof(*out))) {
        return BC250HSA_EINVAL;
    }
    bc250hsa_device_lock(dev);
    *out = dev->batch;
    out->struct_bytes = (uint32_t)sizeof(*out);
    LeaveCriticalSection(&dev->lock);
    return BC250HSA_OK;
}

/* One dispatch, submitted by itself: what build 1 did, and what batching off still
 * does. The caller holds dev->lock. */
static bc250hsa_status submit_one(struct bc250hsa_device* dev, const bc250hsa_dispatch* dispatch,
                                  const void* record, uint32_t record_bytes,
                                  uint64_t* fence_value_out)
{
    bc250hsa_pm4_env env;
    bc250hsa_status  result;
    uint32_t         slot = 0;
    uint32_t         dwords_written = 0;
    uint32_t*        slot_host;
    const uint64_t   fence_value = dev->fence_last_submitted + 1u;

    result = take_slot(dev, &slot);
    if (result != BC250HSA_OK) {
        return result;
    }
    slot_host = (uint32_t*)((uint8_t*)dev->ring.host + (size_t)slot * dev->ring_slot_bytes);
    fill_env(dev, &env, dispatch->flags, fence_value);
    if (dispatch->kernel->private_segment_bytes != 0u) {
        bc250hsa_scratch_plan scratch;
        bc250hsa_allocator allocator;
        result = bc250hsa_plan_scratch(dispatch->kernel->private_segment_bytes,
                                         dispatch->kernel->wave_size, &scratch);
        if (result != BC250HSA_OK) { return result; }
        result = bc250hsa_device_allocator(dev, &allocator);
        if (result != BC250HSA_OK) { return result; }
        /* The slot has retired and the device lock excludes a concurrent owner.
         * Re-check the removal sentinel before any replacement allocation is freed.
         * The allocation remains on the device list until retirement or teardown. */
        result = bc250hsa_prepare_scratch(&dev->slot_scratch[slot], dev->slot_fence[slot],
                                           bc250hsa_fence_read(dev), &scratch, &allocator);
        if (result != BC250HSA_OK) { return result; }
        env.scratch_va = dev->slot_scratch[slot].va;
        env.scratch_bytes = dev->slot_scratch[slot].bytes;
    }

    result = bc250hsa_pm4_build_dispatch(dispatch, &env, slot_host,
                                         dev->ring_slot_bytes / 4u, &dwords_written);
    if (result != BC250HSA_OK) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return result;
    }
    if (dwords_written > BC250HSA_PM4_MAX_DWORDS) {
        return BC250HSA_ENOMEM;
    }
    journal_reset(dev);
    journal_append(dev, record, record_bytes);
    result = submit_ib(dev, slot, dwords_written, fence_value, dev->context, 0);
    journal_reset(dev);
    if (result != BC250HSA_OK) {
        return result;
    }
    dev->fence_last_assigned = fence_value;
    *fence_value_out = fence_value;
    return BC250HSA_OK;
}

/* One dispatch appended to the indirect buffer this device is building. The caller holds
 * dev->lock. */
static bc250hsa_status submit_batched(struct bc250hsa_device* dev,
                                      const bc250hsa_dispatch* dispatch,
                                      const void* record, uint32_t record_bytes,
                                      uint64_t* fence_value_out)
{
    bc250hsa_pm4_env    env;
    bc250hsa_pm4_writer w;
    bc250hsa_status     result;
    uint32_t*           slot_host;
    uint32_t            tries;

    /* Two turns at most: the first may find the open buffer too full for this dispatch,
     * submit it and open another, and an empty buffer always has room for one dispatch
     * (bc250hsa_batch_policy_set refused a cap below BC250HSA_PM4_MAX_DWORDS). */
    for (tries = 0; tries < 2u; tries++) {
        if (dev->batch_open &&
            ((record != NULL) != (dev->journal_count != 0u) ||
             (record != NULL && (dev->journal_count >= BC250_HIP_JOURNAL_MAX_RECORDS ||
               record_bytes > BC250_HIP_JOURNAL_MAX_BYTES - dev->journal_bytes)))) {
            result = bc250hsa_batch_submit_locked(dev);
            if (result != BC250HSA_OK) { return result; }
        }
        if (!dev->batch_open) {
            uint32_t slot = 0;
            result = take_slot(dev, &slot);
            if (result != BC250HSA_OK) {
                return result;
            }
            dev->batch_slot = slot;
            dev->batch_fence_value = dev->fence_last_submitted + 1u;
            dev->batch_count = 0;
            journal_reset(dev);
            dev->batch_opened_us = bc250hsa_now_us();
            /* A new buffer knows nothing about the hardware's register state: another
             * context's work runs between two of our submissions. */
            memset(&dev->batch_state, 0, sizeof(dev->batch_state));
            slot_host =
                (uint32_t*)((uint8_t*)dev->ring.host + (size_t)slot * dev->ring_slot_bytes);
            fill_env(dev, &env, dispatch->flags, dev->batch_fence_value);
            bc250hsa_pm4_writer_init(&w, slot_host, dev->ring_slot_bytes / 4u, 0u);
            bc250hsa_pm4_ib_head(&w, &env);
            dev->batch_dwords = w.count;
            /* The slot must not be handed out again while this buffer is being
             * written. Its fence value cannot have retired, because nothing submitted
             * it yet, so the retire test of take_slot keeps the slot. */
            dev->slot_fence[slot] = dev->batch_fence_value;
            dev->batch_open = 1;
        }

        slot_host = (uint32_t*)((uint8_t*)dev->ring.host +
                                (size_t)dev->batch_slot * dev->ring_slot_bytes);
        fill_env(dev, &env, dispatch->flags, dev->batch_fence_value);
        bc250hsa_pm4_writer_init(&w, slot_host, batch_dword_cap(dev), dev->batch_dwords);
        result = bc250hsa_pm4_ib_append(&w, dispatch, &env, dev->batch_count == 0u,
                                        &dev->batch_state);
        if (result != BC250HSA_OK) {
            bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
            return result;
        }
        if (!w.overflow) {
            journal_append(dev, record, record_bytes);
            dev->batch_dwords = w.count;
            dev->batch_count++;
            *fence_value_out = dev->batch_fence_value;
            dev->fence_last_assigned = dev->batch_fence_value;
            break;
        }
        /* No room. Submit what is there and open another buffer. An empty buffer that
         * still has no room is a refusal and not a second turn. */
        if (dev->batch_count == 0u) {
            dev->batch_open = 0;
            dev->slot_fence[dev->batch_slot] = 0u;
            dev->batch_dwords = 0;
            journal_reset(dev);
            return BC250HSA_ENOMEM;
        }
        result = bc250hsa_batch_submit_locked(dev);
        if (result != BC250HSA_OK) {
            return result;
        }
    }

    /* The caps. A dispatch that arrives late is the last of its buffer, so work is
     * never held longer than max_hold_us past the moment another call arrives. */
    if (dev->batch_count >= dev->batch.max_dispatches ||
        bc250hsa_now_us() - dev->batch_opened_us >= (uint64_t)dev->batch.max_hold_us) {
        return bc250hsa_batch_submit_locked(dev);
    }
    return BC250HSA_OK;
}

static bc250hsa_status dispatch_submit(struct bc250hsa_device* dev,
                                        const bc250hsa_dispatch* dispatch,
                                        const void* record, uint32_t record_bytes,
                                        uint64_t* fence_value_out)
{
    bc250hsa_status result;

    if (dev == NULL || dispatch == NULL || fence_value_out == NULL) {
        return BC250HSA_EINVAL;
    }
    *fence_value_out = 0u;
    if (!dev->submit_capable) {
        return BC250HSA_EUNSUPPORTED;
    }
    if (dev->device_lost) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return BC250HSA_EDEVICELOST;
    }

    /* The local memory limit comes from the field this device read at open and not from
     * a property structure built per dispatch. */
    result = bc250hsa_pm4_check_dispatch(dispatch, dev->lds_bytes_per_workgroup);
    if (result != BC250HSA_OK) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return result;
    }

    bc250hsa_device_lock(dev);
    if (dev->closing || dev->device_lost) {
        LeaveCriticalSection(&dev->lock);
        return BC250HSA_EDEVICELOST;
    }
    /* Before a fence value is handed out or a batch opened: a copy whose wait failed may
     * still run on the copy context. */
    result = copy_exclusion_locked(dev, "a dispatch");
    if (result != BC250HSA_OK) {
        LeaveCriticalSection(&dev->lock);
        return result;
    }
    if (record == NULL && dev->journal_required) {
        LeaveCriticalSection(&dev->lock);
        return BC250HSA_EUNSUPPORTED;
    }
    if (record != NULL && dev->journal_upload == NULL) {
        dev->journal_upload = (uint8_t*)malloc(BC250_HIP_JOURNAL_MAX_BYTES);
        if (dev->journal_upload == NULL) {
            LeaveCriticalSection(&dev->lock);
            return BC250HSA_ENOMEM;
        }
    }
    if (record != NULL && !dev->journal_required) {
        result = bc250hsa_batch_submit_locked(dev);
        if (result != BC250HSA_OK) {
            LeaveCriticalSection(&dev->lock);
            return result;
        }
        dev->journal_required = 1;
    }
    if (dispatch->kernel->private_segment_bytes != 0u) {
        /* One fixed ring stride per IB; submit any promised batch first so fence
         * ordering remains monotonic. Scratch-free dispatches retain batching. */
        result = bc250hsa_batch_submit_locked(dev);
        if (result == BC250HSA_OK) {
            result = submit_one(dev, dispatch, record, record_bytes, fence_value_out);
        }
    } else if (dev->batch.enabled != 0u) {
        result = submit_batched(dev, dispatch, record, record_bytes, fence_value_out);
    } else {
        result = submit_one(dev, dispatch, record, record_bytes, fence_value_out);
    }
    if (result != BC250HSA_OK) { *fence_value_out = 0u; }
    LeaveCriticalSection(&dev->lock);
    return result;
}

bc250hsa_status bc250hsa_dispatch_submit(struct bc250hsa_device* dev,
                                         const bc250hsa_dispatch* dispatch,
                                         uint64_t* fence_value_out)
{
    return dispatch_submit(dev, dispatch, NULL, 0u, fence_value_out);
}

/* --------------------------------------------------------------------------------
 * The GPU copy and fill on the copy context (scratch/hip-gpu-copy/CONTRACT.md section 7)
 * ------------------------------------------------------------------------------ */

bc250hsa_status bc250hsa_wait_policy_set(struct bc250hsa_device* dev, uint32_t slice_ms,
                                         uint32_t total_ms)
{
    if (dev == NULL) {
        return BC250HSA_EINVAL;
    }
    bc250hsa_device_lock(dev);
    dev->wait_slice_ms = slice_ms;
    dev->wait_total_ms = total_ms;
    LeaveCriticalSection(&dev->lock);
    return BC250HSA_OK;
}

/* The bound of a wait this library performs for itself, which is the one place where it
 * needs one: a GPU copy returns after its own fence, so the wait is inside the library
 * and not in the caller. 0 and 0 take the library defaults, and layer 2 sets the
 * process's own numbers at device open (bc250hsa.h rule 6 keeps the policy there). */
bc250hsa_status bc250hsa_wait_internal(struct bc250hsa_device* dev, uint64_t value)
{
    return bc250hsa_wait(dev, value, dev->wait_slice_ms, dev->wait_total_ms);
}

/* The context a copy-only indirect buffer goes to: dev->copy_context, created on the first
 * one. The caller holds dev->lock.
 *
 * Why a context of its own and not dev->context (CONTRACT.md deviation 13.23). The first
 * recorded dispatch turns the journal of dev->context on in the kernel driver, and from
 * then on HipJournalBind refuses a buffer of that context that no uploaded record names.
 * A copy has no dispatch to record, and journal ABI 1 has no record for anything else
 * (Bc250HipRecordValid wants an entry address, a descriptor and a grid). A context that
 * never uploads keeps its journal off, which docs/design/hip-dispatch-journal.md states
 * for a standalone layer-1 client: "Standalone layer-1 clients can still use that API on
 * separate contexts". Both contexts belong to this device, so the buffer sees the same
 * address space, residency and monitored fence.
 *
 * A context the operating system would not create is said once, and every later copy is
 * refused with BC250HSA_EOS and nothing submitted, which layer 2 answers with the
 * host-processor path. */
static bc250hsa_status copy_context_locked(struct bc250hsa_device* dev, D3DKMT_HANDLE* out)
{
#ifdef BC250HSA_NEGATIVE_CONTROL_COPY_ON_MAIN_CONTEXT
    /* The compiled negative control of the lab plan: the copy goes to the main context,
     * which is what the build of 2026-10-11 did, and the journal check of submit_ib is
     * compiled out with it. Never a product build. */
    if (dev->copy_context_failed == 0) {
        dev->copy_context_failed = 2;
        bc250hsa_log(BC250HSA_LOG_WARN,
                     "NEGATIVE CONTROL BUILD: copy-only indirect buffers go to the main"
                     " context 0x%x", (unsigned)dev->context);
    }
    *out = dev->context;
    return BC250HSA_OK;
#else
    bc250hsa_status status;

    *out = 0u;
    if (dev->copy_context != 0u) {
        *out = dev->copy_context;
        return BC250HSA_OK;
    }
    if (dev->copy_context_failed) {
        return BC250HSA_EOS;
    }
    status = bc250hsa_context_create(dev, &dev->copy_context);
    if (status != BC250HSA_OK) {
        dev->copy_context = 0u;
        dev->copy_context_failed = 1;
        bc250hsa_log(BC250HSA_LOG_ERROR,
                     "the copy context could not be created (%s); no GPU copy or fill runs on"
                     " this device",
                     bc250hsa_status_string(status));
        return status;
    }
    bc250hsa_log(BC250HSA_LOG_INFO,
                 "fence check: copy context 0x%x created beside the main context 0x%x; both"
                 " signal fence handle 0x%x",
                 (unsigned)dev->copy_context, (unsigned)dev->context, (unsigned)dev->fence);
    *out = dev->copy_context;
    return BC250HSA_OK;
#endif
}

bc250hsa_status bc250hsa_copy_context_ready_locked(struct bc250hsa_device* dev)
{
    D3DKMT_HANDLE context;
    return copy_context_locked(dev, &context);
}

/* Builds the list into an indirect buffer of its own and submits it on the copy context,
 * with every earlier submission of this device retired. The caller holds dev->lock and
 * keeps it until the value retires (bc250hsa_copy_run_ops). */
static bc250hsa_status copy_submit_locked(struct bc250hsa_device* dev, const bc250hsa_copy_op* ops,
                                          uint32_t count, uint64_t* fence_value_out)
{
    bc250hsa_pm4_env    env;
    bc250hsa_pm4_writer w;
    bc250hsa_status     result;
    D3DKMT_HANDLE       context = 0u;
    uint32_t*           slot_host;
    uint32_t            needed = 0u;
    uint32_t            slot = 0u;
    uint32_t            i;
    uint64_t            value;

    for (i = 0; i < count; i++) {
        const uint32_t dwords = bc250hsa_pm4_copy_dwords(ops[i].bytes);
        if (dwords == 0u || needed > 0xFFFFFFFFu - dwords) {
            return BC250HSA_EINVAL;
        }
        needed += dwords;
    }
    /* A list no empty buffer could hold is a refusal and not an endless retry. The caller
     * splits it. The longest list this build sends is a fill's three parts, which is 21
     * dwords against a cap of thousands, so no caller of this build splits anything; the
     * check is here for the list a two-dimensional copy would send
     * (CONTRACT.md deviation 13.10). */
    if (needed > batch_dword_cap(dev)) {
        return BC250HSA_ENOMEM;
    }
    result = copy_context_locked(dev, &context);
    if (result != BC250HSA_OK) {
        return result;
    }
    /* An earlier copy whose wait failed may still run, on this same context but with no
     * order to the main context's work that this copy now flushes. */
    result = copy_exclusion_locked(dev, "a copy");
    if (result != BC250HSA_OK) {
        return result;
    }

    /* The order of the one fence. The scheduler orders the buffers of one context and
     * nothing across two, while both write absolute values into the same fence. A copy
     * that ran before an earlier buffer of the main context would write a value the
     * earlier one has not reached, and a wait for that earlier value would wake on it;
     * a dispatch that ran before the copy would do the same to the copy's wait. So the
     * open batch goes out, everything submitted retires, and only then the copy goes,
     * and the caller keeps dev->lock until it retired too. Layer 2 has already waited
     * for the work this copy depends on (hip_memory.cpp, wait_for_stream and
     * wait_for_device), so for the legacy stream this wait finds the value there. */
    if (dev->batch_open) {
        bc250hsa_count_add(BC250HSA_C_GPU_COPY_FORCED_FLUSHES, 1u);
    }
    result = bc250hsa_batch_submit_locked(dev);
    if (result != BC250HSA_OK) {
        return result;
    }
    if (dev->fence_last_submitted != 0u) {
        result = bc250hsa_wait_internal(dev, dev->fence_last_submitted);
        if (result != BC250HSA_OK) {
            return result;
        }
    }

    result = take_slot(dev, &slot);
    if (result != BC250HSA_OK) {
        return result;
    }
    value = dev->fence_last_submitted + 1u;
    slot_host = (uint32_t*)((uint8_t*)dev->ring.host + (size_t)slot * dev->ring_slot_bytes);
    fill_env(dev, &env, 0u, value);
    bc250hsa_pm4_writer_init(&w, slot_host, dev->ring_slot_bytes / 4u, 0u);
    bc250hsa_pm4_ib_head(&w, &env);
    result = bc250hsa_pm4_append_ops(&w, ops, count);
    if (result == BC250HSA_OK) {
        result = bc250hsa_pm4_ib_tail(&w, &env);
    }
    if (result == BC250HSA_OK && w.overflow) {
        result = BC250HSA_ENOMEM;
    }
    if (result != BC250HSA_OK) {
        return result;
    }
    result = submit_ib(dev, slot, w.count, value, context, 1);
    if (result != BC250HSA_OK) {
        /* Nothing holds this value and the slot keeps its old retired one, so a refusal
         * costs nothing else. A failed SubmitCommand is a device the operating system
         * no longer takes work from, and it is said as a loss by name (bc250hsa.h section
         * 5.1). The journal check of submit_ib is a refusal with nothing submitted. */
        if (result != BC250HSA_EUNSUPPORTED) {
            bc250hsa_mark_lost(dev, "the submission of a copy failed");
        }
        return result;
    }
    dev->fence_last_assigned = value;
    /* Until bc250hsa_copy_run_ops sees it retire. */
    dev->copy_unretired = value;
    bc250hsa_count_add(BC250HSA_C_GPU_COPY_PACKETS, needed / BC250HSA_DMA_DATA_DWORDS);
    *fence_value_out = value;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_copy_run_ops(struct bc250hsa_device* dev, const bc250hsa_copy_op* ops,
                                      uint32_t count, bc250hsa_op_receipt* receipt)
{
    bc250hsa_status result;
    uint64_t        fence_value = 0u;

    if (receipt == NULL) {
        return BC250HSA_EINVAL;
    }
    receipt->state = BC250HSA_OP_NOT_SUBMITTED;
    receipt->fence_value = 0u;
    if (dev == NULL || ops == NULL || count == 0u) {
        return BC250HSA_EINVAL;
    }
    if (!dev->submit_capable) {
        return BC250HSA_EUNSUPPORTED;
    }
    bc250hsa_copy_lock_enter(dev);
    if (dev->device_lost || dev->closing) {
        bc250hsa_copy_lock_leave(dev);
        return BC250HSA_EDEVICELOST;
    }
    result = copy_submit_locked(dev, ops, count, &fence_value);
    if (result == BC250HSA_OK) {
        /* From here on the device has the list. A wait that ends in anything but success
         * has not established that it retired, whatever the reason: a timeout, a removal,
         * an event that Windows would not create or register. The caller keeps every range
         * the list names until fence_value retires (CONTRACT.md section 7, BD-110). The
         * wait is inside the lock, for the order of the fence (copy_submit_locked). */
        receipt->state = BC250HSA_OP_SUBMITTED;
        receipt->fence_value = fence_value;
        result = bc250hsa_wait_internal(dev, fence_value);
        if (result == BC250HSA_OK) {
            receipt->state = BC250HSA_OP_RETIRED;
            dev->copy_unretired = 0u;
        } else {
            dev->copy_unretired_status = result;
            if (!dev->device_lost) {
                /* The lock opens now, and dev->copy_unretired holds every later submission
                 * (copy_exclusion_locked) and every host-processor copy of the caller
                 * (bc250hsa_copy_unretired). */
                bc250hsa_count_add(BC250HSA_C_GPU_COPY_UNRETIRED, 1u);
                bc250hsa_log(BC250HSA_LOG_ERROR,
                             "copy exclusion: the copy with fence value %llu did not retire (%s);"
                             " every later submission of this device waits for it first and is"
                             " refused while it has not retired",
                             (unsigned long long)fence_value, bc250hsa_status_string(result));
            }
        }
    }
    bc250hsa_copy_lock_leave(dev);
    /* A refused list or a failed submission leaves NOT_SUBMITTED. A failed submission has
     * marked the device lost, which the status now says. */
    return bc250hsa_lost_status(dev, result);
}

bc250hsa_status bc250hsa_dispatch_submit_recorded(struct bc250hsa_device* dev,
                                                  const bc250hsa_dispatch* dispatch,
                                                  const void* record,
                                                  uint32_t record_bytes,
                                                  uint64_t* fence_value_out)
{
    BC250_HIP_DISPATCH_RECORD header;
    if (fence_value_out != NULL) { *fence_value_out = 0u; }
    if (dispatch == NULL || dispatch->kernel == NULL || record == NULL ||
        !Bc250HipRecordValid(record, record_bytes))
        return BC250HSA_EINVAL;
    memcpy(&header, record, sizeof(header));
    /* The runtime retains the semantic metadata. This bridge checks that the
     * record belongs to the exact dispatch whose PM4 it will build. */
    if (header.Bytes != record_bytes || header.DispatchId == 0u ||
        header.EntryVa != dispatch->kernel->entry_va ||
        header.DescriptorVa != dispatch->kernel->descriptor_va ||
        header.KernargVa != dispatch->kernarg_va ||
        header.DynamicLdsBytes != dispatch->launch.dynamic_group_bytes ||
        memcmp(header.Grid, dispatch->launch.grid, sizeof(header.Grid)) != 0 ||
        memcmp(header.Block, dispatch->launch.block, sizeof(header.Block)) != 0)
        return BC250HSA_EINVAL;
    return dispatch_submit(dev, dispatch, record, record_bytes, fence_value_out);
}
