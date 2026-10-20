// Scanout metadata read under the WDDM primary transaction. Rollback uses
// reserved driver storage; a captured application address conveys no ownership.
#pragma once
struct _BC250_DEVICE;
typedef struct BC250_LINK_SURFACE {
    unsigned long long Physical;
    unsigned long Pitch, Format, Width, Height, Visible;
} BC250_LINK_SURFACE;
NTSTATUS DcnLinkSurfaceCapture(struct _BC250_DEVICE* Device, unsigned long Width,
    unsigned long Height, BC250_LINK_SURFACE* Surface);
NTSTATUS DcnLinkSurfaceRestore(struct _BC250_DEVICE* Device,
    const BC250_LINK_SURFACE* Surface);
// Inside a link writer, right after its Begin: RuntimePost describes the
// reserved bridge before RuntimeDisplayChanged is set (BD-195, I3). State only.
void DcnLinkSurfaceAdopt(struct _BC250_DEVICE* Device);
