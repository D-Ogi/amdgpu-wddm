// Display modes, stage A, as the miniport keeps them (modeset.c): the monitor's EDID read once at start, the
// monitor descriptor dxgkrnl asks for, the list of source modes display.c offers, and the source size and scaling
// that pipe 0 shows now. The OTG timing never changes: every mode is scanned out at the timing the firmware lit,
// through the DCN scaler (dcn_scale.c). docs/design/display-modes.md has the design. Included by bc250kmd.h after
// the WDK headers.
#pragma once
#include "edid.h"
#include "dcn_scale.h"

#define BC250_MODESET_DPCD_BYTES 16ul       // DPCD 0x000-0x00F: the receiver capability fields, for the log

typedef struct _BC250_MODESET {
    // Serializes the PASSIVE_LEVEL owners: start, commit, stop and the power transitions. The quiet restore at
    // bugcheck and reset takes nothing (it runs at high IRQL on the way down).
    FAST_MUTEX Lock;
    ULONG Requested;                        // EnableDisplayModes as this start read it
    ULONG Level;                            // what this start offers: BC250_DISPLAY_MODES_*
    ULONG PipeVerdict;                      // enum bc250_pipe_verdict of the pipe the firmware lit
    LONG AuxStatus;                         // the AUX session's result (0 = the EDID was read)
    ULONG EdidReason;                       // enum bc250_edid_reason
    BOOLEAN EdidValid;                      // the base block passed its checks
    BOOLEAN DescriptorServed;               // QueryDeviceDescriptor answers with the EDID
    BOOLEAN DpcdValid;
    BOOLEAN PipeChanged;                    // pipe 0 is not in the firmware's shape: stop, reset and D3 owe the restore
    BOOLEAN Suspended;                      // D3: the shape went back to native; D0 programs the committed one again
    ULONG EdidLength;
    UCHAR Edid[BC250_EDID_MAX_BYTES];
    UCHAR Dpcd[BC250_MODESET_DPCD_BYTES];
    BC250_EDID_INFO Info;
    BC250_MODE_LIST Modes;
    // The committed source size. Post's until a commit changes it. Written only while wddm.c's primary transaction
    // is held (WddmPrimaryExclusiveBegin), so the flip DDI, which holds that transaction itself, reads a stable pair.
    volatile LONG SourceWidth, SourceHeight;
    ULONG Scaling;                          // enum bc250_scaling of the committed plan
    BC250_SCALER_PLAN Plan;                 // the committed plan (valid while PipeChanged)
    BC250_SCALER_RESULT LastResult;
    LONG LastStatus;
    ULONG AuxStallUs;                       // the stall time the EDID read spent
    ULONG Commits, Changes, Refusals, Restores, Underflows, Busy;
} BC250_MODESET;

struct _BC250_DEVICE;
void ModesetInitialize(struct _BC250_DEVICE* Device);                   // AddDevice
void ModesetStart(struct _BC250_DEVICE* Device);                        // StartDevice, after the inherited timing
// CommitVidPn of a source mode Width x Height with a VidPN scaling (enum bc250_scaling; IDENTITY for the native
// size). Returns STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE for a size or scaling this start does not offer.
NTSTATUS ModesetCommit(struct _BC250_DEVICE* Device, ULONG Width, ULONG Height, ULONG Scaling);
void ModesetStop(struct _BC250_DEVICE* Device);                         // DcnStop, after the firmware surface is back
void ModesetRestoreQuiet(struct _BC250_DEVICE* Device);                 // bugcheck and reset: any IRQL, no log, no lock
void ModesetPowerDown(struct _BC250_DEVICE* Device);                    // before D3
void ModesetPowerUp(struct _BC250_DEVICE* Device);                      // back in D0
NTSTATUS ModesetQueryDescriptor(struct _BC250_DEVICE* Device, PDXGK_DEVICE_DESCRIPTOR Descriptor);
// The parsed EDID for DP audio step 4, or NULL when this start has none (dpaudio.c then programs the fixed set).
const BC250_EDID_INFO* ModesetEdidForAudio(const struct _BC250_DEVICE* Device);
// The scalings (BC250_SCALING_BIT()s) a source mode of Width x Height gets on this start; 0 when it is not offered.
ULONG ModesetScalingSupport(const struct _BC250_DEVICE* Device, ULONG Width, ULONG Height);
