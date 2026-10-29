// DCN201 OTG0 timing operations. The passive display owner supplies serialization,
// a blank/quiescent plane, admitted display bandwidth, and cancellation-aware I/O.
#pragma once
#include "../kmd/display_io.h"
#include "../kmd/edid.h"

typedef struct BC250_OPTC_SYNC {
    unsigned long VReady, VStartup, VUpdateOffset, VUpdateWidth;
} BC250_OPTC_SYNC;

// Which term of the admission gate refused a timing (BD-177). Several terms answer the same
// STATUS_NOT_SUPPORTED to the caller, and one log line per target is the whole ring budget, so
// the term has to be nameable. BC250_OPTC_REASON_VSTARTUP_BLANK is the one a reduced-blanking
// mode hits: the prefetch the display plan asks for does not fit in the mode's vertical blanking.
enum bc250_optc_reason {
    BC250_OPTC_REASON_OK = 0,
    BC250_OPTC_REASON_EXTENT,               // missing argument, zero or oversize active/blanking
    BC250_OPTC_REASON_FLAGS,                // not progressive digital separate sync
    BC250_OPTC_REASON_SYNC,                 // sync width or offset outside its blanking
    BC250_OPTC_REASON_TOTAL,                // a total above the OTG register field
    BC250_OPTC_REASON_VSTARTUP_BLANK,       // no prefetch, or it does not fit the vertical blanking
    BC250_OPTC_REASON_VSTARTUP_FIELD,       // prefetch above the VSTARTUP register field
    BC250_OPTC_REASON_VREADY_FIELD,
    BC250_OPTC_REASON_VUPDATE_OFFSET_FIELD,
    BC250_OPTC_REASON_VUPDATE_WIDTH_FIELD
};
// Admission only; no register access. RGB8 progressive, one OPP, no stereo/DSC.
// Reason returns enum bc250_optc_reason, Valid is the same gate as a predicate.
int Bc250OptcTimingReason(const BC250_TARGET_TIMING* Timing,const BC250_OPTC_SYNC* Sync);
int Bc250OptcTimingValid(const BC250_TARGET_TIMING* Timing,const BC250_OPTC_SYNC* Sync);
// The short name of a reason for a log line, never NULL, no allocation, any IRQL.
const char* Bc250OptcReasonName(int Reason);
// These return the FIRST I/O failure; AMD's void helpers cannot hide a failed
// register access. A failure after any write requires a full old-mode replay.
long Bc250OptcDisable(BC250_DISP_IO* Io);
long Bc250OptcProgram(BC250_DISP_IO* Io,const BC250_TARGET_TIMING* Timing,
    const BC250_OPTC_SYNC* Sync,int DisplayPort);
long Bc250OptcEnable(BC250_DISP_IO* Io);
// BD-223: the global sync of OTG0 (VREADY, VSTARTUP, VUPDATE offset and width) as the registers hold it now.
long Bc250OptcReadGlobalSync(BC250_DISP_IO* Io,BC250_OPTC_SYNC* Sync);
// BD-223: AMD optc1_program_global_sync on a running OTG0, inside the caller's acknowledged OTG0 master update lock,
// for a multiplane-overlay plane whose display plan moves VREADY or the VUPDATE width only. VSTARTUP and the VUPDATE
// offset must equal the registers: a new VSTARTUP also needs set_vtg_params, which moves VTG0_FP2
// with it (dcn10_program_pipe), and the flip does not program the VTG, and the VUPDATE offset is where the flip latches. Refused before
// any write otherwise. No wait.
long Bc250OptcGlobalSyncLocked(BC250_DISP_IO* Io,const BC250_OPTC_SYNC* Sync);
// Read-only inverse of optc1_program_timing for the admitted progressive RGB
// path. Caller supplies the measured clock and excludes concurrent modesets.
// Two identical snapshots are required; every refusal clears the output.
long Bc250OptcReadTiming(BC250_DISP_IO* Io,unsigned long PixelKhz,BC250_TARGET_TIMING* Timing);
