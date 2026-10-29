// Windows lifetime owner for the single connector. Included after WDK headers.
#pragma once
#include "../shim/bc250_sink.h"
#include "../shim/bc250_hpd.h"
#include "connector_diag.h"
#include "connector_handoff.h"

struct _BC250_DEVICE;
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
    // BD-195: the generation whose disable answered BUSY, and its worker re-arms
    // (at most CONNECTOR_BUSY_REARMS). Operation held.
    ULONG BusyGeneration, BusyRearms;
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
// current sink, STATUS_INSUFFICIENT_RESOURCES with no memory for the plan; otherwise SUCCESS, with Out->Result the
// return of Bc250Dml201Plan, the plan's clocks and the budget the clocks that run. The plan itself stays inside
// connector_link.c, so that a caller does not include the DML headers: their bool is not the one of amdgpu.h.
typedef struct _BC250_LINK_PLANE {
    ULONG PitchPixels, ViewportWidth, ViewportHeight, RecoutWidth, RecoutHeight, HTaps, VTaps;
} BC250_LINK_PLANE;
typedef struct _BC250_LINK_PLANE_DML {
    int Result;
    ULONG Valid, DispClockKhz, DppClockKhz;
    ULONG BudgetDispKhz, BudgetDppKhz;
} BC250_LINK_PLANE_DML;
NTSTATUS ConnectorLinkPlaneDml(struct _BC250_DEVICE* Device, const BC250_LINK_PLANE* Plane,
    BC250_LINK_PLANE_DML* Out);
// PASSIVE, Connector.Operation not held. TRUE when a connected sink is published
// and its link is off and not suspended: a power-on CommitVidPn must program it
// (BD-195). *Active and *Connected are for the caller's log.
BOOLEAN ConnectorLinkNeedsPowerOn(struct _BC250_DEVICE* Device, BOOLEAN* Active, BOOLEAN* Connected);
// Both called at PASSIVE with Connector.Operation held. Resume may access
// hardware in retained power phase 3, before notifications reopen in phase 0.
NTSTATUS ConnectorLinkSuspend(struct _BC250_DEVICE* Device);
NTSTATUS ConnectorLinkResume(struct _BC250_DEVICE* Device, ULONG Generation);
