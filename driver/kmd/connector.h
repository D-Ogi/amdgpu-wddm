// Windows lifetime owner for the single connector. Included after WDK headers.
#pragma once
#include "../shim/bc250_sink.h"
#include "../shim/bc250_hpd.h"
#include "connector_diag.h"
#include "connector_handoff.h"
#include "dcn_overlay_input.h"    // BC250_OVERLAY_DESCRIPTION and BC250_LINK_OVERLAY_CHECK of the Check3 preflight

struct _BC250_DEVICE;

// BD-341: what Windows was told about the child, and when. The log ring keeps the first 256 lines of a load and
// wraps the rest, so the first child events of a start are kept here and printed again by the summary and the stop.
#define BC250_CONNECTOR_CHILD_EVENTS 8u
#define BC250_CHILD_EVENT_RELATIONS 1u      // QueryChildRelations: Connected holds the HPD awareness value
#define BC250_CHILD_EVENT_STATUS 2u         // QueryChildStatus: the connection answer
#define BC250_CHILD_EVENT_INDICATE 3u       // DxgkCbIndicateChildStatus: the value and its status
#define BC250_CHILD_EVENT_WORKER 4u         // the end of one hotplug worker run: Connected is what it publishes
typedef struct _BC250_CONNECTOR_CHILD_EVENT {
    ULONG Ms;                       // after the connector start
    UCHAR Kind, Enabled, Connected, Suspended;
    ULONG Generation, Published;    // the current generation, and that of the published snapshot (0: none)
    LONG Sense;                     // the HPD sense the worker read; -1 when the event read none
    NTSTATUS Status;
} BC250_CONNECTOR_CHILD_EVENT;

typedef struct _BC250_CONNECTOR_SNAPSHOT {
    volatile LONG References;
    ULONG Generation;
    BC250_SINK Sink;                 // immutable after publication
} BC250_CONNECTOR_SNAPSHOT;

typedef struct _BC250_CONNECTOR {
    KMUTEX Operation;               // PASSIVE owner; never raises IRQL for ATOM
    KSPIN_LOCK Lock;                // queue and snapshot pointer only, not ISR
    KEVENT Idle;
    KTIMER Debounce;
    KDPC DebounceDpc;
    PIO_WORKITEM WorkItem;
    BC250_CONNECTOR_SNAPSHOT* Snapshot;
    BC250_DISP_IO HpdIo, DiscoveryIo;
    BC250_HPD Hpd;
    volatile LONG Running, Enabled, Generation, Pending, RxPending;
    volatile LONG Suspended, Restoring;
    LONG Queued;
    ULONG OperationGeneration;
    // BD-195: the generation whose disable failed, and its worker re-arms (at
    // most CONNECTOR_DISABLE_REARMS). Operation held. DisableSpent counts the
    // generations that used up their re-arms (ConnectorDisableNext).
    ULONG DisableGeneration, DisableRearms;
    volatile LONG DisableSpent;
    volatile LONG Interrupts, RxInterrupts, Workers, Discoveries, Publications;
    volatile LONG Refusals, Notifications, NotifyFailures, Stale, Rearms;
    volatile LONG DiscoveryRetries;     // BD-195: discoveries repeated for one generation (CONNECTOR_DISCOVERY_TRIES)
    NTSTATUS LastStatus;
    BC250_CONNECTOR_OPEN_DIAG FirstOpen;
    BC250_CONNECTOR_TARGET_DIAG FirstAttempt;
    BC250_CONNECTOR_FAILURE_DIAG FirstFailure;
    BC250_CONNECTOR_RECOVERY_DIAG FirstRecovery;
    ULONG StartKind, StartAdapter, StartLimit, StartTargets, StartAccepted;
    // 1 when the start's refusal left the hardware as the firmware had it: it
    // wrote nothing, or every restore of what it wrote succeeded. A planning
    // refusal qualifies; a failed AUX restore or HPD disarm does not (BD-169).
    ULONG StartPreserved;
    NTSTATUS StartStatus;
    // BD-341: the first child events of this start, the latest one, and the counts (ConnectorChildRecord).
    ULONGLONG ChildEpoch;
    BC250_CONNECTOR_CHILD_EVENT ChildEvents[BC250_CONNECTOR_CHILD_EVENTS], ChildLast;
    volatile LONG ChildEventCount, ChildStatusCalls, ChildStatusDisconnected;
} BC250_CONNECTOR;

void ConnectorInitialize(struct _BC250_DEVICE* Device);
NTSTATUS ConnectorStart(struct _BC250_DEVICE* Device);
// TRUE when a ConnectorStart refusal leaves a display the static inherited path
// can present, so the adapter start continues (BD-169). PASSIVE, no lock.
BOOLEAN ConnectorRefusalKeepsDisplay(const struct _BC250_DEVICE* Device);
// TRUE when every cleanup of a refused start succeeded: the HPD disarm, the
// caller's release and the terminal release ConnectorLinkPrepare did itself.
BOOLEAN ConnectorRefusalPreserved(const struct _BC250_DEVICE* Device, NTSTATUS Hpd, NTSTATUS Release);
// PASSIVE, before removing MMIO. No operation/modeset lock held by caller.
NTSTATUS ConnectorStop(struct _BC250_DEVICE* Device);
NTSTATUS ConnectorSuspend(struct _BC250_DEVICE* Device);
NTSTATUS ConnectorResumeDisplay(struct _BC250_DEVICE* Device);
NTSTATUS ConnectorResumeNotifications(struct _BC250_DEVICE* Device);
void ConnectorDestroy(struct _BC250_DEVICE* Device);
NTSTATUS ConnectorBufferPrepare(struct _BC250_DEVICE* Device);
void ConnectorBufferRelease(struct _BC250_DEVICE* Device);
BOOLEAN ConnectorInterrupt(struct _BC250_DEVICE* Device);
void ConnectorDpc(struct _BC250_DEVICE* Device);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
// Queue the real hotplug worker; success means accepted, not rediscovery complete.
NTSTATUS ConnectorReplug(struct _BC250_DEVICE* Device, ULONG* Generation, BOOLEAN* DpcQueued);
#endif
BOOLEAN ConnectorIsEnabled(struct _BC250_DEVICE* Device);
BOOLEAN ConnectorIsConnected(struct _BC250_DEVICE* Device);
// The child reports of QueryChildRelations and QueryChildStatus. An enabled connector answers from its published
// sink. A start without a firmware display (BD-341) has a child that can see a monitor arrive and has none now.
// Otherwise the static inherited path shows the firmware's display, which is always there. PASSIVE.
BOOLEAN ConnectorChildHpdAware(struct _BC250_DEVICE* Device);
BOOLEAN ConnectorChildConnected(struct _BC250_DEVICE* Device);
// BD-341: keep and log one child event (BC250_CHILD_EVENT_*). Sense is -1 when the caller read none. PASSIVE.
void ConnectorChildRecord(struct _BC250_DEVICE* Device, ULONG Kind, BOOLEAN Connected, LONG Sense, NTSTATUS Status);
// BD-341: the kept child events, the counts and the live HPD0 sense, for the summary and the stop. PASSIVE.
void ConnectorLogChildSummary(struct _BC250_DEVICE* Device);
BOOLEAN ConnectorGenerationCurrent(struct _BC250_DEVICE* Device, ULONG Generation);
BC250_CONNECTOR_SNAPSHOT* ConnectorSnapshotAcquire(struct _BC250_DEVICE* Device);
void ConnectorSnapshotRelease(BC250_CONNECTOR_SNAPSHOT* Snapshot);
void ConnectorOperationAcquire(struct _BC250_DEVICE* Device);
void ConnectorOperationRelease(struct _BC250_DEVICE* Device);

// Root hardware owner implements these. Prepare validates only, preserving an
// inherited stream. Enable belongs to CommitVidPN after Windows selects timing.
NTSTATUS ConnectorLinkPrepare(struct _BC250_DEVICE* Device, BC250_SINK* Sink, ULONG Generation);
void ConnectorLinkLogOpen(struct _BC250_DEVICE* Device);
NTSTATUS ConnectorLinkDisable(struct _BC250_DEVICE* Device, ULONG Generation);
NTSTATUS ConnectorLinkRelease(struct _BC250_DEVICE* Device);
// STATUS_RETRY requests discovery; success consumes a healthy short HPDRX pulse.
NTSTATUS ConnectorLinkCheck(struct _BC250_DEVICE* Device, const BC250_SINK* Sink, ULONG Generation);
NTSTATUS ConnectorLinkCommit(struct _BC250_DEVICE* Device, const BC250_CONNECTOR_SNAPSHOT* Snapshot,
    const BC250_TARGET_TIMING* Timing, ULONG SourceWidth, ULONG SourceHeight,
    D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling);
NTSTATUS ConnectorLinkCurrentSignal(struct _BC250_DEVICE* Device, D3DKMDT_VIDEO_SIGNAL_INFO* Signal);
// BD-223: the scaled plane 0 of a multiplane-overlay check, in the units of BC250_DML201_INPUT (dcn_plane.h
// gives the values). PASSIVE, Connector.Operation not held. STATUS_DEVICE_NOT_READY with no active link of the
// current sink; otherwise SUCCESS, with Out->Result the return of Bc250Dml201Plan, the plan's clocks, the budget
// the clocks that run and the DCHUB reference clock, and *Plan the whole plan (dcn.c DcnPlaneCheck keeps it for
// the flip). Plan is incomplete here, so that a caller such as wddm.c does not include the DML headers: their bool
// is not the one of amdgpu.h.
typedef struct _BC250_LINK_PLANE {
    ULONG PitchPixels, ViewportWidth, ViewportHeight, RecoutWidth, RecoutHeight, HTaps, VTaps;
} BC250_LINK_PLANE;
typedef struct _BC250_LINK_PLANE_DML {
    int Result;
    ULONG Valid, DispClockKhz, DppClockKhz;
    ULONG BudgetDispKhz, BudgetDppKhz;
    ULONG DchubRefClockKhz;
} BC250_LINK_PLANE_DML;
struct BC250_DML201_PLAN;
NTSTATUS ConnectorLinkPlaneDml(struct _BC250_DEVICE* Device, const BC250_LINK_PLANE* Plane,
    BC250_LINK_PLANE_DML* Out, struct BC250_DML201_PLAN* Plan);
// CPU-only Check3 preflight against one locked current mode. Does not reserve
// pipe 1, raise its clock or take the hardware; no MMIO writes. STATUS_SUCCESS only
// when the planner describes the group (Out->Result BC250_OVERLAY_OK), STATUS_NOT_SUPPORTED
// for a refused description, STATUS_RETRY when the HPD generation moved.
NTSTATUS ConnectorLinkOverlayCheck(struct _BC250_DEVICE* Device,
    const struct BC250_OVERLAY_DESCRIPTION* Description, BC250_LINK_OVERLAY_CHECK* Out);
// PASSIVE, Connector.Operation not held. TRUE when a connected sink is published
// and its link is off and not suspended: a power-on CommitVidPn must program it
// (BD-195). *Active and *Connected are for the caller's log.
BOOLEAN ConnectorLinkNeedsPowerOn(struct _BC250_DEVICE* Device, BOOLEAN* Active, BOOLEAN* Connected);
// Both called at PASSIVE with Connector.Operation held. Resume may access
// hardware in retained power phase 3, before notifications reopen in phase 0.
NTSTATUS ConnectorLinkSuspend(struct _BC250_DEVICE* Device);
NTSTATUS ConnectorLinkResume(struct _BC250_DEVICE* Device, ULONG Generation);
// The resume that replays no timing (BD-188): the sink changed, could not be read or could not be restored. No
// hardware access; the connector worker's disable at phase 0 takes down what is lit.
void ConnectorLinkResumeChanged(struct _BC250_DEVICE* Device);
