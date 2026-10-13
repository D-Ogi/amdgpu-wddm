// Scalar diagnostics retained after a failed cold start and after link release.
#pragma once
#include "../shim/bc250_atom.h"
#include "../shim/bc250_hdmi_link.h"

enum BC250_CONNECTOR_STEP {
    BC250_LINK_IDLE, BC250_LINK_PLAN, BC250_LINK_SURFACE_CAPTURE,
    BC250_LINK_AUX_CLOSE, BC250_LINK_PLANE_BLANK, BC250_LINK_VISIBILITY,
    BC250_LINK_STREAM_BLANK, BC250_LINK_HDMI_RESTORE, BC250_LINK_TX_DISABLE,
    BC250_LINK_OTG_DISABLE, BC250_LINK_CLOCK_READ, BC250_LINK_PIPE,
    BC250_LINK_PIXEL_CLOCK, BC250_LINK_TIMING, BC250_LINK_OUTPUT,
    BC250_LINK_SCALER, BC250_LINK_ROUTE, BC250_LINK_STREAM,
    BC250_LINK_AUX_OPEN, BC250_LINK_TRAIN, BC250_LINK_PHY, BC250_LINK_TX_ENABLE,
    BC250_LINK_OTG_ENABLE, BC250_LINK_SURFACE_APPLY, BC250_LINK_UNBLANK,
    BC250_LINK_HDMI_MUTE, BC250_LINK_GENERATION, BC250_LINK_AUDIO,
    BC250_LINK_INHERITED_PLAN, BC250_LINK_HDMI_HANDOFF
};

typedef struct BC250_CONNECTOR_TARGET_DIAG {
    ULONG Recorded, Attempt, Generation, Attached, Protocol, Scaling;
    ULONG SourceWidth, SourceHeight, Width, Height, HTotal, VTotal, PixelKhz;
    ULONGLONG RefreshNumerator, RefreshDenominator;
} BC250_CONNECTOR_TARGET_DIAG;

// A value-policy decision, not a readback or proof of hardware completion.
// Effective is meaningful only when Allowed is nonzero; Before is a real sample.
typedef struct BC250_CONNECTOR_ATOM_WRITE_DIAG {
    ULONG Valid, Allowed, Reason, Register, Before, Requested, Effective;
} BC250_CONNECTOR_ATOM_WRITE_DIAG;

typedef struct BC250_CONNECTOR_FAILURE_DIAG {
    ULONG Recorded, Phase, Step, AtomValid, IoOffset, IoReads, IoWrites, IoAccess;
    NTSTATUS Status, IoStatus;
    ULONG LockStatus, LockStatusValid;
    BC250_ATOM_RESULT Atom;
    BC250_CONNECTOR_ATOM_WRITE_DIAG AtomWrite;
    BC250_HDMI_RESTORE_DETAIL HdmiRestore;
    BC250_HDMI_RESTORE_DETAIL HdmiStream;
    ULONG HdmiStage;
    BC250_CONNECTOR_TARGET_DIAG Target;
} BC250_CONNECTOR_FAILURE_DIAG;

// The first attempted recovery is separate from an earlier admission refusal.
// Original and Failure are paired by Attempt even if recovery was unavailable.
typedef struct BC250_CONNECTOR_RECOVERY_DIAG {
    ULONG Recorded, Attempt, PreviousValid, GenerationCurrent, Active, ReplayAttempted;
    NTSTATUS Status;
    BC250_CONNECTOR_TARGET_DIAG PreviousTarget;
    BC250_CONNECTOR_FAILURE_DIAG Original, Failure;
} BC250_CONNECTOR_RECOVERY_DIAG;

enum BC250_CONNECTOR_OPEN_STEP {
    BC250_OPEN_READY = 0,
    BC250_OPEN_FRAMEBUFFER,
    BC250_OPEN_INHERITED_PIPE,
    BC250_OPEN_SIGNAL,
    BC250_OPEN_CONTEXT_POOL,
    BC250_OPEN_ATOM_POOL,
    BC250_OPEN_DML_POOL,
    BC250_OPEN_FIRMWARE,
    BC250_OPEN_IDENTITY,
    BC250_OPEN_ATOM_VALIDATE,
    BC250_OPEN_REFERENCE_CLOCK
};

typedef struct BC250_CONNECTOR_OPEN_DIAG {
    ULONG Recorded, Generation, Step, AtomReason, Readiness;
    NTSTATUS Status, PipeStatus;
    ULONG Support, PipeValid, Bottom, Control;
} BC250_CONNECTOR_OPEN_DIAG;
