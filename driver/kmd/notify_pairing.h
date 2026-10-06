/* notify_pairing.h - who pairs a DxgkCbNotifyInterrupt report with the DPC-level notification dxgkrnl waits for.
 *
 * The contract (ref/ddi-display/d3dkmddi.md:2392, DXGKCB_NOTIFY_DPC remarks): "The display miniport driver's DPC
 * callback routine calls DXGKCB_NOTIFY_DPC to inform the GPU scheduler about an update to a fence". The
 * DXGKCB_NOTIFY_INTERRUPT page gives the recipe from an ISR ("after the driver calls DXGKCB_NOTIFY_INTERRUPT but
 * before the driver exits its ISR, the driver must queue a DPC"); d3dkmddi.md:12801 says the same for one
 * specific case only, the DIRQL SetVidPnSourceAddress CRTC_VSYNC report, so the general statement is :2392.
 *
 * What this driver does on the completion path, read off the code (pnp.c Bc250DpcRoutine, wddm.c WddmGpuFence,
 * WddmQueueReport, WddmReport, WddmReportDpcPublish, WddmDpc):
 *
 *   device pass   : dxgkrnl's DPC calls Bc250DpcRoutine. WddmGpuFence reads the fence page, records any
 *                   retirement and queues our own ReportDpc. Then WddmDpc calls DxgkCbNotifyDpc - with nothing
 *                   of the completion path reported yet in this pass.
 *   report pass   : ReportDpc runs WddmReportDpcPublish, which calls DxgkCbNotifyInterrupt (at DIRQL, through
 *                   DxgkCbSynchronizeExecution) and then DxgkCbQueueDpc.
 *   device pass   : that DxgkCbQueueDpc brings us back into Bc250DpcRoutine, whose WddmDpc finally pairs the
 *                   report of the previous pass.
 *
 * So every completion's notification reaches the scheduler one DPC hop after the report it belongs to. The hop
 * measured 12 to 20 us on the lab. WHAT IT IS WORTH, said exactly, because the first draft of this header got it
 * wrong: RotTR session 418 retired one packet per report in 100 % of 19694 boundaries over 105 s, which is 187
 * completions a second, not the 1268 submissions a second the submit path counts. Saving 15 us on each of them is
 * about 0.03 ms of a 70 Hz frame. It is NOT the cause of the 8 ms VSync-ended stalls: C48's own kill rule killed
 * that reading (scratch/m15/etw/c48/c48-result.json - the scheduler worker wakes a median 19 us after the
 * completion, runs its passes and refuses, with ReadyNodeSwMapBits bit 0 clear in 418 of 418). The notification
 * was there; the context was not ready. What remains here is a real deviation from the contract text with a small
 * measured cost, and an instrument that prices it - no more than that.
 *
 * The fix (gate NotifyDpcInReport, default on) calls DxgkCbNotifyDpc at the end of the report pass itself, which
 * is what the contract text describes, and then the DxgkCbQueueDpc whose only job was to bring that call about is
 * not made either: with the gate open a completion costs one notification instead of one notification plus an
 * otherwise empty dxgkrnl DPC. Reports made from a DDI or from a VSync path still queue it, because they have no
 * report pass behind them.
 *
 * The VSync reports are counted apart. They are DxgkCbNotifyInterrupt calls too, so a notification that carries
 * one is not idle - counting only the completion path made "notifications carried no report" wrong by about 62 a
 * second and left the flip path's own pairing unwatched.
 *
 * This header is the decision model of both shapes: no kernel call, no state of ours, so a host test can drive
 * the real order of passes and show what each one leaves behind. It is not a simulation of dxgkrnl. The caller
 * serializes it (wddm.c holds Lock over these counters), because two DPCs on two processors reach them.
 */
#ifndef BC250_NOTIFY_PAIRING_H
#define BC250_NOTIFY_PAIRING_H

typedef struct _BC250_NOTIFY_PAIRING {
    unsigned long Reports;       /* completion/preemption DxgkCbNotifyInterrupt calls that reached dxgkrnl */
    unsigned long Pairings;      /* DxgkCbNotifyDpc calls that carried at least one unpaired report */
    unsigned long SamePass;      /* reports paired inside the pass that made them: the contract's shape */
    unsigned long Deferred;      /* reports whose pairing needed a later pass: the hop */
    unsigned long Unpaired;      /* reports still waiting for a DxgkCbNotifyDpc right now */
    unsigned long MaxUnpaired;   /* the worst backlog reached */
    unsigned long IdleNotifies;  /* DxgkCbNotifyDpc calls with nothing of either kind to pair */
    unsigned long VsyncReports;  /* CRTC_VSYNC DxgkCbNotifyInterrupt calls: the flip path's own reports */
    unsigned long VsyncPending;  /* of them, the ones still waiting for a DxgkCbNotifyDpc */
    unsigned long MaxVsyncPending;
    unsigned long Contended;     /* notifications handed to DxgkCbQueueDpc because another CPU held the call */
} BC250_NOTIFY_PAIRING;

static __inline void Bc250NotifyPairingReset(BC250_NOTIFY_PAIRING* State)
{
    State->Reports = State->Pairings = State->SamePass = State->Deferred = 0u;
    State->Unpaired = State->MaxUnpaired = State->IdleNotifies = 0u;
    State->VsyncReports = State->VsyncPending = State->MaxVsyncPending = State->Contended = 0u;
}

/* WddmReport returned TRUE for a completion or a preemption: the report has reached dxgkrnl at DIRQL and now owes
 * a DPC-level notification. A report that failed must not be counted - it never reached dxgkrnl. */
static __inline void Bc250NotifyPairingReport(BC250_NOTIFY_PAIRING* State)
{
    State->Reports++;
    State->Unpaired++;
    if (State->Unpaired > State->MaxUnpaired) State->MaxUnpaired = State->Unpaired;
}

/* The same, for a CRTC_VSYNC report. Kept on its own counters: the flip path's pairing is a correctness question
 * of its own, and a notification that carries one of these is not an idle notification. */
static __inline void Bc250NotifyPairingVsyncReport(BC250_NOTIFY_PAIRING* State)
{
    State->VsyncReports++;
    State->VsyncPending++;
    if (State->VsyncPending > State->MaxVsyncPending) State->MaxVsyncPending = State->VsyncPending;
}

/* DxgkCbNotifyDpc. Returns the number of completion/preemption reports it paired, 0 when it had none to carry.
 * SamePass classifies only those: a VSync report has no such classification here, because the two paths that
 * make one are a DPC of their own (the software timer) and the device pass itself (the hardware vblank). */
static __inline unsigned long Bc250NotifyPairingNotifyDpc(BC250_NOTIFY_PAIRING* State, int SamePass)
{
    unsigned long paired = State->Unpaired;

    if (paired == 0u && State->VsyncPending == 0u) { State->IdleNotifies++; return 0u; }
    State->Unpaired = 0u;
    State->VsyncPending = 0u;
    State->Pairings++;
    if (paired != 0u) { if (SamePass) State->SamePass += paired; else State->Deferred += paired; }
    return paired;
}

/* A notification that found another processor inside DxgkCbNotifyDpc for this adapter and handed the work to
 * DxgkCbQueueDpc instead. Nothing is paired, nothing is lost: the queued dxgkrnl DPC pairs it next. */
static __inline void Bc250NotifyPairingContend(BC250_NOTIFY_PAIRING* State)
{
    State->Contended++;
}

/* One Bc250DpcRoutine. WddmDcnVsync runs just before WddmDpc in it, so a pass that acknowledged a vblank made
 * that report in this very pass - VsyncReported says whether it did. Nothing of the completion path is reported
 * here, so a completion pairing from this pass is always a deferred one. */
static __inline void Bc250NotifyPairingDevicePass(BC250_NOTIFY_PAIRING* State, int VsyncReported)
{
    if (VsyncReported) Bc250NotifyPairingVsyncReport(State);
    (void)Bc250NotifyPairingNotifyDpc(State, 0);
}

/* One WddmReportDpcPublish over Nodes nodes that had something to report. Gate is NotifyDpcInReport. */
static __inline void Bc250NotifyPairingReportPass(BC250_NOTIFY_PAIRING* State, unsigned long Nodes, int Gate)
{
    unsigned long i;

    for (i = 0; i < Nodes; i++) Bc250NotifyPairingReport(State);
    if (Gate && Nodes != 0u) (void)Bc250NotifyPairingNotifyDpc(State, 1);
}

/* One hardware completion end to end, as the driver really runs it: the device pass that read the fence, the
 * report pass it queued, and - with the gate closed - the device pass that the report's own DxgkCbQueueDpc brings
 * back. With the gate open that third pass is not asked for at all, which is where the second notification of
 * every completion goes away. */
static __inline void Bc250NotifyPairingCompletion(BC250_NOTIFY_PAIRING* State, int Gate)
{
    Bc250NotifyPairingDevicePass(State, 0);
    Bc250NotifyPairingReportPass(State, 1u, Gate);
    if (!Gate) Bc250NotifyPairingDevicePass(State, 0);
}

/* The completion that opens a long stall: the fence is read, the report is made, and no further DPC of ours is
 * queued before the next unrelated interrupt. Returns non-zero when the scheduler is left holding an unpaired
 * report over that whole interval. The stall itself is not caused by this (see the note at the top of the file);
 * this is only about who owes whom a notification while it lasts. */
static __inline int Bc250NotifyPairingStallHead(BC250_NOTIFY_PAIRING* State, int Gate)
{
    Bc250NotifyPairingDevicePass(State, 0);
    Bc250NotifyPairingReportPass(State, 1u, Gate);
    return State->Unpaired != 0u;
}

#endif /* BC250_NOTIFY_PAIRING_H */
