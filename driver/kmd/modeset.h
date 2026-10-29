// Display modes, stage A, as the miniport keeps them (modeset.c): the monitor's EDID read once at start, the
// monitor descriptor dxgkrnl asks for, the list of source modes display.c offers, and the source size and scaling
// that pipe 0 shows now. The OTG timing never changes: every mode is scanned out at the timing the firmware lit,
// through the DCN scaler (dcn_scale.c). docs/design/display-modes.md has the design. Included by bc250kmd.h after
// the WDK headers.
#pragma once
#include "../shim/bc250_ddc.h"
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
    BC250_INHERITED_PIPE InheritedPipe;     // read before any TMDS exit, even with display modes off
    LONG InheritedPipeStatus;
    ULONG InheritedSupport;                // transformations equivalent to the saved source/target tuple
    ULONG InheritedGate;                   // enum bc250_pipe_gate: the first check that refused that pipe
    ULONG InheritedGateWord;               // the word that check read (BD-169: the start record carries both)
    BOOLEAN InheritedFixed;                // scaler/timing retained; only the GOP source is offered
    volatile LONG InheritedSupportCalls, InheritedCofuncCalls, InheritedCommitCalls; // first 16 calls per start
    LONG AuxStatus;                         // the AUX session's result (0 = the EDID was read)
    ULONG EdidReason;                       // enum bc250_edid_reason
    BOOLEAN EdidValid;                      // the base block passed its checks
    BOOLEAN DescriptorServed;               // QueryDeviceDescriptor answers with the EDID
    BOOLEAN DpcdValid;
    BOOLEAN PipeChanged;                    // pipe 0 is not in the firmware's shape: stop, reset and D3 owe the restore
    BOOLEAN Suspended;                      // D3: the shape went back to native; D0 programs the committed one again
    ULONG EdidLength;
    BC250_DUAL_MODE_ADAPTER Adapter;
    UCHAR Edid[BC250_EDID_MAX_BYTES];
    UCHAR Dpcd[BC250_MODESET_DPCD_BYTES];
    UCHAR DownstreamPort[16];               // GitHub #45 review: DPCD 0x080-0x08F of a branch device (DownstreamRead)
    BOOLEAN DownstreamRead;
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
NTSTATUS ModesetPrepareInherited(struct _BC250_DEVICE* Device, const D3DKMDT_VIDEO_SIGNAL_INFO* Signal);
NTSTATUS ModesetValidateInherited(struct _BC250_DEVICE* Device);        // before D0 display access or VSync re-arm
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
