// DisplayPort audio, the part with no Windows in it: the checked register accessors, the step 0 observation, the
// step 1 precondition decision and the Azalia endpoint write sequences. driver/kmd/dpaudio.c drives it with BAR5
// under a spin lock; driver/kmd/test/dpaudio_test.c drives the same file against a fake register file.
//
// Every offset and every indirect index comes from regs.generated.h (gen_regs.py over dcn_2_0_1_offset.h through
// tools/regcalc), every field mask from dcn_2_0_1_sh_mask.h. The sequences are Linux amdgpu's (drivers/gpu/drm/amd/
// display/dc/dce/dce_audio.c, v6.18): dce_aud_hw_init, dce_aud_az_configure, dce_aud_az_enable, dce_aud_az_disable,
// with the deviations named where they are made. Plain C and plain integers, like scanout_admit.h, so the host
// test compiles the production code rather than a copy.
#pragma once
#include "bc250kmd_escape.h"

// NTSTATUS values by number, for a file that includes no WDK header. Anything below zero is a failure.
#define BC250_AZ_STATUS_SUCCESS 0L
#define BC250_AZ_STATUS_ACCESS_DENIED ((long)0xC0000022L)       // offset or index not on the generated table
#define BC250_AZ_STATUS_INVALID_PARAMETER ((long)0xC000000DL)   // endpoint out of range

// What unit A measured under Linux (facts M819, evidence/linux/2026-10-07-L1007-dp-audio): the codec's vendor and
// device, its revision, and the pin configuration default both of its pins report. Values, not offsets.
#define BC250_DPAUDIO_CODEC_ID 0x1002AA01ul
#define BC250_DPAUDIO_CODEC_REVISION 0x00100700ul
#define BC250_DPAUDIO_CONFIG_DEFAULT 0x185600F0ul
#define BC250_DPAUDIO_ENDPOINTS 2u

// One register access. Offset is a BAR5 byte offset that the caller has already checked against the audio tables;
// the callback only performs it. Returns 0 or a negative NTSTATUS.
typedef long (*BC250_AZ_READ_FN)(void* Context, unsigned long Offset, unsigned long* Value);
typedef long (*BC250_AZ_WRITE_FN)(void* Context, unsigned long Offset, unsigned long Value);

typedef struct _BC250_AZ_IO {
    void* Context;
    BC250_AZ_READ_FN Read;
    BC250_AZ_WRITE_FN Write;
    unsigned long IndirectReads, IndirectWrites, DirectWrites, Refusals;     // counted by the checked accessors
} BC250_AZ_IO;

// The checked accessors: g_MmioAudioAllow / g_MmioAudioWriteAllow for direct offsets, g_AzIxReadAllow /
// g_AzIxWriteAllow for indirect indices. Nothing reaches a register past them.
int Bc250AzReadAllowed(unsigned long Offset);
int Bc250AzWriteAllowed(unsigned long Offset);
long Bc250AzRead(BC250_AZ_IO* Io, unsigned long Offset, unsigned long* Value);
long Bc250AzWrite(BC250_AZ_IO* Io, unsigned long Offset, unsigned long Value);
long Bc250AzIndirectRead(BC250_AZ_IO* Io, unsigned long Endpoint, unsigned long Index, unsigned long* Value);
long Bc250AzIndirectWrite(BC250_AZ_IO* Io, unsigned long Endpoint, unsigned long Index, unsigned long Value);

// Step 0: every slot of BC250_DPAUDIO_OBS_LIST, reads only. A failed read leaves its slot 0 and its ValidMask bit
// clear; FirstFailure keeps the first failure's status.
typedef struct _BC250_DPAUDIO_OBSERVATION {
    unsigned long Regs[BC250_DPAUDIO_OBS_SLOTS];
    unsigned long long ValidMask;
    long FirstFailure;
} BC250_DPAUDIO_OBSERVATION;
void Bc250DpAudioObserve(BC250_AZ_IO* Io, BC250_DPAUDIO_OBSERVATION* Obs);

// The switches and the mapping, before any register is read. Returns BC250_DPAUDIO_REASON_OK or the refusal.
unsigned long Bc250DpAudioGate(int MmioMapped, unsigned long EnableDpAudio, unsigned long EnableDpAudioEndpoint);

// Step 1's preconditions over a step 0 observation. Returns the reason (OK or the first failed precondition) and
// fills the plan: the stream encoder that drives the monitor, the Azalia endpoint that belongs to it, and notes.
typedef struct _BC250_DPAUDIO_PLAN {
    unsigned long Reason;
    unsigned long Stream;                   // DPn whose video stream is on
    unsigned long Endpoint;                 // = Stream: the audio instance is the stream encoder's engine id
    unsigned long Notes;                    // BC250_DPAUDIO_NOTE_*
} BC250_DPAUDIO_PLAN;
unsigned long Bc250DpAudioDecide(const BC250_DPAUDIO_OBSERVATION* Obs, BC250_DPAUDIO_PLAN* Plan);

// Step 1's write groups, in the order Bc250DpAudioRun runs them. Each returns 0 or the failing access's status.
typedef struct _BC250_DPAUDIO_RESULT {
    unsigned long HotPlugBefore, HotPlugAfter;      // HOT_PLUG_CONTROL of the endpoint the group worked on
    unsigned long SizeRates, PowerStates;           // Bc250DpAudioHwInit: read back after the writes
    unsigned long Writes;                           // register writes this group performed
} BC250_DPAUDIO_RESULT;
long Bc250DpAudioHwInit(BC250_AZ_IO* Io, BC250_DPAUDIO_RESULT* Result);                       // endpoint 0
long Bc250DpAudioConfigure(BC250_AZ_IO* Io, unsigned long Endpoint, BC250_DPAUDIO_RESULT* Result);
long Bc250DpAudioSetEnabled(BC250_AZ_IO* Io, unsigned long Endpoint, int Enable, BC250_DPAUDIO_RESULT* Result);

// A whole start after the gate: observe, decide, and only on OK the three write groups in order (hw_init,
// configure, enable). A failed write ends the groups, and the disable sequence runs on the chosen endpoint so that
// AUDIO_ENABLED is left 0. Returns the reason: OK, the refusal (nothing written beyond the INDEX selects of the
// reads), or WRITE_FAILED. dpaudio.c runs it under its lock; the host test runs it against a fake register file.
typedef struct _BC250_DPAUDIO_RUN {
    BC250_DPAUDIO_OBSERVATION Obs;
    BC250_DPAUDIO_PLAN Plan;
    BC250_DPAUDIO_RESULT Init, Config, Enable, Clear;
    unsigned long Groups;                   // write groups completed: 1 hw_init, 2 configure, 3 enable
    unsigned long Wrote;                    // a write was attempted: the stop path owes the endpoint AUDIO_ENABLED 0
    long Status, ClearStatus;               // the failing write's status; the disable sequence's status
} BC250_DPAUDIO_RUN;
unsigned long Bc250DpAudioRun(BC250_AZ_IO* Io, BC250_DPAUDIO_RUN* Run);

const char* Bc250DpAudioReasonText(unsigned long Reason);
