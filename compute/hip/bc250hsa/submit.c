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
    return (dev == NULL) ? 0u : dev->fence_last_submitted;
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

bc250hsa_status bc250hsa_dispatch_submit(struct bc250hsa_device* dev,
                                         const bc250hsa_dispatch* dispatch,
                                         uint64_t* fence_value_out)
{
    bc250hsa_pm4_env            env;
    struct bc250hsa_submit_blob blob;
    D3DKMT_SUBMITCOMMAND        command;
    bc250hsa_status             result;
    NTSTATUS                    status;
    uint32_t                    slot = 0;
    uint32_t                    dwords_written = 0;
    uint64_t                    fence_value;
    uint64_t                    slot_va;
    uint32_t*                   slot_host;
    bc250hsa_props              props;

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

    props.struct_bytes = (uint32_t)sizeof(props);
    result = bc250hsa_props_read(dev, &props);
    result = bc250hsa_pm4_check_dispatch(
        dispatch, (result == BC250HSA_OK) ? props.lds_bytes_per_workgroup : 0u);
    if (result != BC250HSA_OK) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        return result;
    }

    EnterCriticalSection(&dev->lock);

    result = take_slot(dev, &slot);
    if (result != BC250HSA_OK) {
        goto done;
    }
    slot_va = dev->ring.va + (uint64_t)slot * dev->ring_slot_bytes;
    slot_host = (uint32_t*)((uint8_t*)dev->ring.host + (size_t)slot * dev->ring_slot_bytes);
    fence_value = dev->fence_last_submitted + 1u;

    memset(&env, 0, sizeof(env));
    env.struct_bytes = (uint32_t)sizeof(env);
    /* Node 0 is the graphics ring (fact M50: this part has no compute IP). */
    env.flags = dispatch->flags | BC250HSA_DISPATCH_GFX_RING;
    env.fence_va = dev->fence_gpu_va;
    env.fence_value = fence_value;
    memcpy(env.private_segment_rsrc, dev->private_segment_rsrc,
           sizeof(env.private_segment_rsrc));
    env.ib_pad_dwords = BC250HSA_IB_PAD_DWORDS;

    result = bc250hsa_pm4_build_dispatch(dispatch, &env, slot_host,
                                         dev->ring_slot_bytes / 4u, &dwords_written);
    if (result != BC250HSA_OK) {
        bc250hsa_count_add(BC250HSA_C_SUBMISSIONS_REFUSED, 1u);
        goto done;
    }
    if (dwords_written > BC250HSA_PM4_MAX_DWORDS) {
        result = BC250HSA_ENOMEM;
        goto done;
    }
    memcpy(dev->last_ib, slot_host, (size_t)dwords_written * 4u);
    dev->last_ib_dwords = dwords_written;

    memset(&blob, 0, sizeof(blob));
    blob.magic = BC250HSA_SUBMIT_MAGIC;
    blob.version = BC250HSA_SUBMIT_VERSION;
    blob.size = BC250HSA_SUBMIT_PREFIX_BYTES + (uint32_t)sizeof(blob.ib[0]);
    blob.ip_type = BC250HSA_IP_GFX;
    blob.num_ibs = 1u;
    blob.fence_va = dev->fence_gpu_va;
    blob.fence_value = fence_value;
    blob.ib[0].va_start = slot_va;
    blob.ib[0].ib_bytes = dwords_written * 4u;
    blob.ib[0].ip_type = BC250HSA_IP_GFX;

    memset(&command, 0, sizeof(command));
    command.Commands = slot_va;
    command.CommandLength = dwords_written * 4u;
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
        result = bc250hsa_os_status("SubmitCommand", status);
        goto done;
    }

    dev->fence_last_submitted = fence_value;
    dev->slot_fence[slot] = fence_value;
    bc250hsa_count_add(BC250HSA_C_SUBMISSIONS, 1u);
    *fence_value_out = fence_value;
    result = BC250HSA_OK;

done:
    LeaveCriticalSection(&dev->lock);
    return result;
}
