// BD-090: what the post-display release found on the link, and what it did about it (connector_link.c's
// ConnectorLinkHandoff, pnp.c's Bc250StopDeviceAndReleasePostDisplayOwnership). Included after WDK headers.
#pragma once
struct _BC250_DEVICE;

typedef enum _BC250_LINK_HANDOFF_ACTION {
    BC250_LINK_HANDOFF_STATIC,      // no connector owner: the static inherited route keeps the firmware stream
    BC250_LINK_HANDOFF_ACTIVE,      // the link is on; the stop restores the surface that it scans
    BC250_LINK_HANDOFF_NO_SINK,     // no display is connected
    BC250_LINK_HANDOFF_SUSPENDED,   // the connector is suspended or has no link owner; nothing is programmed
    BC250_LINK_HANDOFF_NO_TARGET,   // the link is off, and the sink offers no timing of at least 800x600
    BC250_LINK_HANDOFF_ENABLED,     // the link was off; a target now scans the reserved buffer, black
    BC250_LINK_HANDOFF_FAILED,      // the link was off; the enable of a target failed
    BC250_LINK_HANDOFF_ACTIONS
} BC250_LINK_HANDOFF_ACTION;

typedef struct _BC250_LINK_HANDOFF {
    ULONG Action;                   // BC250_LINK_HANDOFF_ACTION
    ULONG Detached;                 // 1: the target is the mode that an OS detach of this same sink took down
    ULONG Width, Height, PixelKhz;  // the target enabled or tried (ENABLED, FAILED), else 0
    NTSTATUS Status;
} BC250_LINK_HANDOFF;

// PASSIVE. Connector.Operation not held; the caller may hold StartHealth (the order CommitVidPn uses). Programs
// hardware only for ENABLED and FAILED, through ConnectorLinkCommit.
NTSTATUS ConnectorLinkHandoff(struct _BC250_DEVICE* Device, BC250_LINK_HANDOFF* Handoff);
