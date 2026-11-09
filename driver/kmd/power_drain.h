/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_POWER_DRAIN_H
#define BC250_POWER_DRAIN_H
// BD-188: why the software half of a retained way down (wddm.c WddmSuspendRetained) found WDDM work outstanding.
// The way down waits up to BC250_WDDM_SUSPEND_DRAIN_MS, in 1 ms steps, for that work to complete on its normal path
// (the interrupt, the DPC and the OS report), and logs this mask when it still refuses. It makes up no completion.
#define BC250_POWER_BUSY_GFX_HW        0x0001u /* node 0: a job on the ring (HwPending) */
#define BC250_POWER_BUSY_GFX_DEFERRED  0x0002u /* node 0: a fence held for the report (DeferredValid) */
#define BC250_POWER_BUSY_COMPUTE_HW    0x0004u /* node 2: a job on the ring */
#define BC250_POWER_BUSY_COMPUTE_DEFER 0x0008u /* node 2: a fence held for the report */
#define BC250_POWER_BUSY_PAGING_HW     0x0010u /* node 1: a paging packet on SDMA0 */
#define BC250_POWER_BUSY_PAGING_QUEUED 0x0020u /* node 1: a paging job queued in the driver */
#define BC250_POWER_BUSY_REPORT        0x0040u /* a completion report is being published */
#define BC250_POWER_BUSY_SUBMISSIONS   0x0080u /* a submission is still inside the driver */
#define BC250_POWER_BUSY_COMPLETION    0x0100u /* a completion waits for its report */
#define BC250_POWER_BUSY_PREEMPTION    0x0200u /* a preemption waits for its report */
#define BC250_POWER_BUSY_REFUSAL       0x0400u /* a refused or rejected submission waits for its report */
#define BC250_POWER_BUSY_WATCHDOG      0x0800u /* a node watchdog reported a fault: only a reset ends it */
#define BC250_POWER_BUSY_STOPPING      0x1000u /* the device stop has begun */
// The bound of the wait. Long enough for the interrupt and DPC path of a job that is about to finish (a frame is
// about 16 ms), short against the power IRP budget. A watchdog fault is not waited for: no completion can come.
#define BC250_WDDM_SUSPEND_DRAIN_MS 500u
static __inline int Bc250PowerDrainWorthWaiting(unsigned busy)
{
    return busy != 0 && (busy & (BC250_POWER_BUSY_WATCHDOG | BC250_POWER_BUSY_STOPPING)) == 0;
}
#endif
