// DisplayPort audio, the part with no Windows in it: the checked register accessors, the step 0 observation, the
// precondition decision, the Azalia endpoint write sequences (step 1) and the stream sequences (step 2: wall DTO,
// AFMT, DP_SEC). driver/kmd/dpaudio.c drives it with BAR5 under a spin lock; driver/kmd/test/dpaudio_test.c drives
// the same file against a fake register file.
//
// Every offset and every indirect index comes from regs.generated.h (gen_regs.py over dcn_2_0_1_offset.h through
// tools/regcalc), every field mask from dcn_2_0_1_sh_mask.h. The sequences are Linux amdgpu's (drivers/gpu/drm/amd/
// display/dc, v6.18): dce/dce_audio.c dce_aud_hw_init, dce_aud_az_configure, dce_aud_az_enable, dce_aud_az_disable
// and dce_aud_wall_dto_setup, and dio/dcn10/dcn10_stream_encoder.c enc1_se_dp_audio_setup, enc1_se_dp_audio_enable,
// enc1_se_dp_audio_disable and enc1_se_audio_mute_control, with the deviations named where they are made. Plain C
// and plain integers, like scanout_admit.h, so the host test compiles the production code rather than a copy.
#pragma once
#include "bc250kmd_escape.h"

// NTSTATUS values by number, for a file that includes no WDK header. Anything below zero is a failure.
#define BC250_AZ_STATUS_SUCCESS 0L
#define BC250_AZ_STATUS_ACCESS_DENIED ((long)0xC0000022L)       // offset or index not on the generated table
#define BC250_AZ_STATUS_INVALID_PARAMETER ((long)0xC000000DL)   // endpoint or stream out of range
#define BC250_AZ_STATUS_MISMATCH ((long)0xC000009CL)            // STATUS_DEVICE_DATA_ERROR: a read-back differed

// What unit A measured under Linux (facts M819, evidence/linux/2026-10-07-L1007-dp-audio): the codec's vendor and
// device, its revision, and the pin configuration default both of its pins report. Values, not offsets.
#define BC250_DPAUDIO_CODEC_ID 0x1002AA01ul
#define BC250_DPAUDIO_CODEC_REVISION 0x00100700ul
#define BC250_DPAUDIO_CONFIG_DEFAULT 0x185600F0ul
#define BC250_DPAUDIO_ENDPOINTS 2u
#define BC250_DPAUDIO_STREAMS 2u

// Step 2's wall clock: DTO1 makes 24 MHz out of the DP reference clock, 24 MHz = refclk x PHASE / MODULE
// (get_azalia_clock_info_dp, dce_audio.c:1041-1059: phase 24 x 10 000, module = the DTO source clock in kHz x 10).
#define BC250_DPAUDIO_DTO1_PHASE 240000ul
// CLK4_0_CLK4_CLK2_CURRENT_CNT counts the DP reference clock in 100 kHz units (facts M788: 6000 = 600.000 MHz under
// Windows). A count outside these bounds is a wrong read, not a clock to build the audio wall clock on.
#define BC250_DPAUDIO_REFCLK_MIN 5000ul      // 500 MHz
#define BC250_DPAUDIO_REFCLK_MAX 7000ul      // 700 MHz

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

// The switches and the mapping, before any register is read. Returns BC250_DPAUDIO_REASON_OK or the refusal. The
// endpoint must never be visible without its stream (an endpoint with no audio clock is worse than none: no sound,
// and video players slaved to the audio clock run slow), so EnableDpAudioStream 0 refuses the whole start, like
// EnableDpAudioEndpoint 0.
unsigned long Bc250DpAudioGate(int MmioMapped, unsigned long EnableDpAudio, unsigned long EnableDpAudioEndpoint,
                               unsigned long EnableDpAudioStream);

// The preconditions of steps 1 and 2 over a step 0 observation. Returns the reason (OK or the first failed
// precondition) and fills the plan: the stream encoder that drives the monitor, the Azalia endpoint that belongs to
// it, the DTO1 module of the stream half, and notes.
typedef struct _BC250_DPAUDIO_PLAN {
    unsigned long Reason;
    unsigned long Stream;                   // DPn whose video stream is on
    unsigned long Endpoint;                 // = Stream: the audio instance is the stream encoder's engine id
    unsigned long Notes;                    // BC250_DPAUDIO_NOTE_*
    unsigned long RefClock;                 // CLK4_0_CLK4_CLK2_CURRENT_CNT, 100 kHz units
    unsigned long DtoModule;                // RefClock in kHz x 10: the DTO1 module for a 24 MHz wall clock
} BC250_DPAUDIO_PLAN;
unsigned long Bc250DpAudioDecide(const BC250_DPAUDIO_OBSERVATION* Obs, BC250_DPAUDIO_PLAN* Plan);

// Step 1's write groups. Each returns 0 or the failing access's status.
typedef struct _BC250_DPAUDIO_RESULT {
    unsigned long HotPlugBefore, HotPlugAfter;      // HOT_PLUG_CONTROL of the endpoint the group worked on
    unsigned long SizeRates, PowerStates;           // Bc250DpAudioHwInit: read back after the writes
    unsigned long Writes;                           // register writes this group performed
} BC250_DPAUDIO_RESULT;
long Bc250DpAudioHwInit(BC250_AZ_IO* Io, BC250_DPAUDIO_RESULT* Result);                       // endpoint 0
long Bc250DpAudioConfigure(BC250_AZ_IO* Io, unsigned long Endpoint, BC250_DPAUDIO_RESULT* Result);
long Bc250DpAudioSetEnabled(BC250_AZ_IO* Io, unsigned long Endpoint, int Enable, BC250_DPAUDIO_RESULT* Result);

// Step 2's two sequences on stream encoder Stream (DIGn, DPn). Every write is a read-modify-write of named fields
// only, read back at once; a read-back whose named bits differ ends the enable with BC250_AZ_STATUS_MISMATCH. The
// enable stops at the first failure; the disable runs every step whatever fails and keeps the first failure. Both
// return 0 or that failure's status, which Step, Status and the Mismatch fields describe.
typedef struct _BC250_DPAUDIO_STREAM_RESULT {
    unsigned long Step;                     // enum bc250_dpaudio_step: the first step that failed (NONE when none)
    long Status;                            // that step's status
    unsigned long MismatchOffset, MismatchExpected, MismatchActual;  // the named bits written and read back
    unsigned long Writes;                   // register writes performed
    // Read back by the sequence after its last write of each register; 0 when the sequence did not reach it.
    unsigned long DtoSource, DtoModule, DtoPhase, AfmtCntl, SrcControl, PacketControl, PacketControl2, SecCntl;
    unsigned long AudN, Timestamp;
} BC250_DPAUDIO_STREAM_RESULT;
long Bc250DpAudioStreamEnable(BC250_AZ_IO* Io, unsigned long Stream, unsigned long Endpoint, unsigned long DtoModule,
                              BC250_DPAUDIO_STREAM_RESULT* Result);
long Bc250DpAudioStreamDisable(BC250_AZ_IO* Io, unsigned long Stream, BC250_DPAUDIO_STREAM_RESULT* Result);

// The stop sequence: the stream off (when StreamWritten), then AUDIO_ENABLED 0 on the endpoint (when
// EndpointWritten). Linux's order at a stream disable (disable_dio_audio_packet, then az_disable). Both halves run
// whatever the other did; returns the first failure.
typedef struct _BC250_DPAUDIO_STOP {
    BC250_DPAUDIO_STREAM_RESULT Stream;
    BC250_DPAUDIO_RESULT Endpoint;
    long StreamStatus, EndpointStatus;
} BC250_DPAUDIO_STOP;
long Bc250DpAudioStopSequence(BC250_AZ_IO* Io, unsigned long Stream, unsigned long Endpoint, int StreamWritten,
                              int EndpointWritten, BC250_DPAUDIO_STOP* Stop);

// A whole start after the gate: observe, decide, and only on OK the four write groups in order: hw_init, configure
// (step 1 without its last write), the stream (step 2), and AUDIO_ENABLED last. A failure in any group ends the
// groups and the stop sequence runs: the stream off if its group began, and AUDIO_ENABLED 0 on the chosen endpoint.
// Returns the reason: OK, the refusal (nothing written beyond the INDEX selects of the reads), STREAM_MISMATCH or
// WRITE_FAILED. dpaudio.c runs it under its lock; the host test runs it against a fake register file.
typedef struct _BC250_DPAUDIO_RUN {
    BC250_DPAUDIO_OBSERVATION Obs;
    BC250_DPAUDIO_PLAN Plan;
    BC250_DPAUDIO_RESULT Init, Config, Enable;
    BC250_DPAUDIO_STREAM_RESULT Stream;
    BC250_DPAUDIO_STOP Undo;                // the stop sequence after a failure
    unsigned long Groups;                   // groups completed: 1 hw_init, 2 configure, 3 stream, 4 AUDIO_ENABLED
    unsigned long Wrote;                    // a write was attempted: the stop path owes the endpoint AUDIO_ENABLED 0
    unsigned long StreamWrote;              // the stream group began
    long Status, UndoStatus;                // the failing write's status; the stop sequence's status
} BC250_DPAUDIO_RUN;
unsigned long Bc250DpAudioRun(BC250_AZ_IO* Io, BC250_DPAUDIO_RUN* Run);

const char* Bc250DpAudioReasonText(unsigned long Reason);
const char* Bc250DpAudioStepText(unsigned long Step);
