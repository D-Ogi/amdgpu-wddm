#ifndef BC250_SCANOUT_ADMIT_H
#define BC250_SCANOUT_ADMIT_H
#include "dcn_translate.h"
#include "surface_format.h"

// M15.14: which allocation SetVidPnSourceAddress may hand to the display pipeline.
//
// Until 0.7.205.1 the answer was a single clause in wddm.c - "not an application allocation" - and it
// was the whole reason an application's own swap-chain buffer could never scan out. The clause was
// right about the risk and wrong about the rule: what must not reach HUBP0 is a surface whose address,
// geometry, pitch, format or segment the driver cannot vouch for, and provenance is only one way of
// not vouching for it. This header is that rule, written out, so that the dangerous edit in the driver
// is one call to a function the host test can drive through every refusal (test/scanout_admit_test.c,
// gate "scanout-admit").
//
// What this is NOT: permission to program the plane. dcn.c's AddressAllowed still holds the address
// itself against the firmware's own plane and the VRAM carve-out, and it is unchanged by this work.
// This decides which allocation is a candidate; that decides which address may be written. Two
// independent refusals in series, on purpose, because the cost of being wrong once is a wrong address
// in HUBP0 on a part with no working GPU reset (facts M53).
//
// Plain integers and no WDK header, like gdi_admission.h, so the host test compiles the production
// rule rather than a copy of it.
//
// A candidate's provenance and intent:
//   UmdAlloc          the object arrived as a BC2A blob (an application allocation), not an LB7A one.
//   ScanoutRequested  its creator asked for scan-out and described the surface: the BC2A
//                     UMD_BLOB_A_SCANOUT flag with its v3 geometry, or an LB7A type-0 surface whose
//                     E26R record carries BC250_SURFACE_RESOURCE_SCANOUT. The kernel driver recorded
//                     the description at CreateAllocation and placed the allocation in the local
//                     segment for it; here it only re-derives whether that description can be scanned
//                     out at all.
// An object that asked for neither keeps exactly the checks it had before this header existed, and is
// admitted by exactly the same four of them. That class is dxgkrnl's own shared primary, which DWM
// flips today, and it is also every LB7A primary of our own shells that did not set the E26R scan-out
// bit: those are aperture-resident, so the 4 KiB and segment clauses below would be wrong for them and
// dcn.c refuses their address instead (DcnTranslateCardAddress on an aperture address). Nothing in user
// mode may be told a flip of such a surface is supported - the D3D11 shell's CheckDirectFlipSupport
// therefore answers only for a surface that did set the bit.
#define BC250_SCANOUT_ADMIT_OK 0
#define BC250_SCANOUT_NO_ALLOCATION 1   // the handle did not resolve to an allocation of this adapter
#define BC250_SCANOUT_NOT_REQUESTED 2   // an application allocation that never asked to be scanned out
#define BC250_SCANOUT_FORMAT 3          // not a SCANOUT_PRIMARY row of the shared format table
#define BC250_SCANOUT_GEOMETRY 4        // not the POST mode's width and height
#define BC250_SCANOUT_PITCH 5           // no row layout, or a pitch the plane cannot express
#define BC250_SCANOUT_SIZE 6            // the rows do not fit in the allocation
#define BC250_SCANOUT_SEGMENT 7         // not resident in the segment whose descriptor says DirectFlip
#define BC250_SCANOUT_ALIGNMENT 8       // the address is not 4 KiB aligned
#define BC250_SCANOUT_GATED 9           // the operator closed the EnableScanoutAdmit gate for this start
#define BC250_SCANOUT_STATUSES 10

typedef struct _BC250_SCANOUT_CANDIDATE {
    int UmdAlloc;                       // the object came in as a BC2A blob
    int ScanoutRequested;               // its creator asked for scan-out and described the surface
    unsigned long Width, Height, Pitch, Format;  // the description: Format is a D3DDDIFORMAT
    unsigned long long Size;            // the allocation's own byte count
    unsigned long Segment;              // DXGKARG_SETVIDPNSOURCEADDRESS.PrimarySegment
    unsigned long long Address;         // DXGKARG_SETVIDPNSOURCEADDRESS.PrimaryAddress
} BC250_SCANOUT_CANDIDATE;

// *Bytes and *Pitch are written on BC250_SCANOUT_ADMIT_OK only; a refusal leaves both alone, so a
// caller that ignores the status cannot accidentally reprogram the plane with a refused layout.
static __inline int Bc250ScanoutAdmit(const BC250_SCANOUT_CANDIDATE* Candidate,
    unsigned long PostWidth, unsigned long PostHeight, unsigned long FlipSegment,
    unsigned long* Pitch, unsigned long long* Bytes)
{
    unsigned long long bytes;
    if (!Candidate || !Pitch || !Bytes) return BC250_SCANOUT_NO_ALLOCATION;
    if (Candidate->UmdAlloc && !Candidate->ScanoutRequested) return BC250_SCANOUT_NOT_REQUESTED;
    // The plane programs an address and a pitch, never a pixel format (dcn.c), so the format decides
    // nothing at the hardware and everything about what the monitor shows: only a row the shared table
    // enables for SCANOUT_PRIMARY is the format the firmware left the plane in.
    if (WddmSurfaceFormatBpp(Candidate->Format, BC250_SURFACE_SCANOUT) != 4) return BC250_SCANOUT_FORMAT;
    // One VidPN source mode exists, the inherited POST one (display.c), so a surface of any other
    // geometry would be scanned out at the wrong stride or past the end of the buffer.
    if (Candidate->Width != PostWidth || Candidate->Height != PostHeight) return BC250_SCANOUT_GEOMETRY;
    if (!DcnSurfaceBytes(Candidate->Width, Candidate->Height, Candidate->Pitch, &bytes))
        return BC250_SCANOUT_PITCH;
    if (bytes > Candidate->Size) return BC250_SCANOUT_SIZE;
    if (Candidate->ScanoutRequested) {
        // Everything from here is new and applies to application surfaces alone. dxgkrnl's own shared
        // primary is admitted by the four checks above, exactly as it was before M15.14. A pitch that
        // is not a whole number of 4-byte pixels needs no clause here: DcnSurfaceBytes already refuses
        // it, which is what keeps HUBPREQ0_DCSURF_SURFACE_PITCH (pitch/4 - 1) exact.
        //
        // The plane's address field is a 4 KiB page number on this generation; VidMm gives an
        // application allocation page granularity anyway, so a misaligned address means the candidate
        // is not the allocation's base and nothing here describes what the plane would read.
        if (Candidate->Address & 0xFFFull) return BC250_SCANOUT_ALIGNMENT;
        // Residency. Segment 1's descriptor is the only one with Flags.DirectFlip (wddm.c); the
        // aperture is system memory reached through the GART, which the display core does not read.
        // PrimarySegment is dxgkrnl's own answer to where the allocation is, not a hope of the UMD's.
        if (!FlipSegment || Candidate->Segment != FlipSegment) return BC250_SCANOUT_SEGMENT;
    }
    *Pitch = Candidate->Pitch;
    *Bytes = bytes;
    return BC250_SCANOUT_ADMIT_OK;
}

// BC250_SCANOUT_GATED is the one status this header does not decide: wddm.c answers it for a requesting
// candidate before it asks, because the gate is a start-time registry value and not a property of the
// surface. It has a status of its own all the same, so that the counters and the refusal line say "the
// operator turned this off" instead of naming a clause that never ran.
static const char* const g_Bc250ScanoutStatusNames[BC250_SCANOUT_STATUSES] = {
    "ok", "no-allocation", "not-requested", "format", "geometry", "pitch", "size", "segment", "alignment",
    "gated" };

static __inline const char* Bc250ScanoutStatusText(int Status)
{
    return Status >= 0 && Status < BC250_SCANOUT_STATUSES ? g_Bc250ScanoutStatusNames[Status] : "unknown";
}
#endif
