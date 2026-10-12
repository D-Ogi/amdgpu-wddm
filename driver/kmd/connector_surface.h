// Immutable scanout identity saved under the WDDM primary transaction.
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
