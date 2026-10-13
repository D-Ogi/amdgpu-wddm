/* PnP-owned HDMI takeover. No escape and no work item can execute ATOM. */
#pragma once
#include "../shim/bc250_hdmi_link.h"
struct _BC250_DEVICE;
void HdmiLinkStartAudio(struct _BC250_DEVICE *Device, BOOLEAN Resume);
BOOLEAN HdmiLinkStop(struct _BC250_DEVICE *Device);
// PASSIVE_LEVEL, with the WDDM primary transaction already held by the caller.
BOOLEAN HdmiLinkStopOwned(struct _BC250_DEVICE *Device);
// Caller serializes the context against stop/release; no hardware access.
void HdmiLinkGetRestoreFailure(struct _BC250_DEVICE *Device, BC250_HDMI_RESTORE_DETAIL* Out);
NTSTATUS HdmiLinkRelease(struct _BC250_DEVICE *Device);
NTSTATUS DisplayVerifyInheritedTiming(struct _BC250_DEVICE *Device);
