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

bc250hsa_status bc250hsa_wait(struct bc250hsa_device* dev, uint64_t value, uint32_t slice_ms,
                              uint32_t total_ms)
{
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;
    HANDLE                                     event;
    NTSTATUS                                   status;
    uint64_t                                   waited_ms = 0;
    uint64_t                                   observed;
    bc250hsa_status                            result = BC250HSA_ETIMEOUT;

    if (dev == NULL) {
        return BC250HSA_EINVAL;
    }
    if (!dev->submit_capable) {
        return BC250HSA_EUNSUPPORTED;
    }
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
        EnterCriticalSection(&dev->lock);
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
        bc250hsa_mark_lost(dev, "the fence mapping reads UINT64_MAX");
        return BC250HSA_EDEVICELOST;
    }
    if (observed >= value) {
        bc250hsa_count_add(BC250HSA_C_WAITS_FAST, 1u);
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
        if (observed == UINT64_MAX || !bc250hsa_device_executing(dev)) {
            bc250hsa_mark_lost(dev, "the fence or the execution state reports a loss");
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
            bc250hsa_mark_lost(dev, "the fence mapping reads UINT64_MAX after the wait");
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

/* A free command ring slot. A slot is free when the fence value it was last used with
 * has retired. Nothing is ever overwritten under the command processor. */
static bc250hsa_status take_slot(struct bc250hsa_device* dev, uint32_t* slot_out)
{
    uint32_t tried;

    for (tried = 0; tried < dev->ring_slots; tried++) {
        const uint32_t slot = (dev->next_slot + tried) % dev->ring_slots;
        if (dev->slot_fence[slot] == 0u || bc250hsa_fence_read(dev) >= dev->slot_fence[slot]) {
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

/* Hands one finished indirect buffer to the kernel driver. The caller holds dev->lock
 * and has written dwords dwords into the slot. The private blob names one indirect
 * buffer, which is the one shape the kernel driver runs (driver/kmd/wddm.c refuses a
 * blob whose single_ib is 0). */
static bc250hsa_status submit_ib(struct bc250hsa_device* dev, uint32_t slot, uint32_t dwords,
                                 uint64_t fence_value)
{
    struct bc250hsa_submit_blob blob;
    D3DKMT_SUBMITCOMMAND        command;
    NTSTATUS                    status;
    const uint64_t              slot_va = dev->ring.va + (uint64_t)slot * dev->ring_slot_bytes;
    const uint32_t*             slot_host =
        (const uint32_t*)((const uint8_t*)dev->ring.host + (size_t)slot * dev->ring_slot_bytes);

    if (dwords > dev->last_ib_capacity) {
        return BC250HSA_ENOMEM;
    }
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
    command.BroadcastContext[0] = dev->context;

    /* Host stores sit in the write buffers until a fence. The system call drains
     * them too; this makes the order obvious. */
    bc250hsa_write_barrier();
    status = D3DKMTSubmitCommand(&command);
    if (!NT_SUCCESS(status)) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return bc250hsa_os_status("SubmitCommand", status);
    }

    dev->fence_last_submitted = fence_value;
    dev->slot_fence[slot] = fence_value;
    bc250hsa_count_add(BC250HSA_C_SUBMISSIONS, 1u);
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
        result = submit_ib(dev, slot, w.count, dev->batch_fence_value);
    }

    if (result == BC250HSA_OK) {
        if (dev->batch_count > 1u) {
            bc250hsa_count_add(BC250HSA_C_BATCHES_SUBMITTED, 1u);
            bc250hsa_count_add(BC250HSA_C_DISPATCHES_BATCHED, dev->batch_count);
        }
    } else {
        /* The dispatches of this buffer already have their fence value, and a caller
         * holds it. Nothing can deliver them now, and returning the failure to whoever
         * happens to call next would hide it, so the device is lost by name. */
        dev->slot_fence[slot] = 0u;
        bc250hsa_mark_lost(dev, "the submission of a batched indirect buffer failed");
    }
    dev->batch_open = 0;
    dev->batch_count = 0;
    dev->batch_dwords = 0;
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
    EnterCriticalSection(&dev->lock);
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
    EnterCriticalSection(&dev->lock);
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
    EnterCriticalSection(&dev->lock);
    *out = dev->batch;
    out->struct_bytes = (uint32_t)sizeof(*out);
    LeaveCriticalSection(&dev->lock);
    return BC250HSA_OK;
}

/* One dispatch, submitted by itself: what build 1 did, and what batching off still
 * does. The caller holds dev->lock. */
static bc250hsa_status submit_one(struct bc250hsa_device* dev, const bc250hsa_dispatch* dispatch,
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

    result = bc250hsa_pm4_build_dispatch(dispatch, &env, slot_host,
                                         dev->ring_slot_bytes / 4u, &dwords_written);
    if (result != BC250HSA_OK) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return result;
    }
    if (dwords_written > BC250HSA_PM4_MAX_DWORDS) {
        return BC250HSA_ENOMEM;
    }
    result = submit_ib(dev, slot, dwords_written, fence_value);
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
        if (!dev->batch_open) {
            uint32_t slot = 0;
            result = take_slot(dev, &slot);
            if (result != BC250HSA_OK) {
                return result;
            }
            dev->batch_slot = slot;
            dev->batch_fence_value = dev->fence_last_submitted + 1u;
            dev->batch_count = 0;
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

bc250hsa_status bc250hsa_dispatch_submit(struct bc250hsa_device* dev,
                                         const bc250hsa_dispatch* dispatch,
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

    EnterCriticalSection(&dev->lock);
    if (dev->batch.enabled != 0u) {
        result = submit_batched(dev, dispatch, fence_value_out);
    } else {
        result = submit_one(dev, dispatch, fence_value_out);
    }
    LeaveCriticalSection(&dev->lock);
    return result;
}
